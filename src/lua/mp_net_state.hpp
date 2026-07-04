#pragma once

#include "core/types.hpp"

#include <memory>
#include <string>
#include <vector>

namespace osc::sim {
class INetTransport;
class TcpTransport;
class MuxTransport;
class LockstepSession;
class SimState;
} // namespace osc::sim

namespace osc::lua {

class LanLobby;

// Process-wide multiplayer network state. Populated by the lobby HostGame /
// JoinGame bindings (which create the TcpTransport, wrap it in a MuxTransport,
// and start a LanLobby) and consumed by the game loop: the lobby phase syncs the
// config + seed and fires a launch barrier; at launch the LockstepSession is
// built over the mux game channel and drives the sim. One active networked
// session at a time; when no transport has been set up the game stays in
// ordinary single-player.
struct MpNetState {
    enum class Role { None, Host, Join };
    Role role = Role::None;
    std::string join_address = "127.0.0.1";
    osc::u16 port = 47624;
    osc::u32 local_source = 0;                 // host = 0, first client = 1
    std::vector<osc::u32> all_sources{0, 1};
    osc::u64 seed = 0xC0FFEE1234567890ull;      // shared RNG seed (host chooses)
    std::unique_ptr<osc::sim::MuxTransport> mux; // owns the TcpTransport
    osc::sim::TcpTransport* host_tcp = nullptr;  // non-owning alias; host only
    std::unique_ptr<LanLobby> lobby;             // lobby-phase handshake
    std::unique_ptr<osc::sim::LockstepSession> session; // built at game launch
    bool transport_ready = false;              // a transport exists, awaiting launch

    bool active() const { return session != nullptr; }
    void reset();
};

// Accessor for the single process-wide instance.
MpNetState& mp_net_state();

// Create a TcpTransport for the lobby (host binds+listens; join connects).
// Called by the HostGame / JoinGame moho bindings. Returns false on failure.
bool mp_begin_host(osc::u16 port);
bool mp_begin_join(const std::string& address, osc::u16 port);

// Host only: accept any pending peer connections; returns peers connected now.
int mp_poll_connections();

// Drain the underlying transport into the mux channels once, and (host) accept
// pending connections. Call once per frame while a transport exists.
void mp_pump();

// The active lobby handshake, or nullptr if no LAN transport was set up.
LanLobby* mp_lobby();

// At game launch: if a transport is ready, build the LockstepSession over the
// mux game channel, install SimState's command sink, and seed the sim (making it
// multiplayer). No-op in single-player. Returns true if a session was attached.
bool mp_attach_session(osc::sim::SimState& sim);

// Tear down any active lobby + session + transport (game end / return to lobby).
void mp_teardown();

} // namespace osc::lua
