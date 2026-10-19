#pragma once

// The game's clients, as Moho's user side gives them to the UI (M218d):
// GetSessionClients, the command sources' names, and chat. In a lobby's
// game each owner is a client (numbered as its command source), the local
// one or another over the lobby's connections; a single-player game has
// one, local. SessionSendChatMessage sends to the clients chosen, the local
// one through a queue, and each frame's pump hands what came to
// gamemain.lua's ReceiveChat(sender, msg), as Moho's client manager does.

#include "core/types.hpp"

#include <cstddef>
#include <vector>

struct lua_State;

namespace osc::lua {

/// Whether the game is a network game: a lobby's ("UDP"/"TCP").
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

/// A matchmaking client's EjectPlayer (GPGNet, M220b): the game's client
/// whose lobby uid is `uid`, ejected as EjectSessionClient ejects one.
enum class EjectByUid : u8 {
    Ejected,
    NoGame,       ///< no network game
    NoSuchClient, ///< no client has that uid
    Local,        ///< it is this client
};
EjectByUid eject_session_uid(u32 uid);

/// Each frame of a network game: retail's disconnect dialog looks at the
/// clients (uimain.UpdateDisconnectDialog, as Moho's session calls it).
void pump_disconnect_dialog(lua_State* L);

/// SessionRequestPause / SessionResume (M218f): a command to the sim
/// (SimState::request_pause), through the lockstep in a network game, and
/// gamemain.OnUserPause(bool) at once for this player, as Moho's. False for a
/// replay or no game (the caller pauses the client).
bool session_request_pause(lua_State* L);
bool session_resume(lua_State* L);
/// WLD_SingleStep: paused, one beat runs its tick.
bool session_single_step(lua_State* L);

/// SessionIsPaused. False (`paused` untouched) for a replay or no game.
bool session_is_paused(lua_State* L, bool& paused);

/// SetGameSpeed, WLD_GameSpeed and the WLD_*SimRate commands in a network
/// game (M218i): a player asks the lockstep for the speed, which changes
/// only if the lobby made it adjustable; an observer can't. False (nothing
/// done) in single-player, where the caller sets its own.
bool session_request_speed(i32 rate);

/// GetGameSpeed in a network game: the game's speed. False in single-player.
bool session_speed(i32& rate);

/// Each frame of a network game: uimain.NoteGameSpeedChanged(client, speed)
/// for each change the lockstep applied, as Moho's client manager calls it.
void pump_speed_changes(lua_State* L);

void show_desyncs(lua_State* L, osc::u32 beat, const std::vector<osc::u32>& sources);
void pump_desyncs(lua_State* L);

/// Each frame of a game: gamemain.OnPause(pausedBy,
/// timeoutsRemaining) as a pause starts, OnResume() as it ends (Moho's
/// Sync.PausedBy).
void pump_pause_state(lua_State* L);

/// The local client's chat not yet delivered, dropped: its game is over
/// (a UI state resetting, a network game torn down), so none of it may
/// reach the next.
void reset_session_chat();

} // namespace osc::lua
