# M220: the FAF client's link (GPGNet)

## Why

Most people who play Forged Alliance online today play through FAF, whose
client (with its ICE adapter) launches the game and drives it over GPGNet,
the link Moho kept from GPG's own matchmaking service. For the FAF client
to launch this engine in place of ForgedAlliance.exe, the engine must speak
GPGNet as Moho does: take `/gpgnet host:port`, connect, and carry out the
client's commands (make a lobby, host, join, connect to a peer, eject) while
telling it the game's state.

There is no FAF install here, and the FAF client can't run in CI; the tests
use a fake GPGNet client that speaks the protocol. What Moho does is read
from faf-re (`src/sdk/moho/net/CGpgNetInterface.cpp`, `app/CScApp.cpp`),
which decompiles FAF's patched executable, and from retail's own Lua for
GPGNet: `lua/multiplayer/{gpgnet,onlineprovider}.lua` and
`lua/ui/lobby/autolobby.lua`. The notes are in the project memory
(`moho-gpgnet-semantics`).

## The protocol

A TCP connection the game opens to the client. Both ways it carries
commands: a name (u32 length and its bytes), a u32 argument count, and each
argument a u8 type and its body: 0 a 32-bit integer, 1 a string (u32 length
and bytes), 2 data (the same). Little-endian throughout.

- The client to the game: `CreateLobby`, `HostGame`, `JoinGame`,
  `ConnectToPeer`, `DisconnectFromPeer`, `HasSupcom`, `HasForgedAlliance`,
  `SendNatPacket`, `EjectPlayer`, `Test`.
- The game to the client: `GameState` (`Idle` once connected, `Lobby` once
  a lobby is made, `Launching` from the lobby's scripts, `Ended`), and what
  the scripts send with `GpgNetSend(cmd, args...)`; `Desync`, `Stats`,
  `Bottleneck`, `BottleneckCleared`, `ProcessNatPacket` from the engine.
- A command the game can't carry out is logged; the link stays up.

## Slices

- **M220a: the link.** The codec, and a client connection the frame pumps
  (connect, read whole commands as they come, write). `/gpgnet host:port`
  on the command line connects at startup instead of showing the front
  end, and imports `lua/multiplayer/gpgnet.lua`'s `CreateUI`, as Moho's
  CScApp does. On connecting, `GameState Idle`. `GpgNetActive()` and
  `GpgNetSend(cmd, args...)` (numbers as integers, strings as strings) in
  the UI state. `CreateLobby` calls `onlineprovider.lua`'s `CreateLobby`
  and answers `GameState Lobby`; the lobby's other commands call the lobby
  object's method of the same name, `HostGame`'s map name made a scenario
  path. A failed connection, or the client closing it, ends the game's
  link (logged; Moho shows a box and exits).
- **M220b: a game through the auto-lobby.** The lobby takes the uid the
  client gives each player (its FAF id) rather than numbering joiners;
  `JoinGame` names the host; `ConnectToPeer` for a pair the lobby's star
  already joins through the host is a no-op (FAF's ICE adapter relays per
  pair, so the host's links are enough). Two processes, driven by the fake
  client through retail's `autolobby.lua` (`CreateLobby` with autolaunch),
  host, join, launch and play in lockstep, the client hearing `Lobby`,
  `Launching`. `EjectPlayer` ejects through the session (M218e); a desync
  is reported (`Desync`).
- **M220c: UDP.** FAF's ICE adapter relays UDP only, giving each peer a
  local endpoint: the lobby's connections gain a reliable, ordered UDP
  transport (the `UDP` protocol), with the adapter's NAT packets
  (`SendNatPacket`, `ProcessNatPacket`) passed through.
- **M220d: FAF's scripts.** FAF's `lobby.lua` and `init_faf.lua` (FAF's Lua
  is open source) on retail data: the command-line options FAF passes, and
  the `GpgNetSend` traffic its lobby sends (`GameOption`, `PlayerOption`,
  `GameMods`, `GameResult`...).

## Tests (M220a)

- **Unit:** the codec round-trips every argument type, and reads a command
  split across reads only once whole; a malformed one (a length past the
  limit) closes the link. Over the loopback, a fake client sees
  `GameState Idle` on connecting, the game hears its commands in order,
  `GpgNetSend` reaches it, and closing the client is noticed.
- **Data (`data.gpgnet_lobby`):** the engine started with `/gpgnet` against
  a fake client (Python) reaches `GameState Idle`; `CreateLobby` makes a
  lobby through retail's `onlineprovider.lua` and `GameState Lobby` comes
  back; `HostGame` hosts it.

## M220b, as built
- **Uids:** a lobby made with a player uid (`InternalCreateLobby`'s sixth
  argument, the client's `CreateLobby`) uses it: the host as its own, a
  joiner asking the host for it (a taken one refused, `UidTaken`); without
  one the host still numbers its players, past any taken.
- **`JoinGame(address, remotePlayerName, remotePlayerUID)`** names the
  host, as Moho's `CLobby.JoinGame` reads it; the lobby had taken the name
  as this player's (retail's LAN screens pass none, so it never showed).
- **Established after joining:** a joiner tells the host it reaches
  everyone once its `ConnectionToHostEstablished` has run, so the host's
  `EstablishedPeers` for it comes after what it sent on joining. Retail's
  auto-lobby sends its player then, and launches on `EstablishedPeers`
  once every player is in: fired on the joiner's arrival, as it had been,
  the host never launched.
- **`GetCommandLineArg(option, count)`** is Moho's `CFG_GetArgOption`: the
  `count` arguments after the option (case aside), or false; the auto-lobby
  reads `/players` and `/team` with it, FAF's lobby many more.
  `HasCommandLineArg` is case-blind too.
- **`DisconnectFromPeer`** on the host closes that player's connection.
- **`EjectPlayer(uid)`** ejects the game's client with that uid, as
  `EjectSessionClient` does; this client can't eject itself (Moho's leaves
  the game; logged here).
- **`Desync`:** a network game's first desync is told the client once:
  its tick, this player's army, and the two sides' checksums (each folded
  from its domains).
- **A test's run** (`--gpgnet-scripted`) ends with the link even in a game,
  failing if the game desynced, and logs its tick every 50.

## Tests (M220b)
- **Unit (`test_net_lobby`):** a client's uids (the host's, a joiner's; a
  taken one refused; one without numbered past them), JoinGame naming the
  host, data by uid, DisconnectFromPeer; the host's `EstablishedPeers` for
  a joiner after the data it sent on joining; the command line (case aside,
  counts, false when missing); EjectPlayer by uid (this client's refused,
  none with it, one ejected: this client reports its source).
- **`data.gpgnet_game` (gate):** the stand-in client drives two games
  through retail's auto-lobby, as FAF's matchmaker does (uids, `/players 2`,
  `/team`, a faction each: the auto-lobby leaves a random one unresolved):
  host, join, `ConnectToPeer` both ways, both `Launching`, both play to
  tick 100 in lockstep; closing the links ends each with no desync or
  script error.

## M220c, the design
FAF's ICE adapter runs beside the game and relays UDP only: for each other
player it binds a local UDP port and tells the game (`JoinGame`,
`ConnectToPeer`) to reach that player at `127.0.0.1:<that port>`; what the
player sends arrives at the game's lobby port from that same local port.
The lobby's connections have been TCP; the adapter can't carry them. Moho's
own lobby protocol for `UDP` is UDP too, so retail's LAN lobby ("UDP") gets
the same.

- **A reliable stream over UDP** (`sim::ReliableUdp`): what the lobby sends
  is a byte stream (its messages framed as over TCP), cut into segments of
  at most 1 KiB, each numbered; the receiver acknowledges the next it
  expects, keeps segments that come early, and hands the bytes over in
  order; the sender resends a segment unacknowledged past its timeout
  (doubling, to 2 s), with at most 256 in flight. A connection opens with a
  hello the joiner repeats until answered, closes with a goodbye, and is
  lost after 10 s without a word (the lobby pings every second). The logic
  runs over an interface for datagrams, so tests drive it over a network
  that drops, reorders and duplicates.
- **One UDP socket a lobby**, bound to its port: the host knows each player
  by the address its datagrams come from, and answers there (the adapter's
  port for that player). A joiner talks to the address it joined.
- **`LobbyNet` over either stream**: the lobby's TCP connections, or the
  reliable UDP stream, chosen by the protocol (`UDP`, `TCP`).
- **NAT packets** (`SendNatPacket`, `ProcessNatPacket`) stay unsupported:
  today's ICE adapter makes its own connections, sending nothing through
  the game's socket.

## M220c, as built
- As designed, and: the streams pump on a thread of their own too (every
  5 ms, locked), as Moho's network runs apart from its frame. The first cut
  pumped only from the frame, and a game loading its map (16 s here) lost
  its peer after the 10 s without a word.
- An open stream sends at once (as a socket would), not at the next pump.
- An acknowledgement of what came in order waits up to 20 ms for data to
  ride on, as TCP's delayed ACK: a lockstep's frame each way each tick then
  carries them, and no segment is resent on a clean network. One early or
  again is acknowledged at once.
- A joiner binds its lobby's port too: the ICE adapter sends to it.
- The data-free CI pairs run over UDP now, so `cross-os-play` covers
  Winsock's UDP.
- The tests' wait for a host's lobby binds its UDP port (in use: hosting)
  where it had connected over TCP.

## Tests (M220c)
- **Unit (`test_reliable_udp`, `[udp]`):** a lockstep's traffic (a
  message each way every pump) resends nothing on a clean network, the
  acknowledgements riding on the data; a send goes at once; and, over an
  in-memory network that
  loses, duplicates and reorders (seeded): 250 KB both ways clean, 420 KB
  with 30% lost, 10% twice and up to 60 ms late; closing delivers what was
  sent, then goodbye; a quiet peer lost just past 10 s; a hello unanswered,
  or to a side not listening, fails; a peer that starts over is a new
  stream and the old one's datagrams are ignored; over real loopback
  sockets, 100 KB queued while connecting.
- **Unit (`test_lobby_net`):** every lobby scenario over TCP and over UDP.
- **Data:** `data.lan_game`, `data.lan_game_quit`, `data.gpgnet_lobby`,
  `data.gpgnet_game` over UDP; `data.gpgnet_game_relayed` through a relay
  standing in for the ICE adapter, losing 5%.
