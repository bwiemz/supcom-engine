#include "sim/reliable_udp.hpp"

#include "sim/socket_platform.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <random>

namespace osc::sim {

using net::kInvalidSocket;
using net::socket_t;

// ── Addresses ─────────────────────────────────────────────────────────────

std::string UdpAddress::text() const {
    return std::to_string((ip >> 24) & 0xFF) + "." + std::to_string((ip >> 16) & 0xFF) + "." +
           std::to_string((ip >> 8) & 0xFF) + "." + std::to_string(ip & 0xFF) + ":" +
           std::to_string(port);
}

std::optional<UdpAddress> UdpAddress::parse(const std::string& host, u16 port) {
    net::startup();
    in_addr a{};
    if (inet_pton(AF_INET, host == "localhost" ? "127.0.0.1" : host.c_str(), &a) != 1)
        return std::nullopt;
    return UdpAddress{ntohl(a.s_addr), port};
}

// ── The socket ────────────────────────────────────────────────────────────

struct UdpSocketPort::Impl {
    socket_t fd = kInvalidSocket;
    u16 port = 0;
};

UdpSocketPort::UdpSocketPort() : impl_(std::make_unique<Impl>()) {}

UdpSocketPort::~UdpSocketPort() {
    net::close_socket(impl_->fd);
}

bool UdpSocketPort::open(u16 port) {
    net::startup();
    const socket_t s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == kInvalidSocket) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        net::close_socket(s);
        return false;
    }
    socklen_t len = sizeof(addr);
    if (getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len) == 0)
        impl_->port = ntohs(addr.sin_port);
    net::set_blocking(s, false);
    net::ignore_udp_resets(s); // a peer gone mustn't fail our reads (Windows)
    impl_->fd = s;
    return true;
}

u16 UdpSocketPort::port() const {
    return impl_->port;
}

void UdpSocketPort::send_to(const UdpAddress& to, const std::vector<u8>& datagram) {
    if (impl_->fd == kInvalidSocket) return;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(to.ip);
    addr.sin_port = htons(to.port);
    // A datagram that can't go now is as good as lost: the stream resends
    (void)sendto(impl_->fd, reinterpret_cast<const char*>(datagram.data()),
                 static_cast<int>(datagram.size()), net::kSendFlags,
                 reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
}

std::vector<std::pair<UdpAddress, std::vector<u8>>> UdpSocketPort::receive() {
    std::vector<std::pair<UdpAddress, std::vector<u8>>> out;
    if (impl_->fd == kInvalidSocket) return out;
    // A bound on one call's reads: a flood mustn't hold the frame
    for (int i = 0; i < 4096; ++i) {
        u8 buf[2048];
        sockaddr_in from{};
        socklen_t len = sizeof(from);
        const int n =
            static_cast<int>(recvfrom(impl_->fd, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                                      reinterpret_cast<sockaddr*>(&from), &len));
        if (n < 0) break; // nothing more (or an error: nothing to read)
        out.emplace_back(UdpAddress{ntohl(from.sin_addr.s_addr), ntohs(from.sin_port)},
                         std::vector<u8>(buf, buf + n));
    }
    return out;
}

// ── The streams ───────────────────────────────────────────────────────────

namespace {

/// Every datagram starts with this, then its type and its stream's tag.
constexpr u8 kMagic = 0xA7;
enum Type : u8 {
    kHello = 1,   ///< a joiner opening a stream
    kWelcome = 2, ///< the answer
    kData = 3,    ///< u32 segment, u32 next expected from the peer, bytes
    kAck = 4,     ///< u32 next expected from the peer
    kBye = 5,
};
/// Nothing sent to a peer this long: an acknowledgement, to keep it heard.
constexpr i64 kKeepAliveMs = 1000;
/// An acknowledgement of what came in order waits this long for data of
/// ours to ride on (as TCP's delayed ACK): a lockstep's traffic, a frame
/// each way each tick, then needs none of its own.
constexpr i64 kAckDelayMs = 20;

void put_u32(std::vector<u8>& out, u32 v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<u8>(v >> (8 * i)));
}

u32 get_u32(const std::vector<u8>& d, size_t at) {
    u32 v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<u32>(d[at + i]) << (8 * i);
    return v;
}

std::vector<u8> header(u8 type, u32 tag) {
    std::vector<u8> d{kMagic, type};
    put_u32(d, tag);
    return d;
}

u32 random_tag() {
    // One a thread: streams may be opened from more than one
    thread_local std::mt19937 rng{std::random_device{}()};
    u32 t = 0;
    while (t == 0) t = rng();
    return t;
}

} // namespace

struct ReliableUdp::Stream {
    UdpAddress peer;
    u32 tag = 0;
    State state = State::Connecting;
    i64 started = 0;
    i64 last_heard = 0;
    i64 last_hello = -1;
    i64 last_sent = 0;
    // Sending: bytes not yet in a segment, and those unacknowledged
    std::vector<u8> outgoing;
    u32 next_seq = 0;
    struct Segment {
        std::vector<u8> bytes;
        i64 sent_at = 0;
        i64 wait = kFirstResendMs;
    };
    std::map<u32, Segment> unacked;
    // Receiving: the next segment expected, those come early, and the bytes
    u32 expected = 0;
    std::map<u32, std::vector<u8>> early;
    std::vector<u8> received;
    /// An acknowledgement owed: at once (a segment early or again: its
    /// sender may be resending), or since when (in order: it may wait)
    bool ack_now = false;
    i64 ack_owed_since = -1;

    bool live() const { return state == State::Open || state == State::Closing; }
};

ReliableUdp::ReliableUdp(DatagramPort& port) : port_(port) {}

ReliableUdp::~ReliableUdp() {
    stop_ = true;
    if (pumper_.joinable()) pumper_.join();
    const std::lock_guard lock(mu_);
    for (auto& [id, s] : streams_)
        if (s->live()) send_control(*s, kBye);
}

void ReliableUdp::run_in_background() {
    if (pumper_.joinable()) return;
    pumper_ = std::thread([this] {
        while (!stop_) {
            const i64 now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count();
            {
                const std::lock_guard lock(mu_);
                pump_locked(now);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });
}

void ReliableUdp::listen(bool on) {
    const std::lock_guard lock(mu_);
    listening_ = on;
}

u32 ReliableUdp::connect(const UdpAddress& to, i64 now) {
    const std::lock_guard lock(mu_);
    auto s = std::make_unique<Stream>();
    s->peer = to;
    s->tag = random_tag();
    s->state = State::Connecting;
    s->started = now;
    s->last_heard = now;
    const u32 id = next_id_++;
    streams_[id] = std::move(s);
    return id;
}

std::vector<u32> ReliableUdp::take_accepted() {
    const std::lock_guard lock(mu_);
    return std::exchange(accepted_, {});
}

ReliableUdp::Stream* ReliableUdp::find(u32 id) const {
    const auto it = streams_.find(id);
    return it == streams_.end() ? nullptr : it->second.get();
}

ReliableUdp::Stream* ReliableUdp::by_address(const UdpAddress& a) const {
    for (const auto& [id, s] : streams_)
        if (s->peer == a && s->state != State::Closed) return s.get();
    return nullptr;
}

void ReliableUdp::send_control(const Stream& s, u8 type) const {
    std::vector<u8> d = header(type, s.tag);
    if (type == kAck) put_u32(d, s.expected);
    port_.send_to(s.peer, d);
}

void ReliableUdp::on_datagram(const UdpAddress& from, const std::vector<u8>& d) {
    if (d.size() < 6 || d[0] != kMagic) return; // not ours
    const u8 type = d[1];
    const u32 tag = get_u32(d, 2);
    Stream* s = by_address(from);
    if (type == kHello) {
        if (s && s->tag == tag) { // its hello again: the welcome was lost
            send_control(*s, kWelcome);
            return;
        }
        if (!listening_) return;
        if (s) s->state = State::Closed; // the peer started over
        auto fresh = std::make_unique<Stream>();
        fresh->peer = from;
        fresh->tag = tag;
        fresh->state = State::Open;
        fresh->started = fresh->last_heard = fresh->last_sent = now_;
        const u32 id = next_id_++;
        send_control(*fresh, kWelcome);
        streams_[id] = std::move(fresh);
        accepted_.push_back(id);
        return;
    }
    if (!s || s->tag != tag) return; // another stream's, or a stale one's
    s->last_heard = now_;
    const auto acknowledge = [&](u32 next) {
        s->unacked.erase(s->unacked.begin(), s->unacked.lower_bound(next));
    };
    switch (type) {
    case kWelcome:
        if (s->state == State::Connecting) s->state = State::Open;
        return;
    case kData: {
        if (d.size() < 14) return;
        if (s->state == State::Connecting) s->state = State::Open; // the welcome was lost
        const u32 seq = get_u32(d, 6);
        acknowledge(get_u32(d, 10));
        if (seq == s->expected) {
            if (s->ack_owed_since < 0) s->ack_owed_since = now_;
            s->received.insert(s->received.end(), d.begin() + 14, d.end());
            ++s->expected;
            for (auto it = s->early.find(s->expected); it != s->early.end();
                 it = s->early.find(s->expected)) {
                s->received.insert(s->received.end(), it->second.begin(), it->second.end());
                s->early.erase(it);
                ++s->expected;
            }
        } else {
            s->ack_now = true;
            if (seq > s->expected && seq - s->expected < kWindow)
                s->early.emplace(seq, std::vector<u8>(d.begin() + 14, d.end()));
        }
        return;
    }
    case kAck:
        if (d.size() >= 10) acknowledge(get_u32(d, 6));
        return;
    case kBye: s->state = State::Closed; return;
    default: return;
    }
}

void ReliableUdp::flush(Stream& s) {
    bool sent = false;
    const auto send_segment = [&](u32 seq, const std::vector<u8>& bytes) {
        std::vector<u8> d = header(kData, s.tag);
        put_u32(d, seq);
        put_u32(d, s.expected);
        d.insert(d.end(), bytes.begin(), bytes.end());
        port_.send_to(s.peer, d);
        sent = true;
    };
    // New segments, while the window has room
    size_t at = 0;
    while (at < s.outgoing.size() && s.unacked.size() < kWindow) {
        const size_t n = std::min(kSegmentBytes, s.outgoing.size() - at);
        Stream::Segment seg;
        seg.bytes.assign(s.outgoing.begin() + static_cast<long>(at),
                         s.outgoing.begin() + static_cast<long>(at + n));
        seg.sent_at = now_;
        const u32 seq = s.next_seq++;
        send_segment(seq, seg.bytes);
        s.unacked.emplace(seq, std::move(seg));
        at += n;
    }
    s.outgoing.erase(s.outgoing.begin(), s.outgoing.begin() + static_cast<long>(at));
    // Those unacknowledged past their wait, again
    for (auto& [seq, seg] : s.unacked) {
        if (now_ - seg.sent_at < seg.wait) continue;
        send_segment(seq, seg.bytes);
        seg.sent_at = now_;
        seg.wait = std::min(seg.wait * 2, kMostResendMs);
    }
    const bool ack_owed =
        s.ack_now || (s.ack_owed_since >= 0 && now_ - s.ack_owed_since >= kAckDelayMs);
    if (!sent && (ack_owed || now_ - s.last_sent >= kKeepAliveMs)) {
        send_control(s, kAck);
        sent = true;
    }
    if (sent) { // what we sent says what we have
        s.last_sent = now_;
        s.ack_now = false;
        s.ack_owed_since = -1;
    }
    // Closing, and everything sent has arrived: goodbye
    if (s.state == State::Closing && s.outgoing.empty() && s.unacked.empty()) {
        send_control(s, kBye);
        s.state = State::Closed;
    }
}

void ReliableUdp::pump(i64 now) {
    const std::lock_guard lock(mu_);
    pump_locked(now);
}

void ReliableUdp::pump_locked(i64 now) {
    now_ = std::max(now_, now); // two threads pump: the clock never goes back
    for (const auto& [from, d] : port_.receive()) on_datagram(from, d);
    for (auto& [id, s] : streams_) {
        switch (s->state) {
        case State::Connecting:
            if (now - s->started > kConnectMs) {
                s->state = State::Closed;
            } else if (s->last_hello < 0 || now - s->last_hello >= kHelloEveryMs) {
                s->last_hello = now;
                send_control(*s, kHello);
            }
            break;
        case State::Open:
        case State::Closing:
            if (now - s->last_heard > kLostMs) s->state = State::Closed;
            else flush(*s);
            break;
        case State::Closed: break;
        }
    }
}

bool ReliableUdp::send(u32 id, const u8* data, size_t len) {
    const std::lock_guard lock(mu_);
    Stream* s = find(id);
    if (!s || (s->state != State::Open && s->state != State::Connecting)) return false;
    s->outgoing.insert(s->outgoing.end(), data, data + len);
    // Sent now, as a socket's would be (on the last pump's clock); a stream
    // still connecting sends once it opens
    if (s->state == State::Open) flush(*s);
    return true;
}

std::vector<u8> ReliableUdp::take_received(u32 id) {
    const std::lock_guard lock(mu_);
    Stream* s = find(id);
    return s ? std::exchange(s->received, {}) : std::vector<u8>{};
}

ReliableUdp::State ReliableUdp::state(u32 id) const {
    const std::lock_guard lock(mu_);
    const Stream* s = find(id);
    return s ? s->state : State::Closed;
}

UdpAddress ReliableUdp::address(u32 id) const {
    const std::lock_guard lock(mu_);
    const Stream* s = find(id);
    return s ? s->peer : UdpAddress{};
}

void ReliableUdp::close(u32 id) {
    const std::lock_guard lock(mu_);
    Stream* s = find(id);
    if (!s) return;
    if (s->state == State::Open) s->state = State::Closing;
    else if (s->state == State::Connecting) s->state = State::Closed;
}

} // namespace osc::sim
