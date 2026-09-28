# M218: FA's LAN lobby, over the network

## Why

FA's multiplayer lobby is retail Lua (`lua/ui/lobby/lobby.lua`,
`lobbyComm.lua`, `gameselect.lua`, `gamecreate.lua`) talking to two engine
objects:
- a **lobby** (`InternalCreateLobby`, `moho.lobby_methods`): host or join,
  the peers, the scripts' messages (`SendData`, `BroadcastData`), eject, and
  the launch;
- a **discovery service** (`InternalCreateDiscoveryService`) that finds the
  games hosted on the LAN.

The scripts do the rest themselves: slots, factions, teams, colours, chat,
options, ready states. So with the two objects real, retail's own lobby
screen is the multiplayer lobby.

The engine's lobby object loops back to itself. That's right for
single-player (the "None" protocol), but a LAN lobby ("UDP") never reaches
another machine. The engine's own LAN path (`LanHost`/`LanJoin`, a dialog
of its own, a fixed handshake for the map and seed) goes around the retail
lobby. The discovery service is a stub, and the retail scripts' Steam-build
calls aren't bound.

The rules come from faf-re (`CLobby.cpp`, `CDiscoveryService.cpp`,
`SPeer.cpp`, `LuaObject.cpp`) and the retail scripts.

## The rules

### The lobby object
- `InternalCreateLobby(class, "None"|"TCP"|"UDP", port (0: any),
  maxConnections, playerName, uid, natProvider)`.
- **HostGame:** the host is uid 0, and `Hosting()` follows. Retail passes a
  second argument (friends only); it is ignored.
- **JoinGame(address "a.b.c.d:port", name, uid):**
  - a joiner gets the next uid (1, 2, ...) and a name made unique (24
    characters, case aside; 1, 2, ... appended);
  - then `ConnectionToHostEstablished(myId, myName, hostId)`;
  - a full lobby (maxConnections joined) refuses: `Ejected("LobbyFull")`.
- **Ids are strings** everywhere the scripts see them. They compare them with
  `table.find`.
- **GetPeers / GetPeer:** `{name, id, status, ping, quiet,
  establishedPeers}`. The local player is never listed. `quiet` is the ms
  since anything came. The scripts eject a peer quiet for over 10 s.
- **The callbacks:**
  - `EstablishedPeers(id, {ids})`: who each peer reaches;
  - `DataReceived(data)`, with `SenderID` and `SenderName` added;
  - `PeerDisconnected(name, id)`: the scripts' order;
  - `Ejected(reason)`: "KickedByHost" or the host's reason;
  - `ConnectionFailed("HostLeft")`.
- **LaunchGame** isn't networked. The host broadcasts `{Type='Launch',
  GameInfo=...}` through the scripts, and each peer calls `LaunchGame`
  itself on the same config.

### Discovery
- The service broadcasts an empty request to 255.255.255.255:15000 every
  2 s.
- A hosting lobby listens on UDP 15000. It answers with its scripts'
  `GameConfigRequested()` table and its port.
- The service reports:
  - `GameFound(index, config)` for a new game, with `Address`, `Hostname`
    and `Protocol` added, and `GameUpdated` for a known one;
  - `RemoveGame(index)` after 5 s without a reply.
- The index is the 0-based place in its list (gameselect stores
  `games[index + 1]`).

### The launch
- `LaunchGame(cfg)` on each peer uses:
  - `GameOptions.ScenarioFile`;
  - the scenario's FFA armies, filled by `PlayerOptions` slot by slot:
    `LaunchFailed("NoConfig")` without an FFA team, `"StartSpots"` with too
    many players;
  - each human's `OwnerID`, and the `Observers`.
- One command source per owner. The local army is the one whose `OwnerID`
  is the local uid.
- The seed is the host's `hostedTime`, which each player had in their
  welcome.
- The lobby's connections become the game's: each tick waits for every
  client.

## The engine

### Transport
- The lobby is TCP, where Moho's is its own reliable UDP. Cross-play with
  FA's executable is not a goal (it would need its exact protocol).
- The peers are a star through the host, which relays, where Moho meshes
  them. To the scripts it's the same lobby: every peer reaches every other,
  and `establishedPeers` says so.

### Slices
- **M218a (this PR):**
  - `sim::LobbyNet`: the lobby's network, Lua-free. Join and welcome,
    refusal, uids and names, data addressed or to all, relayed, eject,
    departures, ping and quiet, a non-blocking join with a 10 s limit. Its
    messages are framed as the lockstep's are.
  - `lua::lobby_wire`: the scripts' data as bytes. It uses Moho's tags,
    with doubles for numbers; cycles, functions and depth over 32 travel as
    nil.
  - `lua::net_lobby`: networked lobby objects ("TCP"/"UDP"). Their methods
    go through `LobbyNet`, and a per-frame pump makes the callbacks.
    `Hosting` comes at the next pump, after the scripts' setup, and a
    callback may destroy its own lobby. A UI state's lobbies close with it.
  - "None" keeps the loopback.
- **M218b:**
  - the discovery service (UDP broadcast on 15000);
  - the Steam-build stubs retail's gameselect calls
    (`InternalStartSteamDiscoveryService`, `IsSignedInToSteam`,
    `moho.steam_discovery_service_methods`);
  - `ValidateIPAddress`.

  Retail's Multiplayer → LAN screen then lists, hosts and joins games.
- **M218c:** `LaunchGame` into a lockstep game over the lobby's
  connections, one source per owner, seeded by `hostedTime`. A two-process
  test plays retail's lobby to a game. `LanHost`/`LanJoin` and their dialog
  then go.

## Tests (M218a)
- **Unit (`test_lobby_net`, over the loopback):**
  - joins, uids, unique names, the peer lists;
  - data to one player, to all, and from the host, relayed;
  - a full lobby's refusal, and eject;
  - a player leaving, and the host;
  - no address, and a port no longer listening;
  - pings.
- **Unit (`test_lobby_wire`):**
  - retail's message shapes arrive whole (nested tables, lists, boolean
    keys, exact numbers, strings with NULs);
  - functions, cycles and depth are cut;
  - malformed bytes decode to nil and leave the stack as it was.
- **Unit (`test_net_lobby`, Lua on the moho bindings):**
  - a lobby class like retail's lobbyComm hosts and is joined, through
    every method and callback above;
  - data with SenderID and SenderName;
  - eject, departures, and a failed join;
  - LaunchGame failing (until M218c);
  - "None" still loops back.
