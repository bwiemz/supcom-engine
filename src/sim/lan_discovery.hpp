#pragma once

// Finding the games hosted on the LAN (M218b), as Moho's CDiscoveryService
// does: a UDP broadcast asks every two seconds, and each hosting lobby's
// DiscoveryResponder answers with its port and its scripts' description of
// the game (GameConfigRequested). A game unheard for five seconds is gone.
// The answers' bytes are the scripts' data (lua::lobby_wire); this carries
// them.

#include "core/types.hpp"

#include <memory>
#include <string>
#include <vector>

namespace osc::sim {

/// The port hosts listen on, as FA's.
inline constexpr u16 kLanDiscoveryPort = 15000;

/// A game a host answered for.
struct DiscoveredGame {
    std::string address;  ///< "a.b.c.d:port": its lobby, as JoinGame takes it
    std::string hostname; ///< the host's address (dotted: no name lookup)
    u8 protocol = 0;      ///< the lobby's: 1 TCP, 2 UDP
    std::vector<u8> config;
    i64 last_heard_ms = 0;
};

/// A change in the list, by the game's 0-based place in it.
struct DiscoveryEvent {
    enum class Kind : u8 { Found, Updated, Removed };
    Kind kind{};
    u32 index = 0;
    DiscoveredGame game; ///< Found, Updated
};

class LanDiscovery {
public:
    static constexpr i64 kRequestIntervalMs = 2000;
    static constexpr i64 kExpiryMs = 5000;

    /// Ask at `broadcast_address` (255.255.255.255 on a LAN) on `port`.
    explicit LanDiscovery(std::string broadcast_address = "255.255.255.255",
                          u16 port = kLanDiscoveryPort);
    ~LanDiscovery();
    LanDiscovery(const LanDiscovery&) = delete;
    LanDiscovery& operator=(const LanDiscovery&) = delete;

    /// Open the socket. False if none can be made.
    bool open();
    /// Ask (every two seconds), read the answers, forget the silent.
    std::vector<DiscoveryEvent> poll(i64 now_ms);
    /// Forget every game, last first (Moho's Reset).
    std::vector<DiscoveryEvent> reset();
    size_t game_count() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// Who asked: where the answer goes.
struct DiscoveryAsker {
    u32 ip = 0; ///< network order
    u16 port = 0;
};

/// A hosting lobby's side: listens for the broadcasts and answers each.
class DiscoveryResponder {
public:
    DiscoveryResponder();
    ~DiscoveryResponder();
    DiscoveryResponder(const DiscoveryResponder&) = delete;
    DiscoveryResponder& operator=(const DiscoveryResponder&) = delete;

    /// Listen on `port`. False if it's taken (as Moho says: someone else
    /// must be hosting on this machine).
    bool open(u16 port = kLanDiscoveryPort);
    /// The port listened on (open(0): the one the system chose).
    u16 port() const;
    /// Those who asked since the last call.
    std::vector<DiscoveryAsker> poll();
    /// Tell `asker` of the game: its lobby's protocol and port, and its
    /// description.
    void answer(const DiscoveryAsker& asker, u8 protocol, u16 game_port,
                const std::vector<u8>& config);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace osc::sim
