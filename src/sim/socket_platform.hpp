#pragma once

// The sockets the engine's networking shares (the lobby's LobbyNet, LAN
// discovery): POSIX or Winsock, blocking sends of whole buffers, and the
// u32-length framing of extract_wire_frames. Include only from .cpp files:
// it brings in the platform's socket headers.

#include "core/types.hpp"

#include <cerrno>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX // else <windows.h> defines min and max macros, breaking std::min
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace osc::sim::net {

#ifdef _WIN32
using socket_t = SOCKET;
inline constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
using socket_t = int;
inline constexpr socket_t kInvalidSocket = -1;
#endif

inline void startup() {
#ifdef _WIN32
    static bool started = false;
    if (!started) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        started = true; // process-lifetime; matched by no explicit cleanup
    }
#endif
}

inline void close_socket(socket_t s) {
    if (s == kInvalidSocket) return;
#ifdef _WIN32
    closesocket(s);
#else
    ::close(s);
#endif
}

// Writing to a socket whose peer has reset raises SIGPIPE on POSIX, which
// kills the process by default. Suppress it per call (Linux) or per socket
// (macOS/BSD) so a vanished peer is just a failed send.
#ifdef MSG_NOSIGNAL
inline constexpr int kSendFlags = MSG_NOSIGNAL;
#else
inline constexpr int kSendFlags = 0;
#endif

inline void configure_stream(socket_t s) {
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
#ifdef SO_NOSIGPIPE
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
}

/// Make `s` block (send_all's sockets) or not (a connect that mustn't hold
/// up the frame). False on error.
inline bool set_blocking(socket_t s, bool blocking) {
#ifdef _WIN32
    u_long mode = blocking ? 0 : 1;
    return ioctlsocket(s, FIONBIO, &mode) == 0;
#else
    const int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0) return false;
    return fcntl(s, F_SETFL, blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK)) == 0;
#endif
}

/// A non-blocking connect still under way (rather than failed).
inline bool connect_in_progress() {
#ifdef _WIN32
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EINPROGRESS;
#endif
}

/// A non-blocking read that found nothing waiting (rather than failed).
inline bool would_block() {
#ifdef _WIN32
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

/// A UDP socket's reads go on past an ICMP "port unreachable" for an
/// earlier send. Windows otherwise fails the next recvfrom with
/// WSAECONNRESET (an answer to a finder that has gone, say); elsewhere an
/// unconnected socket never sees it.
inline void ignore_udp_resets(socket_t s) {
#ifdef _WIN32
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12) // mstcpip.h's
#endif
    BOOL report = FALSE;
    DWORD returned = 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &report, sizeof(report), nullptr, 0, &returned, nullptr,
             nullptr);
#else
    (void)s;
#endif
}

/// Blocking send of the whole buffer; false on error.
inline bool send_all(socket_t s, const u8* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        int n = static_cast<int>(send(s, reinterpret_cast<const char*>(data + sent),
                                      static_cast<int>(len - sent), kSendFlags));
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

/// `msg` with its little-endian u32 length before it, appended to `out`.
inline void frame_message(std::vector<u8>& out, const std::vector<u8>& msg) {
    const u32 len = static_cast<u32>(msg.size());
    out.push_back(static_cast<u8>(len & 0xFF));
    out.push_back(static_cast<u8>((len >> 8) & 0xFF));
    out.push_back(static_cast<u8>((len >> 16) & 0xFF));
    out.push_back(static_cast<u8>((len >> 24) & 0xFF));
    out.insert(out.end(), msg.begin(), msg.end());
}

} // namespace osc::sim::net
