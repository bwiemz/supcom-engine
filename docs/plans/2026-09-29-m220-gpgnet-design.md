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
