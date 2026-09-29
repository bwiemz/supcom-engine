#pragma once

// The game's clients, as Moho's user side gives them to the UI (M218d):
// GetSessionClients, the command sources' names, and chat. In a lobby's
// game each owner is a client (numbered as its command source), the local
// one or another over the lobby's connections; a single-player game has
// one, local. SessionSendChatMessage sends to the clients chosen, the local
// one through a queue, and each frame's pump hands what came to
// gamemain.lua's ReceiveChat(sender, msg), as Moho's client manager does.

#include <cstddef>

struct lua_State;

namespace osc::lua {

/// Whether the game is a network game: a lobby's ("UDP"/"TCP"), or the
/// fixed LAN handshake's.
bool session_is_multiplayer();

/// How many clients the game has (0: no game).
size_t session_client_count(lua_State* L);

/// GetSessionClients(): nil without a game, else one table per client:
/// name, uid (a string), connected, ping, quiet, local, maxSP,
/// authorizedCommandSources, ejectedBy.
int push_session_clients(lua_State* L);

/// SessionGetCommandSourceNames() in a network game: the clients' names by
/// source. False (nothing pushed) in single-player.
bool push_network_source_names(lua_State* L);

/// SessionSendChatMessage([clients,] msg): to every client (the local one
/// too) or to those chosen (1-based client indices, one or a table). Lua
/// errors as Moho's: a client no game has, or a message over 1024 bytes.
/// Without a game it sends nothing.
void send_session_chat(lua_State* L);

/// Each frame: the chat that came, and the local client's own, to
/// gamemain.lua's ReceiveChat(sender, msg). Nothing while the game's
/// interface (gamemain) isn't loaded.
void pump_session_chat(lua_State* L);

/// EjectSessionClient(index) (M218e): Lua errors as Moho's for an index the
/// game hasn't or the local client; else the lockstep drops the client's
/// source, as the survivors agree.
void eject_session_client(lua_State* L);

/// Each frame of a network game: retail's disconnect dialog looks at the
/// clients (uimain.UpdateDisconnectDialog, as Moho's session calls it).
void pump_disconnect_dialog(lua_State* L);

/// SessionRequestPause / SessionResume in a network game (M218f): through
/// the lockstep (SimState::request_pause), and gamemain.OnUserPause(bool)
/// at once for this player, as Moho's. False in single-player (the caller
/// pauses the game locally).
bool session_request_pause(lua_State* L);
bool session_resume(lua_State* L);

/// SessionIsPaused in a network game: whether the lockstep's pause holds.
/// False (`paused` untouched) in single-player.
bool session_is_paused(lua_State* L, bool& paused);

/// Each frame of a network game: gamemain.OnPause(pausedBy,
/// timeoutsRemaining) as a pause starts, OnResume() as it ends (Moho's
/// Sync.PausedBy).
void pump_pause_state(lua_State* L);

/// The local client's chat not yet delivered, dropped: its game is over
/// (a UI state resetting, a network game torn down), so none of it may
/// reach the next.
void reset_session_chat();

} // namespace osc::lua
