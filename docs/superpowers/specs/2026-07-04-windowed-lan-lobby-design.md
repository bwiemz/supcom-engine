# Windowed LAN Lobby Lifecycle — Design

Date: 2026-07-04
Status: approved, ready for implementation plan
Depends on: multiplayer integration (PRs #12, #13) — `TcpTransport`, `LockstepSession`,
`SimState::route_command`, `mp_net_state`, the `--mp-host`/`--mp-join` harness.

## Goal

Let two engine instances form a networked 1v1 skirmish through the lobby and play a
bit-identical lockstep match on localhost/LAN. The host is authoritative: it syncs
the `sessionConfig` (map, slots, factions) plus a shared RNG seed to the client, and
a launch barrier fires the *same* `LaunchSinglePlayerSession(config)` on both. From
there the existing `mp_attach_session` + lockstep tick loop (already verified) runs
the game.

### In scope
- 1v1 human-vs-human, host + one client, localhost/LAN.
- Deterministic seeded sim RNG (fixes a real desync source in `weapon.cpp`).
- Engine-level lobby sync: config + seed broadcast, ready handshake, launch barrier.
- Transport multiplexing so lobby messages and lockstep frames share one connection.
- CLI/global entry (`--lan-host` / `--lan-join <ip>`) to reach the path in a window.
- A headless two-process test that runs the whole lifecycle and asserts sync.

### Out of scope (documented follow-ups)
AI-over-network, teams/observers, chat sync, kick/eject, host migration, an
"enter IP" lobby UI field, UDP+reliability transport, cross-build float determinism,
lobby timeouts beyond a basic guard, >2 players.

## Architecture

```
FRONT_END / lobby phase                 GAME phase
┌───────────────────────┐               ┌──────────────────────────┐
│ LanLobby (host|client)│  launch       │ LockstepSession          │
│  - config + seed sync │  barrier ───▶ │  (existing, verified)    │
│  - ready handshake    │               │  drives SimState.tick()  │
└──────────┬────────────┘               └───────────┬──────────────┘
           │ lobby channel                          │ game channel
           └──────────────► MuxTransport ◄──────────┘
                                 │  (1-byte tag: L | F)
                            TcpTransport (one TCP connection)
```

One TCP connection carries two logical channels, demultiplexed by a 1-byte tag.
`LanLobby` uses the lobby channel; `LockstepSession` uses the game channel. This
removes any chance of a lobby message being parsed as a lockstep frame during the
lobby→game handoff.

## Component 1 — Deterministic sim RNG

**Problem.** `src/sim/weapon.cpp` uses
`static thread_local std::mt19937 rng{std::random_device{}()}` for firing
randomness. Each process seeds from `random_device`, so two clients roll different
angles → projectile velocities diverge → desync as soon as a weapon with
`firing_randomness > 0` fires.

**Design.**
- A small deterministic generator `SimRandom` (SplitMix64: `u64 state`, `next()`
  → advance + mix, plus `range(lo, hi)` for a bounded `f32`). Header-only in
  `src/sim/`.
- `SimState` owns a `SimRandom` and exposes `void set_seed(u64)` /
  `u32 sim_rand()` / `f32 sim_rand_range(f32 lo, f32 hi)`. Because its state lives
  in the sim and only sim code advances it, two clients fed the same seed and the
  same call sequence produce identical values.
- **Reaching it from the weapon.** `Weapon::try_fire` already receives
  `EntityRegistry&` (not `SimState&`). `EntityRegistry` gains a non-owning
  `SimRandom* sim_random_` that `SimState` sets to `&its SimRandom` when it
  constructs/owns the registry. `weapon.cpp` uses `registry.sim_random().range(-r, r)`
  instead of the `thread_local mt19937`. No new parameters threaded through
  `update_entities → Unit::update → Weapon::update → try_fire`. If the registry
  pointer is null (unit tests that build a bare registry), fall back to a
  registry-local default `SimRandom` so behavior stays deterministic.
- Default seed for single-player: a fixed constant set at sim construction, so SP
  is fully deterministic and its feel is unchanged. Multiplayer: the host's seed,
  applied via `set_seed` before/at launch on both clients.

**Determinism note.** `sim_rand()` must only be called from deterministic sim code
(tick path), never from render/UI. Any non-sim caller would desync; none exist today
besides the weapon path being replaced.

## Component 2 — MuxTransport

New `src/sim/mux_transport.hpp` (header-only, like the loopback transport).

- Wraps an owned `INetTransport` (the real `TcpTransport`).
- Exposes two child `INetTransport` views: `lobby_channel()` and `game_channel()`.
- Outgoing: each child prepends its 1-byte tag (`'L'` = 0x4C lobby, `'F'` = 0x46
  frame) before calling the underlying `broadcast`.
- Incoming: a single `pump()` calls the underlying `receive()` once, strips the tag,
  and routes each message to the matching child's inbox. Each child's `receive()`
  drains its own inbox. `pump()` is called once per frame by whoever owns the mux.
- Host connection acceptance (`poll_connections`) is proxied through to the
  underlying `TcpTransport` (mux exposes `poll_connections()`/`peer_count()`).

This is a small, independently testable unit: feed tagged bytes in, assert they come
out the correct child channel.

## Component 3 — LanLobby

New `src/lua/lan_lobby.{hpp,cpp}` (lives with the lobby bindings; needs Lua to read
`FrontEndData["sessionConfig"]` and to trigger launch).

**Role & lifecycle.** Created when the lobby stands up a transport (host or client).
Owns the `MuxTransport` (built around the transport `mp_net_state` created). Pumped
once per frame while in FRONT_END/lobby. Hands the game channel to
`mp_attach_session` at launch.

**Wire protocol (over the lobby channel).** Little-endian, tag byte then payload:
- `CONFIG` (host→client): serialized `sessionConfig` (scenario path, per-slot
  faction/color/team/name, army count) + `u64 seed`. Reuses the same field set
  `execute_reload_sequence` already reads from `PlayerOptions`.
- `READY` (client→host): empty; client has applied the config.
- `LAUNCH` (host→client): empty (config already delivered); the go signal.

**Host state machine.** `Idle → Advertising (peer connected, CONFIG sent) → Ready
(client READY received) → Launching (operator/host confirms → LAUNCH sent →
LaunchSinglePlayerSession locally)`.

**Client state machine.** `Connecting → Configured (CONFIG received, applied,
READY sent) → Launching (LAUNCH received → LaunchSinglePlayerSession locally)`.

**Config application on the client.** The client builds the same Lua `sessionConfig`
table the host used, stores it in `FrontEndData["sessionConfig"]`, and on LAUNCH
calls the existing `LaunchSinglePlayerSession(config)` binding. Both instances then
run the identical existing reload path. Local source: host = 0, client = 1 (already
the `mp_net_state` convention).

## Component 4 — Integration into the game loop

- `mp_net_state` gains an optional `MuxTransport` (wrapping the `TcpTransport`) and a
  `LanLobby`. When `HostGame`/`JoinGame` create the transport, they also create the
  mux + lobby.
- Game loop, FRONT_END/lobby state: each frame, `lan_lobby->pump()` (drives the
  handshake + accepts connections on the host). When a launch fires it sets
  `__osc_launch_requested` exactly as today.
- `mp_attach_session(sim)` builds the `LockstepSession` over the mux **game
  channel** instead of the raw transport. The per-frame tick branch also calls
  `mux->pump()` (so both channels drain) before `session->send_frame()` /
  `receive_and_advance()`. In-game there are no more lobby messages, but pumping the
  mux keeps the demux single-sourced.
- Teardown: `mp_teardown()` destroys lobby → session → mux → transport in order.

## Component 5 — Windowed reachability & CLI

- `--lan-host [port]`: sets `__osc_mp_host_port` global before the lobby boots.
- `--lan-join <ip> [port]`: sets `__osc_mp_join_address` / `__osc_mp_join_port`.
- With these set, the existing windowed FRONT_END flow creates the transport via
  `HostGame`/`JoinGame` and the lobby loop pumps `LanLobby`. The host operator
  triggers launch through the existing lobby launch button (or, for the headless
  test, an auto-launch once the client is READY).
- A proper text field to type the host IP in the lobby UI is a follow-up; CLI is
  sufficient to reach and verify the path.

## Component 6 — Verification (headless two processes)

`--lan-host` / `--lan-join <ip>` run the full lifecycle headlessly (no window),
mirroring the existing `--mp-host`/`--mp-join` harness but through the real lobby +
launch path:

1. Host binds; client connects (direct IP).
2. Host sends CONFIG (a fixed 1v1 no-AI scenario + seed); client applies, READY.
3. Host auto-LAUNCHes once READY; both call `LaunchSinglePlayerSession(config)`.
4. Both run `execute_reload_sequence` → identical armies from the same config+seed.
5. `mp_attach_session` over the game channel; run a bounded tick loop with a scripted
   move and a weapon fire; assert **identical `compute_sync_checksum()`** each check
   and `desynced=0`. Print an `MP_RESULT`-style line.

**Scenario-hang risk.** Memory notes the real FA scenario can hang after ~15 ticks
once AI threads activate. Mitigations, in order: use a **no-AI** 1v1 config (two
human armies) so AI threads never start; bound the run (e.g. 30–60 ticks); if a
real-scenario tick loop still misbehaves headlessly, fall back to asserting the
**post-launch initial checksum matches** (proving config+seed+army build synced) and
lean on the existing `--mp-host` lockstep proof for the tick-sync guarantee. The
lobby lifecycle itself (the new code) is fully covered regardless.

## Testing plan

Headless unit tests (no sockets, Catch2):
- `test_mux_transport.cpp`: tagged round-trip; a lobby send is only seen on the lobby
  channel, a frame send only on the game channel; interleaved batch demuxes correctly.
- `test_sim_rand.cpp`: same seed → identical `sim_rand()` sequence across two
  `SimState`s; different seeds diverge; `sim_rand_range` bounds.
- `test_lan_lobby.cpp`: host+client `LanLobby` over a `LoopbackHub`/`MuxTransport`
  pair drive the handshake to a launch signal on both sides; client's applied config
  equals the host's.

Integration (two processes): the `--lan-host`/`--lan-join` run above.

Regression: full `osc_tests` suite green; single-player `--full-smoke-test` clean
(SP uses the fixed seed and no lobby, so behavior is unchanged).

## Error handling

- Transport setup failure (bind/connect) → log + abort the LAN path, stay/return to
  front-end (never crash single-player).
- Client never sends READY / host never sends LAUNCH → host simply doesn't launch
  (no barrier fires). A real timeout + user feedback is a follow-up.
- Malformed lobby message (short read) → ignored, like the existing lockstep frame
  reader.
- Desync detected in-game → already surfaced by `LockstepSession::desynced()`;
  graceful stop/handling of a live desync is existing/future work.

## Risks & mitigations

| Risk | Mitigation |
|---|---|
| Non-deterministic weapon RNG desyncs combat | Component 1 replaces it with seeded `sim_rand` (this is why RNG is in-scope). |
| Lobby msg parsed as a lockstep frame at handoff | MuxTransport 1-byte tag keeps channels disjoint. |
| Real scenario tick hang on AI | No-AI 1v1 config + bounded run + fallback to initial-checksum assertion. |
| Two SimStates/singletons in one process | Verify with **two processes**, not in-process, so each has its own `mp_net_state`. |
| Windowed path unverifiable here | Headless two-process test covers all logic except GLFW; document the manual two-window recipe. |

## Success criteria

- New unit tests + full suite green; SP full-smoke clean.
- `--lan-host` + `--lan-join 127.0.0.1` (two processes): both complete the lobby
  handshake, launch together, and report identical sync checksums with `desynced=0`
  over a bounded real/near-real match.
- Single-player behavior unchanged.
