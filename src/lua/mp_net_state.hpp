#pragma once

#include "core/types.hpp"

#include <memory>
#include <string>
#include <vector>

namespace osc::sim {
class INetTransport;
class TcpTransport;
class LockstepSession;
class SimState;
} // namespace osc::sim

namespace osc::lua {

// Process-wide multiplayer network state. Populated by the lobby HostGame /
// JoinGame bindings (which create the TcpTransport) and consumed by the game
// loop at launch (which builds the LockstepSession over that transport and
// drives the sim through it). One active networked session at a time; when no
// transport has been set up the game stays in ordinary single-player.
struct MpNetState {
    enum class Role { None, Host, Join };
    Role role = Role::None;
    std::string join_address = "127.0.0.1";
    osc::u16 port = 47624;
    osc::u32 local_source = 0;                 // host = 0, first client = 1
    std::vector<osc::u32> all_sources{0, 1};
    std::unique_ptr<osc::sim::INetTransport> transport; // Host/JoinGame create it
    osc::sim::TcpTransport* host_tcp = nullptr;         // non-owning; host only
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

// At game launch: if a transport is ready, build the LockstepSession over it
// and install SimState's command sink (making the sim multiplayer). No-op in
// single-player. Returns true if a session was attached.
bool mp_attach_session(osc::sim::SimState& sim);

// Tear down any active session + transport (game end / return to lobby).
void mp_teardown();

} // namespace osc::lua
