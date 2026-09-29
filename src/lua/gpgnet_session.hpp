#pragma once

// The game's GPGNet link, carried out in the UI state (M220a), as Moho's
// CGpgNetInterface does: `/gpgnet host:port` connects to the matchmaking
// client; once connected the game says it is Idle; the client's commands
// make a lobby (through retail's onlineprovider.lua), host, join, and reach
// the lobby's peers; the scripts send the client theirs (GpgNetSend).

#include "sim/gpgnet.hpp"

#include <string>

struct lua_State;

namespace osc::lua {

class LuaState;

/// Connect to the client at `endpoint` ("host:port", the host dotted
/// IPv4). False if it isn't one, or no connection can be started.
bool gpgnet_attach(const std::string& endpoint);

/// Whether the game has a link to a client, up or coming up.
bool gpgnet_active();

/// What the link is.
enum class GpgNetState {
    None,   ///< never attached
    Linked, ///< connecting, or connected
    Closed, ///< it went down (or never came up)
};
GpgNetState gpgnet_state();

/// Each frame: what the link brought, carried out in the UI state `L`.
void pump_gpgnet(lua_State* L);

/// Send the client a command from the engine (a desync, the game's state),
/// if linked.
void gpgnet_send(const sim::GpgNetCommand& command);

/// GpgNetActive() and GpgNetSend(cmd, args...) in the UI state.
void register_gpgnet_bindings(LuaState& state);

/// The link closed and forgotten (tests; a run's end).
void gpgnet_detach();

} // namespace osc::lua
