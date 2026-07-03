# Multiplayer Networking — Design & Roadmap

Status: **design / not yet implemented.** This document scopes the work needed to
take OpenSupCom from single-player + loopback lobby to real networked multiplayer,
and records what already exists.

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

## Phased plan

1. **Command scheduler in single-player.** Route all commands through a tick-keyed
   scheduler with `command_delay = 0`. No wire yet. Deterministic + replay-able SP;
   fully unit-testable. *(This is the safe, high-value first step and unblocks
   everything else.)*
2. **Loopback lockstep.** Two local sims driven by one shared command stream; assert
   their `compute_sync_checksum()` stay equal every tick. Proves determinism.
3. **INetTransport + LAN.** Real sockets, host/join, launch barrier, command
   broadcast, stall-on-missing-frame, periodic checksum exchange.
4. **Desync + drop handling.** Mismatch detection, player-drop → AI/defeat, timeouts.
5. **GpgNet / FAForever.** Matchmaking, ICE, replay upload.

## Testability note

Phases 1–2 are fully verifiable headless (no second machine, no game data) and are
where correctness is won or lost. Phases 3–5 need real networked clients + FA game
data and cannot be validated in the current CI-less, data-less environment; they
should land behind the `INetTransport` seam with loopback coverage.
