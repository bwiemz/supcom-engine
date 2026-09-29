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
- **M218a (#186):**
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
- **M218b (#187):**
  - the discovery service (UDP broadcast on 15000);
  - the Steam-build stubs retail's gameselect calls
    (`InternalStartSteamDiscoveryService`, `IsSignedInToSteam`,
    `moho.steam_discovery_service_methods`);
  - `ValidateIPAddress`.

  Retail's Multiplayer → LAN screen then lists, hosts and joins games.
- **M218c (#192):**
  - `LaunchGame` checks the config as Moho's does (the scenario read as
    retail's `MapUtil.LoadScenario` reads it; "NoConfig", "StartSpots").
  - Command sources are numbered as Moho numbers them: 0, 1, ... per
    owner, the humans by slot and then the observers (so
    `SessionGetLocalCommandSource` and the source names keep their
    meaning). A human's army is Moho's: the slots taken, in order (players
    in slots 1 and 5 play armies 0 and 1). An observer's source plays none.
  - `LobbyNet` carries the lockstep's frames in a `Game` message the host
    relays, apart from the scripts' data; `LobbyGameTransport` hands the
    lobby's connections to the `LockstepSession`. The host takes no more
    joins.
  - `GameLaunched` comes last, since retail's destroys the lobby.
  - A lobby is known by its UI state (its registry), not the thread that
    calls it: retail calls `LaunchGame` from its countdown thread and
    `GetPeer` from its keepalive thread.
- **M218d (#194):** the game's session, as retail's in-game UI sees it
  (Moho's rules in memory: moho-session-semantics):
  - `GetSessionClients`: one client per owner, numbered as its command
    source: `name` (its lobby name), `uid` (a string), `connected`,
    `ping`, `quiet` (from the lobby's connections), `local`, `maxSP`,
    `authorizedCommandSources`, `ejectedBy`. A single-player game has one,
    local; without a game, nil.
  - `SessionIsMultiplayer` (a lobby's game); `SessionGetCommandSourceNames`
    the clients' names; `GetArmiesTable`'s `authorizedCommandSources`.
  - `SessionGetScenarioInfo().Options`: the game's (the sim's
    `ScenarioInfo.Options`, a lobby's `GameOptions`). Retail's tabs reads
    `Timeouts` in a network game; without it the game UI half-built.
  - Chat: `SessionSendChatMessage([clients,] msg)` to everyone (the local
    client too) or those chosen, with Moho's errors (a client the game
    hasn't, over 1024 bytes). The local one's through a queue, others'
    over the lobby's connections (the scripts' Data, free once the game
    starts). Each frame's pump calls `gamemain.ReceiveChat(sender, msg)`.
  - A network game's pause is refused (as Moho's with no timeouts left):
    pausing alone would stop this player's frames and drop them.
- **M218e (#195):** ejecting players, and retail's disconnect dialog:
  - `EjectSessionClient(i)` (Moho's errors: an index the game hasn't, the
    local client) starts the lockstep's drop of the client's source, the
    one a timeout starts: the survivors agree its last frame and defeat
    its army on the same tick. `LockstepSession::eject`, `ejectors` (who
    reported it: `ejectedBy`) and `ejected` (a survivor reports this peer
    dropped).
  - A dropped (or being dropped) client is `connected = false`, with no
    `authorizedCommandSources`, and reads as having no connection (ping 0,
    quiet -1), as Moho closes an ejected client's; the host closes its
    lobby connection too.
  - Each frame of a network game calls retail's
    `uimain.UpdateDisconnectDialog()`, as Moho's session does: the dialog
    shows for a quiet player (over 5 s) or one gone and not yet dropped by
    all still in, and closes once they have.
  - An ejected player's game is over: once a survivor reports it dropped,
    its session ticks no further and drops no one. Cut off from the host
    (the star's hub), it would otherwise time every other player out and
    play on alone; Moho's ejected client waits, its dialog showing everyone
    gone.
  - Found on the way: a drop defeated army `source`, where a lobby's game
    numbers sources by owner (M218c); it now defeats the source's own army,
    and none for an observer's.
  - The two-process tests' ports (29741, 29742) sit below the OSes'
    ephemeral ranges: in them, another test's socket took one and the host
    couldn't listen. When one process fails, `lan_game.py` stops the other.
- **M218f (this PR):** a network game's pause, by source, as Moho's:
  - The lockstep's frames are its ticks' (frame F confirms tick F), so a
    pause can't be frames that don't tick: resuming would run every tick
    they confirmed at once. Instead:
  - **Pausing** is a command (`SessionRequestPause` submits it, from the
    local source): every peer runs it on the same tick T, as Moho's sim
    runs `CMDST_RequestPause`. It is taken unless the game is paused
    already or its source's timeouts are spent; a taken one spends one
    (Moho's: players have the lobby's `Timeouts`, '0', '3' or '-1'
    unlimited; observers none). The sim finishes tick T and holds.
  - **While paused**, peers send no frames, tick no further and drop no
    one (no frames flow), but read what comes.
  - **Resuming** is a session message carrying the pause's serial
    (`SessionResume`, from any source, as Moho's): it can't be a command,
    since no tick runs to carry it. Every peer is held between T and T+1,
    so each resumes at the same place. A peer still on its way to T when
    the resume comes resumes the moment it pauses.
  - Outside a lockstep game (a replay playing one back) a pause resumes
    at once: no message would come.
  - The UI hears it as Moho's does: `gamemain.OnPause(pausedBy,
    timeoutsRemaining)` and `OnResume()` as the pause starts and ends
    (Moho's `Sync.PausedBy`), `OnUserPause(bool)` at once for the player
    who asked, and `SessionIsPaused` true while it holds.
- **M218g:** the fixed LAN handshake goes, now that retail's own screens
  host, find, join and launch games: the engine's "LAN Game" button and
  dialog on the main menu, the `LanHost`/`LanJoin`/`LanNetStatus` globals,
  `--lan-window-host`/`--lan-window-join`, and beneath them `LanLobby`, the
  `MuxTransport` it shared a connection with, and `TcpTransport`. A game's
  network is its lobby's connections, and nothing else.
  - The data-free CI pairs (`mp.*`) play over the lobby's connections: each
    hosts a lobby, joins it, launches as `LaunchGame` does
    (`mp_begin_lobby_game`, `mp_attach_session`) and plays minimal sims in
    lockstep: in sync, a divergence caught, a player gone mid-game dropped.

- **M218h:** a network game's pace. The roadmap's "RTT-adaptive command
  delay" turns out to be the lockstep's own shape: a peer sends a frame a
  round (a tick's time) and runs a tick once every peer's frame for it is
  in, so its frames lead its sim by the network's latency, and a local
  order, going into the frame being sent, waits just that long. What was
  missing is Moho's bound on it: its issue thread runs at most two
  seconds' worth of beats ahead of the one executed. Without it a peer
  whose sim can't keep 10 Hz fell further behind every second, and the
  others, whose drop timer counted how far their frames ran past its,
  dropped it after 30; so did a player who raised the game speed, whose
  rounds ran faster than everyone else's.
  - A peer sends at most 20 frames (`kMaxLead`, two seconds) past its sim;
    held there, it waits for the others, and the game runs at the slowest
    peer's pace. Held, it still sends a word each round (an alive message),
    so a slow peer held at the cap isn't taken for a dead one.
  - The drop timer counts rounds without a word from a peer (a frame or an
    alive message), not frames: held at the cap, frames stop, and a dead
    peer must still drop.
  - A round is a tick's time, whatever this player's speed: a network game
    runs at normal speed until the game's speed is negotiated (Moho's
    `CLIMSG_AdjustSimSpeed`, not done yet).
  - As before, the timer is armed by a peer's first frame: peers load the
    game at their own speed, and one slower to load mustn't drop. A peer
    that never sends one (it died loading) is left to the players, as in
    Moho: the disconnect dialog shows it, and they eject it.

- **M218i:** a network game's speed, as Moho's client manager negotiates
  it. The lobby's `GameSpeed` fixes it (`normal` +0, `fast` +4) or lets
  the players change it (`adjustable`). A player's `SetGameSpeed`,
  `WLD_GameSpeed` or `WLD_Increase/Decrease/ResetSimRate` goes to every
  peer as a message with a clock (Moho's `CLIMSG_AdjustSimSpeed`); each
  applies the newest, a tie going to the lower source, so all come to the
  same speed. An observer can't (Moho's `WLD_CanAdjustSimRate`). Each change
  reaches `uimain.NoteGameSpeedChanged(client, speed)` on every machine.
  - A round is a tick's time at that speed, so the lead cap and the drop
    timeout are counted in rounds scaled to it (their scale held to 0.1–10:
    the frame loop runs a few rounds a frame at most), keeping two and three
    seconds.
  - Not done: Moho also caps the speed at the slowest client's measured
    rate (`maxSP`); here the lead cap does the same work, waiting for it.
  - Single-player keeps its own speed, adjustable whatever the option
    (retail fixes it at `normal` unless the lobby says `adjustable`).

Two fixes the launch found go separately, since single-player has them
too:
- random spawn (FA's default) leaves the slots sparse (players in 1 and 5,
  say). Moho packs them, each army named for its slot. The session reads
  `PlayerOptions` with `luaL_getn`, so it loses armies;
- `GetArmiesTable`'s `faction` was the sim's 1-based index, where the UI
  indexes `factions.lua` with `faction + 1`.

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

## Tests (M218b)
- **Unit (`test_lan_discovery`, loopback, a made-up clock):**
  - a responder answers the broadcast: the game is found at index 0, then
    updated in place when asked again two seconds on;
  - no asking before two seconds;
  - two lobbies list in order, and both go after five silent seconds, each
    at index 0 as the list closes up;
  - Reset goes last first;
  - a second responder can't take the port;
  - an empty answer is still a game;
  - an empty datagram ahead of a request, or of an answer, hides neither.
- **Unit (`test_net_lobby`):**
  - a hosted lobby is found through the scripts' discovery object, its
    config carrying the scripts' description and `Address`, `Hostname` and
    `Protocol`;
  - Reset calls RemoveGame;
  - the Steam stubs; `ValidateIPAddress`.
- **`--lan-screen-test` (gate):** retail's `gameselect.lua` LAN screen,
  opened from the front end, finds and lists a game hosted in the same
  process, with no script error.

## Tests (M218c)
- **Unit (`test_lobby_net`):**
  - game frames reach every other player, a client's through the host, in
    order, and aren't the scripts' data;
  - once the game starts the host takes no more joins;
  - three sims play in lockstep over the lobby's connections, one client's
    order reaching the other through the host.
- **Unit (`test_net_lobby`):**
  - LaunchFailed: no scenario, "NoConfig", "StartSpots", and a player
    neither playing nor watching;
  - three players launch (the host, a player, an observer): the sources
    by slot then observers, their armies, the seed and the launch request.
    A game with gaps (slots 2, 5 and 7, its table iterating 7, 5, 2)
    packs its armies in slot order;
    The methods are called from a coroutine, as retail's threads call them;
  - `GameLaunched` destroys the lobby, and the connections carry the game.
- **`data.lan_game` (gate):** two processes play retail's `lobby.lua` to a
  game (host, join, ready, Launch and the countdown), then 150 ticks in
  lockstep with no desync. Each plays its own slot's army, both armies are
  human, and there's no script error. Spawn and factions are retail's
  defaults, random: the two armies are the slots taken, in order, and
  start apart, each at its slot's marker.

## Tests (M218d)
- **Unit (`test_net_lobby`):**
  - a single-player game: one local client and its fields; chat to all,
    to client 1 and to nobody comes back the next frame through
    `ReceiveChat`, from the player; Moho's refusals;
  - a lobby's game, as the host sees it: two clients by source, names,
    uids, local, connection, the source names; the pause refused; chat to
    Alice alone and to all reaches her over the lobby's connections, the
    host's own comes back to it, and hers reaches it from her.
- **`data.lan_game`:** in the game, each side sees the two clients by
  source, the source names, its own source and each army's; the host's
  pause is refused and the game plays on (both reach tick 150, no source
  dropped); its chat to everyone reaches both (the host's own, looped
  back).

## Tests (M218e)
- **Unit (`test_lockstep`):**
  - a player ejects another still playing: its vote first, then the other
    survivor's; both drop it on the same tick, in sync, its army defeated
    and not another's; the ejected one knows (`ejected`) and neither ticks
    on nor drops anyone; refusals (itself, no such player, twice);
  - a player alone with the one it ejects drops it at once;
  - a dropped player's own army is defeated, not the one numbered as its
    source, and a dropped observer defeats none.
- **Unit (`test_net_lobby`):** `EjectSessionClient`'s refusals in
  single-player; the dialog isn't updated there; in a lobby's game the host
  ejects Alice: she is disconnected, ejected by the host, without sources,
  and the dialog is updated each frame.
- **`data.lan_game_quit` (gate):** the joiner leaves at tick 120; the host
  drops it by agreement, defeats its army, shows retail's disconnect dialog
  and closes it, and plays on alone to tick 150 with no script error.

## Tests (M218f)
- **Unit (`test_lockstep`):**
  - a pause holds both peers on the tick it was taken, however long, with
    no one dropped (no frames flow); asking again while paused does
    nothing; the other player resumes it and both go on together, a tick a
    round (no burst), in sync;
  - a pause holds on its own tick though later frames are in;
  - a source's timeouts run out (spent on every peer) and its pause is then
    refused while the game plays on; another source's are unlimited;
  - a resume that reaches a peer before it has reached the pause releases
    it there, in sync;
  - outside a lockstep game (a replay) a pause resumes at once, its
    timeouts spent as in the game.
- **`data.lan_game`:** the host pauses at tick 100; no tick runs on either
  side while it holds; the joiner resumes it after 60 frames; each UI hears
  `OnPause` (the host's source, 2 of the lobby's 3 timeouts left) and
  `OnResume`; both still reach tick 150 in step. `data.lan_game_quit` too,
  before the joiner leaves.

## Tests (M218g)
- **`mp.lockstep_sync`, `mp.lockstep_desync_detected`,
  `mp.lockstep_peer_drop`** (two processes, no game data, CI): a lobby
  hosted and joined over TCP, the joiner seeded by the host's time from its
  welcome, launched, then 60 rounds in lockstep with the host's scripted
  orders. In sync, both reach the same checksum; with a local-only order on
  the host, both report the desync; with the joiner gone at round 20
  without a word, the host drops it (exactly one) and plays on.
- **Unit (`test_lobby_net`):** the host survives sending to a player gone
  without a goodbye (POSIX's SIGPIPE), and loses it. This was
  `TcpTransport`'s test; the lobby's connections share its send path.
- **Unit (`test_wire_framing`):** the framing tests, kept from
  `test_tcp_transport`.

## Tests (M218h)
- **Unit (`test_lockstep`, `[pace]`):**
  - a peer whose other has gone silent sends 20 frames past its sim, then
    only alive messages; the silent one still drops after the timeout;
  - over a line 4 rounds long, a local order waits 5 ticks (the latency
    and the frame it goes in) and the game still runs a tick a round; over
    one 30 rounds long (past the cap), the game slows, and no one drops;
  - a peer running a round in three of the other's sets the pace: the fast
    one reaches the cap, neither drops, and they end in step;
  - three peers, C sending a frame every 29 of A's rounds and B a round in
    seven: held at the cap, B would go quiet for 35 of A's rounds between
    frames; its alive messages keep it in, and no one drops.
- **`mp.lockstep_slow_peer`** (two processes, CI): the joiner takes 200 ms
  a round; the host, twice as fast, waits for it, drops no one, and both
  end in sync. The pairs' processes now answer for a second after their
  last round before closing: a socket closed with data unread resets the
  connection, and the other side lost its last frame.

## Tests (M218i)
- **Unit (`test_lockstep`, `[speed]`):** a fixed speed refuses a change; an
  adjustable one: a request applies at once for its asker and on the other
  once the message comes, both hearing it; two at the same clock settle on
  the lower source's, a newer one wins, one out of range is held to
  -10..+50, and one older than the applied (delayed on its way) changes
  nothing; at +10 a peer runs 200 frames ahead and drops a silent one after
  300 rounds, at -10 after 2 and 3.
- **Unit (`test_net_lobby`):** a lobby's game at `fast` is at +4 and
  refuses `SetGameSpeed`; at `Adjustable` (any case) a player's
  `SetGameSpeed(3)` is its speed and reaches `NoteGameSpeedChanged(1, 3)`;
  an observer's changes nothing.
- **`data.lan_game`, `data.lan_game_quit`:** the host's lobby makes the
  speed adjustable (retail's `SetGameOption`); the joiner, resuming the
  pause, asks for +2; both machines hear it (retail prints "LanJoiner:
  adjusting game speed to +2") and end at +2.
