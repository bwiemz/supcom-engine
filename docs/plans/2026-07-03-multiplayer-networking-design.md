# Multiplayer Networking — Design & Roadmap

Status: **Phases 1–3 landed and headless-verified (2026-07-04).** Phase 1's
remaining command wiring, the lobby `HostGame`/`JoinGame` → `TcpTransport` +
`LockstepSession` hookup, and a two-process LAN verification are done (PRs #12,
#13). What remains is the windowed lobby lifecycle (Phase 3 tail) and Phases 4–5.
This document scopes the work needed to take OpenSupCom from single-player +
loopback lobby to real networked multiplayer, and records what exists.

## Integration status (2026-07-04)

- **Command routing landed.** `SimState::route_command()` sends a local human
  order to a network sink (→ `LockstepSession::submit_local`, broadcast +
  scheduled) when multiplayer is active and human input is in scope; otherwise it
  applies directly. Single-player installs no sink, so it is bit-identical. AI /
  sim orders stay direct (deterministic on every client — never broadcast). The
  `Issue*` bindings, `InputHandler` right-click/minimap, and the UI-panel
  SimCallback drain all route through this seam. `Stop`/`ClearCommands` are
  network-safe (routed as `CommandType::Stop`, cleared in both branches).
- **Lobby wired.** `mp_net_state` holds the session; `HostGame`/`JoinGame` create
  the `TcpTransport` when a LAN host port / join address is configured (rawget
  globals, bypassing the config.lua lock); `mp_attach_session` builds the
  `LockstepSession` at launch; the tick loop drives via the session when active.
- **Verified headless.** `opensupcom --mp-host` / `--mp-join <addr>` run a real
  two-process TCP lockstep match: identical sync checksums with `desynced=0`
  (positive, incl. a mid-move Stop), and `--mp-desync` proves both peers detect
  an injected divergence. `test_command_routing.cpp` covers the routing seam.
- **Not yet done:** two *windowed* instances rendezvousing through the lobby
  (needs the lobby data channel over the transport + a launch barrier + slot/seed
  broadcast); `Enhance` routing; player-drop/timeout; cross-build FP determinism.

## Goal

Run the same skirmish the engine already plays — all victory conditions
(Assassination / Supremacy / Annihilation / Sandbox), teams, and share rules —
between 2–8 networked players, matching FA's deterministic lockstep model so the
FAForever ecosystem (and its replay format) stays compatible.

## Why lockstep

FA is a **deterministic lockstep** RTS: every client runs the *same* simulation
and only exchanges player *commands*, not unit state. With thousands of units this
is the only tractable model (sending unit state would saturate any link). The
consequences drive the whole design:

1. The sim must be **bit-for-bit deterministic** given the same command stream.
2. Commands are **scheduled a few ticks in the future** (the "command delay") so
   every client has them before executing the tick that consumes them.
3. Clients continuously exchange a **sim checksum** to detect divergence (desync).

## What already exists

- **Loopback lobby** — `lobby:HostGame` / `SendData` / `BroadcastData` deliver to
  `self:DataReceived` in-process (`src/lua/moho_bindings.cpp`). `IsHost` is always
  true, `GetLocalPlayerID` always "0". Good enough to drive the lobby UI; no wire.
- **Fixed-timestep sim loop** — `main.cpp` accumulates real time and calls
  `sim_state->tick()` at 10 Hz. No command scheduling; UI commands apply immediately.
- **Sync checksum** — `SimState::compute_sync_checksum()` (added 2026-07-03) hashes
  the authoritative state deterministically. This is the desync-detection primitive
  the lockstep layer needs; it is not yet wired to any network exchange.
- **Command scheduler** — `CommandScheduler` + `SimState::schedule_command` /
  `ready_to_run_next_tick` (added 2026-07-03) implement Phase 1: orders are
  submitted for a future tick and dispatched in a canonical order at the start of
  that tick. It carries a `command_delay`, per-source confirm-frame tracking, and a
  lockstep gate (single-player never stalls). Determinism is proven by a test that
  runs two sims on an identical command stream and asserts equal checksums each
  tick. **Remaining Phase-1 wiring:** route the existing `Issue*` Lua bindings and
  the `main.cpp` player-input path through `schedule_command` instead of applying
  orders directly (a thin change per call site).
- **Command source stubs** — `GetCurrentCommandSource` → 0, `SessionIsMultiplayer`
  → false, `GpgNetSend` → noop, `ResetSyncTable` → noop.

## What is missing (the work)

### 1. Transport
A reliable-ordered message channel between peers. Options: UDP + a reliability
layer (FA's own model), ENet, or a QUIC/TCP fallback. Must expose: connect/listen,
per-peer send/recv of opaque frames, connection state + RTT, disconnect events.
Keep it behind an interface (`INetTransport`) so the loopback path is one impl and
the real socket path another — tests use loopback.

### 2. Lockstep command scheduler
The core new sim concept: a **command frame** = all players' commands for a given
execution tick.

- Every player command (move/attack/build/…) is tagged with an **execution tick**
  = `current_tick + command_delay` (delay ~ ceil(RTT / tick) + margin, e.g. 3 ticks).
- Commands are broadcast to all peers immediately; each client buffers them keyed
  by execution tick.
- `tick(N)` only runs once **every** player's command frame for tick N has arrived
  (including explicit empty frames). Otherwise the client **stalls** (the classic
  lockstep "waiting for players" state) rather than running ahead.
- Single-player is the degenerate case: one local source, delay 0, never stalls.

This requires routing *all* command application through the scheduler (today UI
commands mutate the sim directly). That refactor is the largest single piece and
is what makes both SP and MP deterministic and replay-able.

### 3. Command-source tagging
Each command carries the originating player (`CmdSource`). `GetCurrentCommandSource`
must return the real source during command execution so FA Lua's per-player logic
(and cheat-detection) works. Selection/UI is local-only and never networked.

### 4. Desync detection & handling
- Each client posts `compute_sync_checksum()` every K ticks into the command
  stream; peers compare. Mismatch → surface a desync, snapshot both states for
  diffing, and (FA behavior) drop to a controlled stop.
- Wire `ResetSyncTable` / the FA `Sync` table so script-side sync data is included.

### 5. Determinism hardening
Lockstep is unforgiving. Audit and remove non-determinism from the sim path:
- No wall-clock / `Math.random` in the sim (a seeded deterministic RNG only —
  note `Date.now`/`rand` are already avoided in several places).
- Deterministic container iteration where it affects sim outcomes (the checksum
  already sorts; the sim's own iteration must not depend on pointer/hash order for
  results — audit `entity_registry_.for_each` consumers that mutate).
- Consistent floating-point across builds (fixed FP mode / avoid fast-math), or
  accept same-build-only determinism initially.

### 6. Host / peer lifecycle & GpgNet
- Host authoritative lobby: slot assignment, launch barrier ("all ready"),
  seed + options broadcast, then hand off to the in-game scheduler.
- Reconnect/timeout/lag handling; player drop → army becomes AI or is defeated
  per the share rule (the victory/share code already handles a defeated army).
- `GpgNet` integration for FAForever matchmaking/ICE (can be stubbed for LAN first).

### 7. Replays
Because lockstep already records the full command stream + seed, a replay is just
that stream re-fed to the scheduler. Persisting/loading it is nearly free once the
scheduler exists, and reuses the same checksum for verification.

- **Transport abstraction + loopback lockstep** — `INetTransport` (with a
  `LoopbackHub`/`LoopbackTransport` in-process impl) and `LockstepSession` (added
  2026-07-03) implement Phase 2 and the Phase-3 transport seam: a session
  broadcasts each frame's commands + confirmation + a checksum over the transport,
  ingests peers' frames, and advances the sim only once every participant has
  confirmed the tick. Tests drive two in-process sims to bit-for-bit sync, prove
  the stall-until-confirmed gate, and detect a divergence via exchanged checksums.
  A real UDP/TCP/ICE transport is now a drop-in behind `INetTransport`.
- **Replay record/playback** — `Replay` + `SimState::set_recording` /
  `queue_replay` (added 2026-07-03) capture the command stream into a versioned,
  serializable buffer and re-feed it into a fresh sim. A test records a match,
  serializes/reloads it, replays it, and asserts the reproduced sim matches the
  original checksum. This realizes Phase 7 early (replays fall out of lockstep for
  free) and doubles as a determinism regression harness.

## Phased plan

1. **Command scheduler in single-player.** *(Complete 2026-07-04.)* Tick-keyed
   `CommandScheduler` integrated into `SimState::tick()` with `command_delay`,
   canonical dispatch order, and the lockstep gate; determinism proven by the
   two-sim checksum test. The `Issue*` bindings and the player-input path now flow
   through `SimState::route_command()` (→ scheduler/session in MP, direct in SP).
2. **Loopback lockstep.** *(Landed 2026-07-03.)* `LockstepSession` + `LoopbackTransport`
   drive two in-process sims over the transport, exchanging command frames +
   confirmations + checksums; `tests/test_lockstep.cpp` proves sync, the
   stall-until-confirmed gate, and desync detection.
3. **INetTransport + LAN.** *(TCP transport landed 2026-07-03; lobby wiring + two-
   process LAN verification 2026-07-04.)* `TcpTransport` implements `INetTransport`
   over real sockets; `HostGame`/`JoinGame` create it and `mp_attach_session`
   drives the game sim through a `LockstepSession`. `opensupcom --mp-host` /
   `--mp-join` verify a synced match between two OS processes. **Remaining:** the
   *windowed* host/join lobby lifecycle + launch barrier (slot/seed broadcast over
   the transport), pipelined command delay for RTT (session is currently strict
   one-tick-per-frame), and a UDP+reliability transport as a latency follow-up.
4. **Desync + drop handling.** Checksum-mismatch detection is in
   (`LockstepSession::desynced`) and verified across two processes via
   `--mp-desync`. Remaining: player-drop → AI/defeat, timeouts.
5. **GpgNet / FAForever.** Matchmaking, ICE, replay upload.

## Testability note

Phases 1–2 are fully verifiable headless (no second machine, no game data) and are
where correctness is won or lost. Phases 3–5 need real networked clients + FA game
data and cannot be validated in the current CI-less, data-less environment; they
should land behind the `INetTransport` seam with loopback coverage.
