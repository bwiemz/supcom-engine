#include "sim/lobby_net.hpp"

#include "sim/net_transport.hpp" // extract_wire_frames, kMaxWireMessage
#include "sim/socket_platform.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <optional>
#include <utility>

namespace osc::sim {

namespace {

using net::kInvalidSocket;
using net::socket_t;

// The messages, by their first byte.
enum class Msg : u8 {
    Join = 1,         ///< client: name, whether a uid is asked for, the uid
    Welcome = 2,      ///< host: host uid, host name, your uid, your name, hosted time, peers
    Rejected = 3,     ///< host: reason (the lobby is full)
    PeerJoined = 4,   ///< host: uid, name
    PeerLeft = 5,     ///< host: uid
    Data = 6,         ///< from, to, payload (the host relays what isn't its own)
    Ping = 7,         ///< stamp
    Pong = 8,         ///< the stamp pinged
    Kick = 9,         ///< host: reason
    Game = 10,        ///< from, payload: a launched game's frame (the host relays a client's)
    Established = 11, ///< client: it reaches everyone it knows of (after its own data)
};

/// Moho caps a player's name at 24 characters.
constexpr size_t kMaxNameLength = 24;

class Writer {
public:
    explicit Writer(Msg type) { bytes_.push_back(static_cast<u8>(type)); }
    Writer& u8v(u8 v) {
        bytes_.push_back(v);
        return *this;
    }
    Writer& u32v(u32 v) {
        for (int i = 0; i < 4; ++i) bytes_.push_back(static_cast<u8>(v >> (8 * i)));
        return *this;
    }
    Writer& u64v(u64 v) {
        for (int i = 0; i < 8; ++i) bytes_.push_back(static_cast<u8>(v >> (8 * i)));
        return *this;
    }
    Writer& str(const std::string& s) {
        u32v(static_cast<u32>(s.size()));
        bytes_.insert(bytes_.end(), s.begin(), s.end());
        return *this;
    }
    Writer& bytes(const std::vector<u8>& b) {
        u32v(static_cast<u32>(b.size()));
        bytes_.insert(bytes_.end(), b.begin(), b.end());
        return *this;
    }
    const std::vector<u8>& out() const { return bytes_; }

private:
    std::vector<u8> bytes_;
};

/// Reads a message's fields; every read fails (nullopt) past its end.
class Reader {
public:
    explicit Reader(const std::vector<u8>& b) : b_(b) {}
    std::optional<u8> u8v() {
        if (at_ + 1 > b_.size()) return std::nullopt;
        return b_[at_++];
    }
    std::optional<u32> u32v() {
        if (at_ + 4 > b_.size()) return std::nullopt;
        u32 v = 0;
        for (int i = 0; i < 4; ++i)
            v |= static_cast<u32>(b_[at_ + static_cast<size_t>(i)]) << (8 * i);
        at_ += 4;
        return v;
    }
    std::optional<u64> u64v() {
        if (at_ + 8 > b_.size()) return std::nullopt;
        u64 v = 0;
        for (int i = 0; i < 8; ++i)
            v |= static_cast<u64>(b_[at_ + static_cast<size_t>(i)]) << (8 * i);
        at_ += 8;
        return v;
    }
    std::optional<std::string> str() {
        const auto n = u32v();
        if (!n || at_ + *n > b_.size()) return std::nullopt;
        std::string s(b_.begin() + static_cast<long>(at_),
                      b_.begin() + static_cast<long>(at_ + *n));
        at_ += *n;
        return s;
    }
    std::optional<std::vector<u8>> bytes() {
        const auto n = u32v();
        if (!n || at_ + *n > b_.size()) return std::nullopt;
        std::vector<u8> v(b_.begin() + static_cast<long>(at_),
                          b_.begin() + static_cast<long>(at_ + *n));
        at_ += *n;
        return v;
    }

private:
    const std::vector<u8>& b_;
    size_t at_ = 0;
};

bool same_no_case(const std::string& a, const std::string& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) ==
                      std::tolower(static_cast<unsigned char>(y));
           });
}

/// Moho's MakeValidPlayerName: `name` cut to 24 characters ("Player" if
/// empty), with 1, 2, ... appended while `taken` says someone has it.
template <typename Taken> std::string unique_name(const std::string& name, const Taken& taken) {
    std::string valid = name.substr(0, kMaxNameLength);
    if (valid.empty()) valid = "Player";
    const std::string base = valid;
    for (int k = 1; taken(valid); ++k) {
        const std::string suffix = std::to_string(k);
        valid = base.substr(0, kMaxNameLength - std::min(kMaxNameLength, suffix.size())) + suffix;
    }
    return valid;
}

/// One TCP connection, framed.
struct Conn {
    socket_t fd = kInvalidSocket;
    std::vector<u8> rbuf;
    std::vector<std::vector<u8>> frames;
    bool connecting = false;       ///< a client's non-blocking connect, not yet done
    u32 uid = LobbyNet::kEveryone; ///< the host's: not yet joined
    std::string name;
    i64 last_heard = -1;
    u32 ping_ms = 0;
    i64 last_ping = -1;

    bool open() const { return fd != kInvalidSocket; }
    void close() {
        net::close_socket(fd);
        fd = kInvalidSocket;
        rbuf.clear();
    }
    /// Send one message; closes the connection if it fails.
    bool send(const std::vector<u8>& msg) {
        if (!open() || connecting) return false;
        if (msg.size() > kMaxWireMessage) {
            spdlog::error("[lobby] not sending a {}-byte message: over the {}-byte limit",
                          msg.size(), kMaxWireMessage);
            return false;
        }
        std::vector<u8> framed;
        net::frame_message(framed, msg);
        if (!net::send_all(fd, framed.data(), framed.size())) {
            close();
            return false;
        }
        return true;
    }
};

/// Read whatever the open connections have, into their frames; a closed or
/// misbehaving connection is closed.
void read_all(std::vector<Conn*>& conns) {
    for (;;) {
        fd_set fds;
        FD_ZERO(&fds);
        socket_t maxfd = 0;
        bool any = false;
        for (Conn* c : conns) {
            if (!c->open() || c->connecting) continue;
            FD_SET(c->fd, &fds);
            maxfd = std::max(maxfd, c->fd);
            any = true;
        }
        if (!any) return;
        timeval tv{0, 0};
        if (select(static_cast<int>(maxfd) + 1, &fds, nullptr, nullptr, &tv) <= 0) return;
        bool progressed = false;
        for (Conn* c : conns) {
            if (!c->open() || c->connecting || !FD_ISSET(c->fd, &fds)) continue;
            u8 tmp[4096];
            const int n =
                static_cast<int>(recv(c->fd, reinterpret_cast<char*>(tmp), sizeof(tmp), 0));
            if (n <= 0) {
                c->close();
                continue;
            }
            c->rbuf.insert(c->rbuf.end(), tmp, tmp + n);
            progressed = true;
            if (!extract_wire_frames(c->rbuf, c->frames)) {
                spdlog::warn("[lobby] a peer announced a message over {} bytes; dropping it",
                             kMaxWireMessage);
                c->close();
            }
        }
        if (!progressed) return;
    }
}

} // namespace

struct LobbyNet::Impl {
    std::string local_name;
    u32 max_connections = 0;
    u32 local_uid = 0;
    u32 host_uid = 0;
    std::optional<u32> wanted_uid; ///< set_local_uid's
    u16 port = 0;
    u64 hosted_time = 0;
    bool is_host = false;
    bool welcomed = false;
    bool done = false; ///< a client refused, kicked or cut off: nothing more comes
    socket_t listen_fd = kInvalidSocket;
    u32 next_uid = 1;
    // The host's: one per player (and not-yet-joined connection). A client's:
    // its one to the host.
    std::vector<Conn> conns;
    i64 join_started = -1;
    std::vector<LobbyPeer> peers;
    std::vector<LobbyEvent> pending; ///< events from calls between polls
    std::vector<std::vector<u8>> game_inbox; ///< game frames, for take_game

    ~Impl() {
        for (Conn& c : conns) c.close();
        net::close_socket(listen_fd);
    }

    LobbyPeer* find_peer(u32 uid) {
        for (LobbyPeer& p : peers)
            if (p.uid == uid) return &p;
        return nullptr;
    }
    Conn* conn_of(u32 uid) {
        for (Conn& c : conns)
            if (c.open() && c.uid == uid) return &c;
        return nullptr;
    }
    std::string name_of(u32 uid) const {
        if (uid == local_uid) return local_name;
        for (const LobbyPeer& p : peers)
            if (p.uid == uid) return p.name;
        return {};
    }
    u32 joined_count() const {
        u32 n = 0;
        for (const Conn& c : conns)
            if (c.open() && c.uid != kEveryone) ++n;
        return n;
    }
    void remove_peer(u32 uid, std::vector<LobbyEvent>& events) {
        const auto it = std::find_if(peers.begin(), peers.end(),
                                     [&](const LobbyPeer& p) { return p.uid == uid; });
        if (it == peers.end()) return;
        events.push_back({LobbyEvent::Kind::PeerLeft, uid, it->name, {}, {}});
        peers.erase(it);
    }
    /// The host: tell everyone joined but `except`.
    void tell_others(u32 except, const std::vector<u8>& msg) {
        for (Conn& c : conns)
            if (c.open() && c.uid != kEveryone && c.uid != except) c.send(msg);
    }

    void host_poll(i64 now, std::vector<LobbyEvent>& events);
    void client_poll(i64 now, std::vector<LobbyEvent>& events);
    void host_frame(Conn& c, const std::vector<u8>& f, i64 now, std::vector<LobbyEvent>& events);
};

LobbyNet::LobbyNet(std::string local_name, u32 max_connections) : impl_(std::make_unique<Impl>()) {
    impl_->local_name = std::move(local_name);
    impl_->max_connections = max_connections;
}

LobbyNet::~LobbyNet() = default;

void LobbyNet::set_local_uid(u32 uid) {
    impl_->wanted_uid = uid;
}

bool LobbyNet::host(u16 port, u64 hosted_time) {
    net::startup();
    socket_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == kInvalidSocket) return false;
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(s, 16) != 0) {
        net::close_socket(s);
        return false;
    }
    socklen_t alen = sizeof(addr);
    if (getsockname(s, reinterpret_cast<sockaddr*>(&addr), &alen) == 0)
        impl_->port = ntohs(addr.sin_port);
    impl_->listen_fd = s;
    impl_->is_host = true;
    impl_->local_uid = impl_->wanted_uid.value_or(0);
    impl_->host_uid = impl_->local_uid;
    impl_->hosted_time = hosted_time;
    impl_->local_name = valid_player_name(impl_->local_uid, impl_->local_name);
    return true;
}

bool LobbyNet::join(const std::string& address, u16 port) {
    net::startup();
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, address.c_str(), &addr.sin_addr) != 1) return false;
    socket_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == kInvalidSocket) return false;
    // Not blocking: an unreachable host mustn't hold up the frame. poll()
    // sees the connection complete, or time out.
    net::set_blocking(s, false);
    Conn c;
    c.fd = s;
    c.connecting = true;
    if (connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 &&
        !net::connect_in_progress()) {
        net::close_socket(s);
        return false;
    }
    impl_->conns.push_back(std::move(c));
    impl_->port = port;
    impl_->is_host = false;
    return true;
}

void LobbyNet::send(u32 to, const std::vector<u8>& payload) {
    const std::vector<u8> msg =
        Writer(Msg::Data).u32v(impl_->local_uid).u32v(to).bytes(payload).out();
    if (impl_->is_host) {
        if (to == kEveryone) impl_->tell_others(kEveryone, msg);
        else if (Conn* c = impl_->conn_of(to)) c->send(msg);
        return;
    }
    // A client: everything goes through the host
    if (!impl_->conns.empty() && impl_->welcomed) impl_->conns.front().send(msg);
}

bool LobbyNet::eject(u32 uid, const std::string& reason) {
    if (!impl_->is_host) return false;
    Conn* c = impl_->conn_of(uid);
    if (!c) return false;
    c->send(Writer(Msg::Kick).str(reason).out());
    c->close();
    impl_->tell_others(uid, Writer(Msg::PeerLeft).u32v(uid).out());
    impl_->remove_peer(uid, impl_->pending);
    return true;
}

void LobbyNet::report_established() {
    if (!impl_->is_host && impl_->welcomed && !impl_->conns.empty())
        impl_->conns.front().send(Writer(Msg::Established).out());
}

void LobbyNet::send_game(const std::vector<u8>& payload) {
    const std::vector<u8> msg = Writer(Msg::Game).u32v(impl_->local_uid).bytes(payload).out();
    if (impl_->is_host) impl_->tell_others(kEveryone, msg);
    else if (!impl_->conns.empty() && impl_->welcomed) impl_->conns.front().send(msg);
}

std::vector<std::vector<u8>> LobbyNet::take_game() {
    std::vector<std::vector<u8>> frames;
    std::swap(frames, impl_->game_inbox);
    return frames;
}

void LobbyNet::stop_joining() {
    net::close_socket(impl_->listen_fd);
    impl_->listen_fd = kInvalidSocket;
}

std::vector<LobbyEvent> LobbyNet::poll(i64 now_ms) {
    std::vector<LobbyEvent> events;
    std::swap(events, impl_->pending);
    if (impl_->is_host) impl_->host_poll(now_ms, events);
    else if (!impl_->conns.empty()) impl_->client_poll(now_ms, events);
    return events;
}

void LobbyNet::Impl::host_poll(i64 now, std::vector<LobbyEvent>& events) {
    // Accept whoever is waiting (until the game starts)
    while (listen_fd != kInvalidSocket) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(listen_fd, &fds);
        timeval tv{0, 0};
        if (select(static_cast<int>(listen_fd) + 1, &fds, nullptr, nullptr, &tv) <= 0) break;
        socket_t s = accept(listen_fd, nullptr, nullptr);
        if (s == kInvalidSocket) break;
        net::configure_stream(s);
        Conn c;
        c.fd = s;
        c.last_heard = now;
        conns.push_back(std::move(c));
    }
    std::vector<Conn*> open;
    open.reserve(conns.size());
    for (Conn& c : conns) open.push_back(&c);
    read_all(open);
    for (size_t i = 0; i < conns.size(); ++i) {
        std::vector<std::vector<u8>> frames;
        std::swap(frames, conns[i].frames);
        for (const auto& f : frames) {
            if (!conns[i].open()) break;
            conns[i].last_heard = now;
            host_frame(conns[i], f, now, events);
        }
    }
    // Those gone: the others hear of it
    for (Conn& c : conns) {
        if (c.open() || c.uid == kEveryone) continue;
        const u32 uid = c.uid;
        c.uid = kEveryone;
        if (!find_peer(uid)) continue; // ejected: already told
        tell_others(uid, Writer(Msg::PeerLeft).u32v(uid).out());
        remove_peer(uid, events);
    }
    conns.erase(std::remove_if(conns.begin(), conns.end(), [](const Conn& c) { return !c.open(); }),
                conns.end());
    // Keep each player's ping and quiet current
    for (Conn& c : conns) {
        if (c.uid == kEveryone || (c.last_ping >= 0 && now - c.last_ping < kPingIntervalMs))
            continue;
        c.last_ping = now;
        c.send(Writer(Msg::Ping).u64v(static_cast<u64>(now)).out());
    }
    for (LobbyPeer& p : peers)
        if (const Conn* c = conn_of(p.uid)) {
            p.last_heard_ms = c->last_heard;
            p.ping_ms = c->ping_ms;
        }
}

void LobbyNet::Impl::host_frame(Conn& c, const std::vector<u8>& f, i64 now,
                                std::vector<LobbyEvent>& events) {
    Reader r(f);
    const auto type = r.u8v();
    if (!type) return;
    switch (static_cast<Msg>(*type)) {
    case Msg::Join: {
        const auto name = r.str();
        // The uid a matchmaking client gave the player, if it has one
        const auto has_uid = r.u8v();
        const auto wanted = r.u32v();
        if (!name || !has_uid || !wanted || c.uid != kEveryone) return;
        if (joined_count() >= max_connections) {
            c.send(Writer(Msg::Rejected).str("LobbyFull").out());
            c.close();
            return;
        }
        const auto taken = [&](u32 uid) {
            return uid == local_uid || uid == kEveryone || find_peer(uid) != nullptr;
        };
        if (*has_uid && taken(*wanted)) {
            c.send(Writer(Msg::Rejected).str("UidTaken").out());
            c.close();
            return;
        }
        while (taken(next_uid)) ++next_uid;
        const u32 uid = *has_uid ? *wanted : next_uid++;
        // Made unique against everyone here
        const std::string valid = unique_name(*name, [&](const std::string& n) {
            return same_no_case(n, local_name) ||
                   std::any_of(peers.begin(), peers.end(),
                               [&](const LobbyPeer& p) { return same_no_case(p.name, n); });
        });
        c.uid = uid;
        c.name = valid;
        Writer w(Msg::Welcome);
        w.u32v(local_uid).str(local_name).u32v(uid).str(valid).u64v(hosted_time);
        w.u32v(static_cast<u32>(peers.size()));
        for (const LobbyPeer& p : peers) w.u32v(p.uid).str(p.name);
        c.send(w.out());
        tell_others(uid, Writer(Msg::PeerJoined).u32v(uid).str(valid).out());
        LobbyPeer p;
        p.uid = uid;
        p.name = valid;
        p.last_heard_ms = now;
        peers.push_back(p);
        events.push_back({LobbyEvent::Kind::PeerJoined, uid, valid, {}, {}});
        return;
    }
    case Msg::Data: {
        (void)r.u32v(); // the sender's claim: it is who its connection is
        const auto to = r.u32v();
        const auto payload = r.bytes();
        if (!to || !payload || c.uid == kEveryone) return;
        const u32 from = c.uid;
        if (*to == local_uid || *to == kEveryone)
            events.push_back({LobbyEvent::Kind::Data, from, c.name, {}, *payload});
        const std::vector<u8> relay = Writer(Msg::Data).u32v(from).u32v(*to).bytes(*payload).out();
        if (*to == kEveryone) tell_others(from, relay);
        else if (*to != local_uid)
            if (Conn* target = conn_of(*to)) target->send(relay);
        return;
    }
    case Msg::Established:
        if (c.uid != kEveryone)
            events.push_back({LobbyEvent::Kind::PeerEstablished, c.uid, c.name, {}, {}});
        return;
    case Msg::Game: {
        (void)r.u32v(); // as Data: the connection says who
        const auto payload = r.bytes();
        if (!payload || c.uid == kEveryone) return;
        game_inbox.push_back(*payload);
        tell_others(c.uid, Writer(Msg::Game).u32v(c.uid).bytes(*payload).out());
        return;
    }
    case Msg::Ping: {
        const auto stamp = r.u64v();
        if (stamp) c.send(Writer(Msg::Pong).u64v(*stamp).out());
        return;
    }
    case Msg::Pong: {
        const auto stamp = r.u64v();
        if (stamp && now >= static_cast<i64>(*stamp))
            c.ping_ms = static_cast<u32>(now - static_cast<i64>(*stamp));
        return;
    }
    default: return; // not a client's to send
    }
}

void LobbyNet::Impl::client_poll(i64 now, std::vector<LobbyEvent>& events) {
    Conn& host = conns.front();
    if (done) return;
    if (join_started < 0) join_started = now;
    // The connection, completing
    if (host.connecting) {
        // Done, one way or the other: writable, or (Winsock reports a failed
        // connect only there) in the exception set
        fd_set wfds;
        fd_set efds;
        FD_ZERO(&wfds);
        FD_ZERO(&efds);
        FD_SET(host.fd, &wfds);
        FD_SET(host.fd, &efds);
        timeval tv{0, 0};
        if (select(static_cast<int>(host.fd) + 1, nullptr, &wfds, &efds, &tv) > 0) {
            int err = 0;
            socklen_t len = sizeof(err);
            getsockopt(host.fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len);
            if (err != 0 || FD_ISSET(host.fd, &efds)) {
                host.close();
            } else {
                host.connecting = false;
                net::set_blocking(host.fd, true);
                net::configure_stream(host.fd);
                host.last_heard = now;
                host.send(Writer(Msg::Join)
                              .str(local_name)
                              .u8v(wanted_uid ? 1 : 0)
                              .u32v(wanted_uid.value_or(0))
                              .out());
            }
        }
        if (host.open() && host.connecting && now - join_started > kJoinTimeoutMs) host.close();
        if (!host.open()) {
            done = true;
            events.push_back({LobbyEvent::Kind::ConnectionFailed, 0, {}, "HostLeft", {}});
            return;
        }
        if (host.connecting) return;
    }
    std::vector<Conn*> one{&host};
    read_all(one);
    std::vector<std::vector<u8>> frames;
    std::swap(frames, host.frames);
    for (const auto& f : frames) {
        host.last_heard = now;
        Reader r(f);
        const auto type = r.u8v();
        if (!type) continue;
        switch (static_cast<Msg>(*type)) {
        case Msg::Welcome: {
            const auto host_id = r.u32v();
            const auto host_name = r.str();
            const auto my_uid = r.u32v();
            const auto my_name = r.str();
            const auto hosted = r.u64v();
            const auto count = r.u32v();
            if (!host_id || !host_name || !my_uid || !my_name || !hosted || !count || welcomed)
                break;
            host_uid = *host_id;
            local_uid = *my_uid;
            local_name = *my_name;
            hosted_time = *hosted;
            peers.clear();
            LobbyPeer h;
            h.uid = host_uid;
            h.name = *host_name;
            h.last_heard_ms = now;
            peers.push_back(h);
            for (u32 k = 0; k < *count; ++k) {
                const auto uid = r.u32v();
                const auto name = r.str();
                if (!uid || !name) break;
                LobbyPeer p;
                p.uid = *uid;
                p.name = *name;
                peers.push_back(p);
            }
            welcomed = true;
            events.push_back({LobbyEvent::Kind::ConnectedToHost, local_uid, local_name, {}, {}});
            break;
        }
        case Msg::Rejected:
        case Msg::Kick: {
            const auto reason = r.str();
            events.push_back({LobbyEvent::Kind::Ejected, 0, {}, reason ? *reason : "", {}});
            host.close();
            done = true;
            welcomed = false;
            peers.clear();
            return;
        }
        case Msg::PeerJoined: {
            const auto uid = r.u32v();
            const auto name = r.str();
            if (!uid || !name || find_peer(*uid)) break;
            LobbyPeer p;
            p.uid = *uid;
            p.name = *name;
            peers.push_back(p);
            events.push_back({LobbyEvent::Kind::PeerJoined, *uid, *name, {}, {}});
            break;
        }
        case Msg::PeerLeft: {
            const auto uid = r.u32v();
            if (uid) remove_peer(*uid, events);
            break;
        }
        case Msg::Data: {
            const auto from = r.u32v();
            (void)r.u32v();
            const auto payload = r.bytes();
            if (from && payload)
                events.push_back({LobbyEvent::Kind::Data, *from, name_of(*from), {}, *payload});
            break;
        }
        case Msg::Game: {
            (void)r.u32v();
            const auto payload = r.bytes();
            if (payload) game_inbox.push_back(*payload);
            break;
        }
        case Msg::Ping: {
            const auto stamp = r.u64v();
            if (stamp) host.send(Writer(Msg::Pong).u64v(*stamp).out());
            break;
        }
        case Msg::Pong: {
            const auto stamp = r.u64v();
            if (stamp && now >= static_cast<i64>(*stamp))
                host.ping_ms = static_cast<u32>(now - static_cast<i64>(*stamp));
            break;
        }
        default: break;
        }
    }
    if (!host.open()) {
        // The host is gone
        done = true;
        welcomed = false;
        peers.clear();
        events.push_back({LobbyEvent::Kind::ConnectionFailed, 0, {}, "HostLeft", {}});
        return;
    }
    if (host.last_ping < 0 || now - host.last_ping >= kPingIntervalMs) {
        host.last_ping = now;
        host.send(Writer(Msg::Ping).u64v(static_cast<u64>(now)).out());
    }
    // Everyone's messages come through the host: its ping and quiet are theirs
    for (LobbyPeer& p : peers) {
        p.last_heard_ms = host.last_heard;
        p.ping_ms = host.ping_ms;
    }
}

bool LobbyNet::hosting() const {
    return impl_->is_host;
}
bool LobbyNet::joined() const {
    return !impl_->is_host && impl_->welcomed && !impl_->done;
}
u32 LobbyNet::local_uid() const {
    return impl_->local_uid;
}
const std::string& LobbyNet::local_name() const {
    return impl_->local_name;
}
u32 LobbyNet::host_uid() const {
    return impl_->host_uid;
}
u16 LobbyNet::port() const {
    return impl_->port;
}
u64 LobbyNet::hosted_time() const {
    return impl_->hosted_time;
}
const std::vector<LobbyPeer>& LobbyNet::peers() const {
    return impl_->peers;
}

const LobbyPeer* LobbyNet::peer(u32 uid) const {
    for (const LobbyPeer& p : impl_->peers)
        if (p.uid == uid) return &p;
    return nullptr;
}

LobbyGameTransport::LobbyGameTransport(std::unique_ptr<LobbyNet> net, std::function<i64()> clock)
    : net_(std::move(net)), clock_(std::move(clock)) {}

void LobbyGameTransport::broadcast(const std::vector<u8>& msg) {
    net_->send_game(msg);
}

std::vector<std::vector<u8>> LobbyGameTransport::receive() {
    for (LobbyEvent& e : net_->poll(clock_())) {
        if (e.kind == LobbyEvent::Kind::PeerLeft)
            spdlog::warn("[mp] {} (uid {}) left the game", e.name, e.uid);
        else if (e.kind == LobbyEvent::Kind::ConnectionFailed)
            spdlog::warn("[mp] the connection to the host is gone");
        else if (e.kind == LobbyEvent::Kind::Ejected)
            spdlog::warn("[mp] the host ejected this player ({})", e.reason);
        else if (e.kind == LobbyEvent::Kind::Data) data_.push_back(std::move(e));
    }
    return net_->take_game();
}

std::vector<LobbyEvent> LobbyGameTransport::take_data() {
    std::vector<LobbyEvent> data;
    std::swap(data, data_);
    return data;
}

std::string LobbyNet::valid_player_name(u32 uid, const std::string& name) const {
    return unique_name(name, [&](const std::string& n) {
        if (uid != impl_->local_uid && same_no_case(n, impl_->local_name)) return true;
        return std::any_of(impl_->peers.begin(), impl_->peers.end(), [&](const LobbyPeer& p) {
            return p.uid != uid && same_no_case(p.name, n);
        });
    });
}

} // namespace osc::sim
