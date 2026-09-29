#pragma once

// A game lobby's network (M218a): what Moho's CLobby does for FA's lobby
// scripts, over TCP. The host listens and is uid 0; each player who joins
// gets the next uid (1, 2, ...) and a name made unique. Peers reach each
// other through the host, which relays their messages: a star, where Moho
// meshes its peers, but the same lobby for the scripts (every peer reaches
// every other). Lua is the binding's business; this carries bytes.
//
// Messages are framed as the lockstep's are (a u32 length, extract_wire_
// frames); each is a type byte, then its fields.

#include "core/types.hpp"
#include "sim/net_transport.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace osc::sim {

/// Another player in the lobby, as this side knows them.
struct LobbyPeer {
    u32 uid = 0;
    std::string name;
    /// The last ping's round trip, in ms. A client knows only its host's;
    /// the other peers' are the host's too (their messages come through it).
    u32 ping_ms = 0;
    /// When anything last came from them (their connection; for a client,
    /// the host's), on poll's clock; -1 if nothing has.
    i64 last_heard_ms = -1;
};

/// Something poll() found.
struct LobbyEvent {
    enum class Kind : u8 {
        ConnectedToHost,  ///< welcomed: the local uid and name, the host's uid
        ConnectionFailed, ///< the host couldn't be reached, or was lost ("HostLeft")
        Ejected,          ///< refused ("LobbyFull") or kicked (the host's reason)
        PeerJoined,       ///< uid, name
        PeerLeft,         ///< uid, name
        Data,             ///< from uid (and name): payload
        PeerEstablished,  ///< the host's: player uid reaches everyone it knows of
    };
    Kind kind{};
    u32 uid = 0;
    std::string name;
    std::string reason;
    std::vector<u8> payload;
};

class LobbyNet {
public:
    /// Everyone: this uid (an addressed message's broadcast target).
    static constexpr u32 kEveryone = 0xFFFFFFFFu;
    /// How long a join waits for the host to answer, in ms.
    static constexpr i64 kJoinTimeoutMs = 10000;
    /// How often each side pings the other, in ms.
    static constexpr i64 kPingIntervalMs = 1000;

    /// `max_connections`: the players who may join (Moho's maxConnections;
    /// one more is refused with "LobbyFull").
    LobbyNet(std::string local_name, u32 max_connections);
    ~LobbyNet();
    LobbyNet(const LobbyNet&) = delete;
    LobbyNet& operator=(const LobbyNet&) = delete;

    /// The uid a matchmaking client gave this player (GPGNet, M220b), before
    /// hosting or joining: the host takes it as its own, and a joiner asks
    /// the host for it (one taken is refused, "UidTaken"). Without it, the
    /// host is 0 and numbers the players who join 1, 2, ...
    void set_local_uid(u32 uid);

    /// Listen on `port` (0: any free one) as the host (uid 0, or the one
    /// set). `hosted_time`
    /// goes to each player in their welcome (FA seeds the game with it).
    /// False if the port can't be listened on.
    bool host(u16 port, u64 hosted_time);
    /// Join the host at `address` (dotted IPv4) and `port`. The connection
    /// completes, and the welcome comes, through poll(). False if `address`
    /// is no address or no socket can be made.
    bool join(const std::string& address, u16 port);

    /// Send `payload` to one peer, or to kEveryone.
    void send(u32 to, const std::vector<u8>& payload);
    void broadcast(const std::vector<u8>& payload) { send(kEveryone, payload); }
    /// The host: remove a player, telling them `reason`. False for no such
    /// player, or not hosting.
    bool eject(u32 uid, const std::string& reason);

    /// A client, once its own part of joining is done (the scripts have had
    /// its ConnectionToHostEstablished, and sent what they send then): it
    /// tells the host it reaches everyone it knows of, as Moho's clients
    /// report the peers they have established (M220b). The host hears it
    /// after the client's data.
    void report_established();

    /// A launched game's lockstep frames (M218c), apart from the scripts'
    /// data: to every other player (the host relays a client's).
    void send_game(const std::vector<u8>& payload);
    /// The game frames poll() has read since the last call, in order.
    std::vector<std::vector<u8>> take_game();
    /// The host, once the game starts: no more joins (the port stops
    /// listening, as Moho's connector goes to the game).
    void stop_joining();

    /// Accept, read, relay and keep alive; what happened since the last
    /// call. `now_ms`: a monotonic clock in milliseconds.
    std::vector<LobbyEvent> poll(i64 now_ms);

    bool hosting() const;
    /// A client the host has welcomed (and not lost).
    bool joined() const;
    u32 local_uid() const;
    const std::string& local_name() const;
    u32 host_uid() const;
    /// The host's listening port, or the port joined.
    u16 port() const;
    /// The host's, or (a client's) from its welcome.
    u64 hosted_time() const;
    /// The other players (never this side).
    const std::vector<LobbyPeer>& peers() const;
    const LobbyPeer* peer(u32 uid) const;

    /// Moho's MakeValidPlayerName: at most 24 characters (none: "Player"),
    /// and a number appended while the name is another player's (not
    /// `uid`'s), case aside.
    std::string valid_player_name(u32 uid, const std::string& name) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// A launched game's connections (M218c): the lobby's, carrying the
/// lockstep's frames, as Moho's LaunchGame hands its peers' connections to
/// the game. Receiving polls the lobby, which keeps its keepalive going;
/// what the lobby reports (a player gone, late chat) is only logged, since
/// the lockstep notices a silent player itself.
class LobbyGameTransport : public INetTransport {
public:
    /// `clock`: poll's monotonic milliseconds.
    LobbyGameTransport(std::unique_ptr<LobbyNet> net, std::function<i64()> clock);
    void broadcast(const std::vector<u8>& msg) override;
    std::vector<std::vector<u8>> receive() override;
    const LobbyNet& net() const { return *net_; }

    /// The game's messages outside the lockstep (chat, M218d): to one
    /// player, and what came since the last call (their Data events).
    void send_data(u32 to, const std::vector<u8>& payload) { net_->send(to, payload); }
    /// The host: a player the game dropped is cut off (M218e), as Moho
    /// closes an ejected client's connection.
    void disconnect(u32 uid) {
        if (net_->hosting()) net_->eject(uid, "KickedByHost");
    }
    std::vector<LobbyEvent> take_data();

private:
    std::unique_ptr<LobbyNet> net_;
    std::function<i64()> clock_;
    std::vector<LobbyEvent> data_;
};

} // namespace osc::sim
