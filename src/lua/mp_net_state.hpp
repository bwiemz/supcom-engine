#pragma once

#include "core/types.hpp"

#include <memory>
#include <string>
#include <vector>

namespace osc::sim {
class INetTransport;
class LobbyGameTransport;
class LockstepSession;
class SimState;
} // namespace osc::sim

namespace osc::lua {

/// A client of a lobby's game (M218d): one per owner, by command source.
struct SessionClient {
    osc::u32 uid = 0; ///< its lobby uid
    std::string name; ///< its lobby name
};

// Process-wide multiplayer network state. A lobby's LaunchGame (M218c) hands
// it the lobby's connections; the game's launch builds the LockstepSession
// over them, and it drives the sim. One networked game at a time; without one
// the game is single-player.
struct MpNetState {
    osc::u32 local_source = 0; ///< this player's command source
    std::vector<osc::u32> all_sources;
    osc::u64 seed = 0xC0FFEE1234567890ull;              ///< the sim's seed (the host's hosted time)
    std::unique_ptr<osc::sim::LockstepSession> session; ///< built at game launch
    bool transport_ready = false; ///< a lobby's game awaits its launch, or plays
    /// The lobby's connections, carrying the lockstep; source s plays army
    /// source_armies[s] (-1: an observer's).
    std::unique_ptr<osc::sim::INetTransport> lobby_transport;
    osc::sim::LobbyGameTransport* lobby_game = nullptr; ///< non-owning: lobby_transport
    std::vector<osc::i32> source_armies;
    /// The clients, by client index (= command source), M218d
    std::vector<SessionClient> clients;

    bool active() const { return session != nullptr; }
    /// The army `source` plays (-1: none).
    osc::i32 army_of(osc::u32 source) const;
    /// The army this player plays (-1: an observer).
    osc::i32 local_army() const { return army_of(local_source); }
    void reset();
};

// Accessor for the single process-wide instance.
MpNetState& mp_net_state();

// A lobby's LaunchGame (M218c): the game will play over `transport`, the
// lobby's connections. Command sources are 0, 1, ... (Moho's: the humans'
// owners by slot, then the observers); `armies[s]` is source s's army (-1:
// an observer's), and `local_source` this player's. Replaces any game set
// up before.
/// `clients[s]`: source s's owner and name.
void mp_begin_lobby_game(std::unique_ptr<osc::sim::LobbyGameTransport> transport,
                         osc::u32 local_source, std::vector<osc::i32> armies,
                         std::vector<SessionClient> clients, osc::u64 seed);

// At game launch: if a lobby's game is ready, build the LockstepSession over
// its connections, install SimState's command sink, and seed the sim (making
// it multiplayer). No-op in single-player. Returns true if a session was
// attached.
bool mp_attach_session(osc::sim::SimState& sim);

// A lobby's game dropped `source` (a timeout or an eject): the host closes
// its connection, as Moho closes an ejected client's (M218e).
void mp_disconnect_source(osc::u32 source);

// Tear down the session and the lobby's connections (game end / return to lobby).
void mp_teardown();

} // namespace osc::lua
