#include "sim/net_transport.hpp"

#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
static constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
static constexpr socket_t kInvalidSocket = -1;
#endif

namespace osc::sim {

namespace {

void net_startup() {
#ifdef _WIN32
    static bool started = false;
    if (!started) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        started = true; // process-lifetime; matched by no explicit cleanup
    }
#endif
}

void close_socket(socket_t s) {
    if (s == kInvalidSocket) return;
#ifdef _WIN32
    closesocket(s);
#else
    ::close(s);
#endif
}

void set_nodelay(socket_t s) {
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one),
               sizeof(one));
}

// Blocking send of the whole buffer; returns false on error.
bool send_all(socket_t s, const u8* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        int n = static_cast<int>(
            send(s, reinterpret_cast<const char*>(data + sent),
                 static_cast<int>(len - sent), 0));
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

void frame_message(std::vector<u8>& out, const std::vector<u8>& msg) {
    u32 len = static_cast<u32>(msg.size());
    out.push_back(static_cast<u8>(len & 0xFF));
    out.push_back(static_cast<u8>((len >> 8) & 0xFF));
    out.push_back(static_cast<u8>((len >> 16) & 0xFF));
    out.push_back(static_cast<u8>((len >> 24) & 0xFF));
    out.insert(out.end(), msg.begin(), msg.end());
}

} // namespace

struct TcpTransport::Impl {
    bool is_host = false;
    socket_t listen_fd = kInvalidSocket;
    u16 bound_port = 0;
    bool ok = false;

    struct Conn {
        socket_t fd = kInvalidSocket;
        std::vector<u8> rbuf; // accumulates until whole frames are available
    };
    std::vector<Conn> conns;

    ~Impl() {
        for (auto& c : conns) close_socket(c.fd);
        close_socket(listen_fd);
    }

    // Pull all complete frames out of a connection buffer.
    static void extract_frames(std::vector<u8>& buf,
                               std::vector<std::vector<u8>>& out) {
        size_t off = 0;
        while (buf.size() - off >= 4) {
            u32 len = static_cast<u32>(buf[off]) |
                      (static_cast<u32>(buf[off + 1]) << 8) |
                      (static_cast<u32>(buf[off + 2]) << 16) |
                      (static_cast<u32>(buf[off + 3]) << 24);
            if (buf.size() - off - 4 < len) break; // frame incomplete
            out.emplace_back(buf.begin() + static_cast<long>(off + 4),
                             buf.begin() + static_cast<long>(off + 4 + len));
            off += 4 + len;
        }
        if (off > 0) buf.erase(buf.begin(), buf.begin() + static_cast<long>(off));
    }
};

TcpTransport::TcpTransport(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
TcpTransport::~TcpTransport() = default;

std::unique_ptr<TcpTransport> TcpTransport::host(u16 port) {
    net_startup();
    auto impl = std::make_unique<Impl>();
    impl->is_host = true;
    socket_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == kInvalidSocket) return std::unique_ptr<TcpTransport>(new TcpTransport(std::move(impl)));
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one),
               sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        listen(s, 8) != 0) {
        close_socket(s);
        return std::unique_ptr<TcpTransport>(new TcpTransport(std::move(impl)));
    }
    socklen_t alen = sizeof(addr);
    if (getsockname(s, reinterpret_cast<sockaddr*>(&addr), &alen) == 0)
        impl->bound_port = ntohs(addr.sin_port);
    impl->listen_fd = s;
    impl->ok = true;
    return std::unique_ptr<TcpTransport>(new TcpTransport(std::move(impl)));
}

std::unique_ptr<TcpTransport> TcpTransport::join(const std::string& address,
                                                 u16 port) {
    net_startup();
    auto impl = std::make_unique<Impl>();
    impl->is_host = false;
    socket_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == kInvalidSocket) return std::unique_ptr<TcpTransport>(new TcpTransport(std::move(impl)));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, address.c_str(), &addr.sin_addr) != 1 ||
        connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close_socket(s);
        return std::unique_ptr<TcpTransport>(new TcpTransport(std::move(impl)));
    }
    set_nodelay(s);
    impl->conns.push_back({s, {}});
    impl->bound_port = port;
    impl->ok = true;
    return std::unique_ptr<TcpTransport>(new TcpTransport(std::move(impl)));
}

int TcpTransport::poll_connections() {
    if (!impl_->is_host || impl_->listen_fd == kInvalidSocket)
        return peer_count();
    for (;;) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(impl_->listen_fd, &fds);
        timeval tv{0, 0};
        int r = select(static_cast<int>(impl_->listen_fd) + 1, &fds, nullptr,
                       nullptr, &tv);
        if (r <= 0 || !FD_ISSET(impl_->listen_fd, &fds)) break;
        socket_t c = accept(impl_->listen_fd, nullptr, nullptr);
        if (c == kInvalidSocket) break;
        set_nodelay(c);
        impl_->conns.push_back({c, {}});
    }
    return peer_count();
}

void TcpTransport::broadcast(const std::vector<u8>& msg) {
    std::vector<u8> framed;
    frame_message(framed, msg);
    for (auto& c : impl_->conns) {
        if (c.fd != kInvalidSocket)
            send_all(c.fd, framed.data(), framed.size());
    }
}

std::vector<std::vector<u8>> TcpTransport::receive() {
    std::vector<std::vector<u8>> out;
    if (impl_->is_host) poll_connections();

    // Drain readable sockets (non-blocking via select with zero timeout).
    for (;;) {
        fd_set fds;
        FD_ZERO(&fds);
        socket_t maxfd = 0;
        bool any = false;
        for (auto& c : impl_->conns) {
            if (c.fd == kInvalidSocket) continue;
            FD_SET(c.fd, &fds);
            if (c.fd > maxfd) maxfd = c.fd;
            any = true;
        }
        if (!any) break;
        timeval tv{0, 0};
        int r = select(static_cast<int>(maxfd) + 1, &fds, nullptr, nullptr, &tv);
        if (r <= 0) break;

        bool progressed = false;
        for (auto& c : impl_->conns) {
            if (c.fd == kInvalidSocket || !FD_ISSET(c.fd, &fds)) continue;
            u8 tmp[4096];
            int n = static_cast<int>(recv(c.fd, reinterpret_cast<char*>(tmp),
                                          sizeof(tmp), 0));
            if (n > 0) {
                c.rbuf.insert(c.rbuf.end(), tmp, tmp + n);
                progressed = true;
            } else {
                // Peer closed or errored — drop the connection.
                close_socket(c.fd);
                c.fd = kInvalidSocket;
            }
        }
        if (!progressed) break;
    }

    // Extract complete frames; the host relays each to the other peers.
    for (size_t i = 0; i < impl_->conns.size(); ++i) {
        std::vector<std::vector<u8>> frames;
        Impl::extract_frames(impl_->conns[i].rbuf, frames);
        for (auto& f : frames) {
            if (impl_->is_host) {
                std::vector<u8> framed;
                frame_message(framed, f);
                for (size_t j = 0; j < impl_->conns.size(); ++j) {
                    if (j == i || impl_->conns[j].fd == kInvalidSocket) continue;
                    send_all(impl_->conns[j].fd, framed.data(), framed.size());
                }
            }
            out.push_back(std::move(f));
        }
    }
    return out;
}

int TcpTransport::peer_count() const {
    int n = 0;
    for (const auto& c : impl_->conns)
        if (c.fd != kInvalidSocket) ++n;
    return n;
}

u16 TcpTransport::port() const { return impl_->bound_port; }
bool TcpTransport::ok() const { return impl_->ok; }

} // namespace osc::sim
