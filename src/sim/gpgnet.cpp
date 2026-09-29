#include "sim/gpgnet.hpp"

#include "sim/socket_platform.hpp"

#include <spdlog/spdlog.h>

#include <optional>
#include <utility>

namespace osc::sim {

using net::kInvalidSocket;
using net::socket_t;

GpgNetArg GpgNetArg::number(i32 n) {
    GpgNetArg a;
    a.type = Type::Num;
    a.num = n;
    return a;
}

GpgNetArg GpgNetArg::string(std::string s) {
    GpgNetArg a;
    a.type = Type::String;
    a.str = std::move(s);
    return a;
}

bool GpgNetArg::operator==(const GpgNetArg& o) const {
    return type == o.type && (type == Type::Num ? num == o.num : str == o.str);
}

namespace {

void put_u32(std::vector<u8>& out, u32 v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<u8>(v >> (8 * i)));
}

void put_bytes(std::vector<u8>& out, const std::string& s) {
    put_u32(out, static_cast<u32>(s.size()));
    out.insert(out.end(), s.begin(), s.end());
}

/// Reads a command from the front of a buffer: nothing (and `malformed`
/// unset) while it is incomplete.
class Reader {
public:
    Reader(const std::vector<u8>& b, size_t start) : b_(b), start_(start), at_(start) {}
    size_t at() const { return at_; }
    bool malformed = false;

    std::optional<u32> u32v() {
        if (b_.size() - at_ < 4) return std::nullopt;
        u32 v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<u32>(b_[at_ + i]) << (8 * i);
        at_ += 4;
        return v;
    }
    std::optional<u8> u8v() {
        if (b_.size() - at_ < 1) return std::nullopt;
        return b_[at_++];
    }
    std::optional<std::string> bytes() {
        const auto n = u32v();
        if (!n) return std::nullopt;
        // A field, or the command so far with it, past its limit
        if (*n > kMaxGpgNetField || at_ - start_ + *n > kMaxGpgNetCommand) {
            malformed = true;
            return std::nullopt;
        }
        if (b_.size() - at_ < *n) return std::nullopt;
        std::string s(b_.begin() + static_cast<long>(at_),
                      b_.begin() + static_cast<long>(at_ + *n));
        at_ += *n;
        return s;
    }

private:
    const std::vector<u8>& b_;
    size_t start_ = 0; // where the command began
    size_t at_ = 0;
};

/// The command at the front of `r`'s buffer, if it is whole.
std::optional<GpgNetCommand> read_command(Reader& r) {
    GpgNetCommand c;
    auto name = r.bytes();
    if (!name) return std::nullopt;
    c.name = std::move(*name);
    const auto argc = r.u32v();
    if (!argc) return std::nullopt;
    if (*argc > kMaxGpgNetArgs) {
        r.malformed = true;
        return std::nullopt;
    }
    for (u32 i = 0; i < *argc; ++i) {
        const auto type = r.u8v();
        if (!type) return std::nullopt;
        GpgNetArg a;
        switch (static_cast<GpgNetArg::Type>(*type)) {
        case GpgNetArg::Type::Num: {
            const auto n = r.u32v();
            if (!n) return std::nullopt;
            a.type = GpgNetArg::Type::Num;
            a.num = static_cast<i32>(*n);
            break;
        }
        case GpgNetArg::Type::String:
        case GpgNetArg::Type::Data: {
            auto s = r.bytes();
            if (!s) return std::nullopt;
            a.type = static_cast<GpgNetArg::Type>(*type);
            a.str = std::move(*s);
            break;
        }
        default: r.malformed = true; return std::nullopt;
        }
        c.args.push_back(std::move(a));
    }
    return c;
}

} // namespace

std::vector<u8> encode_gpgnet(const GpgNetCommand& command) {
    std::vector<u8> out;
    put_bytes(out, command.name);
    put_u32(out, static_cast<u32>(command.args.size()));
    for (const GpgNetArg& a : command.args) {
        out.push_back(static_cast<u8>(a.type));
        if (a.type == GpgNetArg::Type::Num) put_u32(out, static_cast<u32>(a.num));
        else put_bytes(out, a.str);
    }
    return out;
}

bool extract_gpgnet_commands(std::vector<u8>& buf, std::vector<GpgNetCommand>& out) {
    size_t used = 0;
    bool ok = true;
    for (;;) {
        Reader r(buf, used);
        auto c = read_command(r);
        if (r.malformed) {
            ok = false;
            break;
        }
        if (!c) break;
        out.push_back(std::move(*c));
        used = r.at();
    }
    buf.erase(buf.begin(), buf.begin() + static_cast<long>(used));
    return ok;
}

struct GpgNetLink::Impl {
    socket_t fd = kInvalidSocket;
    bool connecting = false;
    bool connected = false;
    i64 connect_started = -1;
    std::vector<u8> rbuf;
    std::vector<Event> pending; // what send() found, for the next poll

    void close(std::vector<Event>& events, std::string reason) {
        if (fd == kInvalidSocket) return;
        net::close_socket(fd);
        fd = kInvalidSocket;
        connecting = connected = false;
        rbuf.clear();
        events.push_back({Event::Kind::Closed, {}, std::move(reason)});
    }
};

GpgNetLink::GpgNetLink() : impl_(std::make_unique<Impl>()) {}

GpgNetLink::~GpgNetLink() {
    net::close_socket(impl_->fd);
}

bool GpgNetLink::connect(const std::string& address, u16 port) {
    if (impl_->fd != kInvalidSocket) return false;
    net::startup();
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, address.c_str(), &addr.sin_addr) != 1) return false;
    const socket_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == kInvalidSocket) return false;
    // Not blocking: a client that doesn't answer mustn't hold up the frame
    net::set_blocking(s, false);
    if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 &&
        !net::connect_in_progress()) {
        net::close_socket(s);
        return false;
    }
    impl_->fd = s;
    impl_->connecting = true;
    impl_->connect_started = -1; // stamped by the first poll
    return true;
}

std::vector<GpgNetLink::Event> GpgNetLink::poll(i64 now_ms) {
    std::vector<Event> events = std::exchange(impl_->pending, {});
    Impl& l = *impl_;
    if (l.fd == kInvalidSocket) return events;
    if (l.connecting) {
        if (l.connect_started < 0) l.connect_started = now_ms;
        fd_set wfds, efds;
        FD_ZERO(&wfds);
        FD_ZERO(&efds);
        FD_SET(l.fd, &wfds);
        FD_SET(l.fd, &efds);
        timeval tv{0, 0};
        if (select(static_cast<int>(l.fd) + 1, nullptr, &wfds, &efds, &tv) > 0) {
            int err = 0;
            socklen_t len = sizeof(err);
            getsockopt(l.fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len);
            if (err != 0 || FD_ISSET(l.fd, &efds)) {
                l.close(events, "couldn't connect");
                return events;
            }
            l.connecting = false;
            l.connected = true;
            net::set_blocking(l.fd, true); // sends block (reads are polled)
            net::configure_stream(l.fd);
            events.push_back({Event::Kind::Connected, {}, {}});
        } else if (now_ms - l.connect_started > kConnectTimeoutMs) {
            l.close(events, "couldn't connect: timed out");
            return events;
        } else {
            return events;
        }
    }
    // Read what has come, a select at a time
    for (;;) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(l.fd, &fds);
        timeval tv{0, 0};
        if (select(static_cast<int>(l.fd) + 1, &fds, nullptr, nullptr, &tv) <= 0) break;
        u8 tmp[4096];
        const int n = static_cast<int>(recv(l.fd, reinterpret_cast<char*>(tmp), sizeof(tmp), 0));
        if (n <= 0) {
            // What came before the close still counts
            std::vector<GpgNetCommand> last;
            extract_gpgnet_commands(l.rbuf, last);
            for (auto& c : last) events.push_back({Event::Kind::Command, std::move(c), {}});
            l.close(events, "the client closed the link");
            return events;
        }
        l.rbuf.insert(l.rbuf.end(), tmp, tmp + n);
        std::vector<GpgNetCommand> commands;
        const bool ok = extract_gpgnet_commands(l.rbuf, commands);
        for (auto& c : commands) events.push_back({Event::Kind::Command, std::move(c), {}});
        if (!ok) {
            l.close(events, "the client sent a malformed command");
            return events;
        }
    }
    return events;
}

bool GpgNetLink::send(const GpgNetCommand& command) {
    Impl& l = *impl_;
    if (!l.connected) return false;
    const std::vector<u8> bytes = encode_gpgnet(command);
    if (!net::send_all(l.fd, bytes.data(), bytes.size())) {
        spdlog::warn("[gpgnet] sending {} failed: the link is down", command.name);
        l.close(l.pending, "sending failed");
        return false;
    }
    return true;
}

bool GpgNetLink::connected() const {
    return impl_->connected;
}

bool GpgNetLink::open() const {
    return impl_->fd != kInvalidSocket;
}

} // namespace osc::sim
