#pragma once

// A networked lobby object (M218a): what InternalCreateLobby makes for the
// "TCP" and "UDP" protocols. The scripts' lobbyComm object carries a
// LobbyNet; its methods go through it, and the frame's pump turns what the
// network brings into the object's callbacks, as Moho's CLobby does:
// ConnectionToHostEstablished, EstablishedPeers, DataReceived (with SenderID
// and SenderName), PeerDisconnected, Ejected, ConnectionFailed. The "None"
// protocol keeps the single-player loopback in bindings/ui/lobby.cpp.

#include "core/types.hpp"

#include <string>

struct lua_State;

namespace osc::lua {

struct NetLobby;

/// The networked lobby the object at `idx` is, or null (a loopback one).
NetLobby* net_lobby_of(lua_State* L, int idx);

/// Make the object at `idx` a networked lobby: `port` to host on (0: any),
/// `max_connections` players who may join, and its player's name.
void make_net_lobby(lua_State* L, int idx, u16 port, u32 max_connections,
                    const std::string& player_name);

// The methods, for a networked lobby object at 1 (its arguments after it).
int net_lobby_HostGame(lua_State* L, NetLobby& lobby);
int net_lobby_JoinGame(lua_State* L, NetLobby& lobby);
int net_lobby_SendData(lua_State* L, NetLobby& lobby);
int net_lobby_BroadcastData(lua_State* L, NetLobby& lobby);
int net_lobby_GetPeers(lua_State* L, NetLobby& lobby);
int net_lobby_GetPeer(lua_State* L, NetLobby& lobby);
int net_lobby_GetLocalPlayerID(lua_State* L, NetLobby& lobby);
int net_lobby_GetLocalPlayerName(lua_State* L, NetLobby& lobby);
int net_lobby_GetLocalPort(lua_State* L, NetLobby& lobby);
int net_lobby_IsHost(lua_State* L, NetLobby& lobby);
int net_lobby_EjectPeer(lua_State* L, NetLobby& lobby);
int net_lobby_MakeValidPlayerName(lua_State* L, NetLobby& lobby);
int net_lobby_LaunchGame(lua_State* L, NetLobby& lobby);
int net_lobby_Destroy(lua_State* L, NetLobby& lobby);
int net_lobby_DebugDump(lua_State* L, NetLobby& lobby);

/// Each frame: the networked lobbies of state `L` take what has arrived and
/// call their objects' callbacks. `now_ms`: a monotonic clock.
void pump_net_lobbies(lua_State* L, i64 now_ms);

/// A state about to close: its lobbies go with it (their sockets close).
void close_net_lobbies(lua_State* L);

/// A monotonic clock in milliseconds, for pump_net_lobbies.
i64 net_lobby_clock_ms();

} // namespace osc::lua
