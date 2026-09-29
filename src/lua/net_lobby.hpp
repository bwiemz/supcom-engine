#pragma once

// A networked lobby object (M218a): what InternalCreateLobby makes for the
// "TCP" and "UDP" protocols. The scripts' lobbyComm object carries a
// LobbyNet; its methods go through it, and the frame's pump turns what the
// network brings into the object's callbacks, as Moho's CLobby does:
// ConnectionToHostEstablished, EstablishedPeers, DataReceived (with SenderID
// and SenderName), PeerDisconnected, Ejected, ConnectionFailed. The "None"
// protocol keeps the single-player loopback in bindings/ui/lobby.cpp.

#include "core/types.hpp"

#include <optional>
#include <string>

struct lua_State;

namespace osc::lua {

struct NetLobby;

/// The networked lobby the object at `idx` is, or null (a loopback one).
NetLobby* net_lobby_of(lua_State* L, int idx);

/// Make the object at `idx` a networked lobby: its protocol ("UDP" or
/// "TCP"), `port` to host on (0: any), `max_connections` players who may
/// join, its player's name, and the uid a matchmaking client gave the
/// player, if one did (M220b).
void make_net_lobby(lua_State* L, int idx, const std::string& protocol, u16 port,
                    u32 max_connections, const std::string& player_name,
                    std::optional<u32> player_uid = std::nullopt);

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
int net_lobby_DisconnectFromPeer(lua_State* L, NetLobby& lobby);
int net_lobby_MakeValidPlayerName(lua_State* L, NetLobby& lobby);
int net_lobby_LaunchGame(lua_State* L, NetLobby& lobby);
int net_lobby_Destroy(lua_State* L, NetLobby& lobby);
int net_lobby_DebugDump(lua_State* L, NetLobby& lobby);

// The discovery service (M218b): InternalCreateDiscoveryService's object.
// It asks the LAN for games every two seconds; the frame's pump brings
// GameFound(index, config), GameUpdated(index, config) and RemoveGame(index).
// A hosting lobby answers with its scripts' GameConfigRequested().
struct NetDiscovery;

/// The discovery service the object at `idx` is, or null.
NetDiscovery* net_discovery_of(lua_State* L, int idx);
/// Make the object at `idx` a discovery service.
void make_net_discovery(lua_State* L, int idx);
int net_discovery_GetGameCount(lua_State* L, NetDiscovery& discovery);
int net_discovery_Reset(lua_State* L, NetDiscovery& discovery);
int net_discovery_Destroy(lua_State* L, NetDiscovery& discovery);

/// Where discovery asks and hosts listen: 255.255.255.255 and port 15000,
/// FA's (tests ask the loopback, on a port of their own).
void set_lan_discovery(const std::string& broadcast_address, u16 port);

/// Each frame: the networked lobbies and discovery services of state `L`
/// take what has arrived and call their objects' callbacks. `now_ms`: a
/// monotonic clock.
void pump_net_lobbies(lua_State* L, i64 now_ms);

/// A state about to close: its lobbies and discovery services go with it
/// (their sockets close).
void close_net_lobbies(lua_State* L);

/// A monotonic clock in milliseconds, for pump_net_lobbies.
i64 net_lobby_clock_ms();

} // namespace osc::lua
