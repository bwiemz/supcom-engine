#pragma once

#include "core/types.hpp"

#include <memory>
#include <string>
#include <utility>
#include <set>
#include <utility>
#include <vector>

namespace osc::sim {

/// Wire framing for the TCP transport: each message is a little-endian u32
/// length, then that many bytes. Lockstep frames and drop reports are far
/// smaller than this; a peer that announces more is malformed or hostile (it
/// would have us buffer gigabytes waiting for the rest), and its connection
/// is dropped.
inline constexpr u32 kMaxWireMessage = 4u << 20;

/// Move every whole message at the front of `buf` into `out`, leaving a
/// partial one. False when the next message announces more than
/// kMaxWireMessage (the caller drops the connection).
bool extract_wire_frames(std::vector<u8>& buf, std::vector<std::vector<u8>>& out);

/// Abstract message transport for lockstep multiplayer. Messages are opaque
/// byte buffers broadcast to all other peers. A real implementation wraps
/// UDP/TCP/ICE; the loopback implementation below drives in-process peers for
/// tests and single-machine play.
class INetTransport {
public:
    virtual ~INetTransport() = default;
    /// Send a message to every other peer.
    virtual void broadcast(const std::vector<u8>& msg) = 0;
    /// Drain and return all messages received since the last call.
    virtual std::vector<std::vector<u8>> receive() = 0;
};

/// Shared in-process medium connecting several LoopbackTransport endpoints.
class LoopbackHub {
public:
    /// Register an endpoint; returns its id.
    int add_endpoint() {
        inboxes_.emplace_back();
        return static_cast<int>(inboxes_.size()) - 1;
    }
    /// Deliver a message to every endpoint except the sender (and those
    /// whose link from it is down).
    void broadcast(int from, const std::vector<u8>& msg) {
        for (int i = 0; i < static_cast<int>(inboxes_.size()); ++i)
            if (i != from && !down_.count({from, i})) inboxes_[i].push_back(msg);
    }
    /// Cut or restore the one-way link from `from` to `to` (tests: a peer that
    /// dies mid-broadcast reaches some peers and not others).
    void set_link(int from, int to, bool up) {
        if (up) down_.erase({from, to});
        else down_.insert({from, to});
    }
    /// Take all messages queued for an endpoint.
    std::vector<std::vector<u8>> drain(int id) {
        std::vector<std::vector<u8>> out;
        if (id >= 0 && id < static_cast<int>(inboxes_.size()))
            std::swap(out, inboxes_[static_cast<size_t>(id)]);
        return out;
    }

private:
    std::vector<std::vector<std::vector<u8>>> inboxes_;
    std::set<std::pair<int, int>> down_;
};

/// INetTransport backed by a shared LoopbackHub (no real network).
class LoopbackTransport : public INetTransport {
public:
    LoopbackTransport(LoopbackHub& hub, int id) : hub_(&hub), id_(id) {}
    void broadcast(const std::vector<u8>& msg) override { hub_->broadcast(id_, msg); }
    std::vector<std::vector<u8>> receive() override { return hub_->drain(id_); }

private:
    LoopbackHub* hub_;
    int id_;
};

/// Cross-platform (POSIX + Winsock) TCP transport. A host binds/listens and
/// relays each received message to the other peers (star topology, matching the
/// loopback broadcast semantics); clients connect to the host. Length-prefixed
/// framing; sends block, receive is non-blocking (select with zero timeout).
/// TCP's reliable, ordered delivery suits latency-tolerant lockstep; a
/// UDP+reliability transport is a possible performance follow-up.
class TcpTransport : public INetTransport {
public:
    /// Bind + listen on the given port (0 = ephemeral; see port()).
    static std::unique_ptr<TcpTransport> host(u16 port);
    /// Connect to a host at address:port.
    static std::unique_ptr<TcpTransport> join(const std::string& address, u16 port);

    ~TcpTransport() override;
    TcpTransport(const TcpTransport&) = delete;
    TcpTransport& operator=(const TcpTransport&) = delete;

    void broadcast(const std::vector<u8>& msg) override;
    std::vector<std::vector<u8>> receive() override;

    /// Host: accept any pending peer connections. Returns peers connected now.
    int poll_connections();
    int peer_count() const;
    u16 port() const;   // actual bound port (useful when host(0))
    bool ok() const;    // false if the socket setup failed

private:
    struct Impl;
    explicit TcpTransport(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace osc::sim
