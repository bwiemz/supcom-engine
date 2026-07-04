# OpenSupCom Current State

Last reviewed: 2026-07-04

## What This Codebase Is

OpenSupCom is a C++20/CMake reimplementation of the Supreme Commander: Forged Alliance Moho engine. The active engine surface is split across:

- `src/lua`: Lua 5.0 integration, FA/FAF script bootstrap, moho class bindings, sim/UI globals, smoke harness.
- `src/sim`: simulation state, entities, units, weapons, economy, intel, orders, AI brains, platoons, manipulators, path interaction.
- `src/map`: scenario and `.scmap` loading, heightmaps, pathfinding grid, visibility grid, terrain quadtree.
- `src/renderer`: Vulkan terrain/unit/UI/HUD rendering, mesh/texture/shader caches, overlays, particles, water, minimap, strategic icons.
- `src/ui`: MAUI-style control tree, input dispatch, WLD UI provider, keymap, font metrics.
- `src/audio` and `src/video`: XWB/XSB/miniaudio audio and MPEG movie playback support.
- `tests`: Catch2 unit coverage for core systems plus focused renderer/sim/parser behaviors.

The code runs against real FA/FAF data via the VFS and currently boots Seton's Clutch far enough to load blueprints, parse the map, start FA AI code, spawn armies, build structures, and execute sim ticks.

## Verified Locally

- `build/tests/Debug/osc_tests.exe` passes: 208 test cases, 5,026 assertions.
- `build/Debug/opensupcom.exe --help` runs and lists the current CLI surface.
- **Multiplayer (LAN lockstep), verified across two OS processes over localhost TCP:**
  `opensupcom.exe --mp-host` + `opensupcom.exe --mp-join 127.0.0.1` reach identical
  sync checksums with `desynced=0` through scripted player orders (incl. a mid-move
  Stop); adding `--mp-desync` makes both peers correctly report `desynced=1` for an
  injected divergence. Command routing (`SimState::route_command`) sends local human
  orders to the `LockstepSession` in multiplayer and applies them directly in
  single-player (unchanged). See `docs/plans/2026-07-03-multiplayer-networking-design.md`.
- **LAN lobby lifecycle, verified across two OS processes:** `opensupcom.exe
  --lan-host` + `opensupcom.exe --lan-join 127.0.0.1` run the real lobby handshake
  (host advertises scenario + RNG seed, client applies + readies, host fires the
  launch barrier) over a `MuxTransport` that carries both the lobby channel and the
  lockstep channel on one connection, then play a synced lockstep match. Both print
  matching scenario, seed, an RNG probe (proving the shared seed reached each sim —
  the fix for `weapon.cpp`'s previously non-deterministic firing randomness), and
  final checksum with `desynced=0`. Windowed reachability: `--lan-window-host` /
  `--lan-window-join <ip>` create the transport at startup and the game loop drives
  the same handshake to launch (a two-window play verified only by logic-equivalence
  to the headless run). See
  `docs/superpowers/specs/2026-07-04-windowed-lan-lobby-design.md`.
- **LAN IP-entry UI:** a front-end "LAN Game" button opens a dialog (host-IP field +
  Host/Join/Close + status) that calls the `LanHost([port])` / `LanJoin(ip[, port])` /
  `LanNetStatus()` engine globals over the LAN lifecycle above. `--lan-ui-test`
  verifies the globals headlessly (`LanHost` creates a listening transport,
  `LanJoin("")` is rejected) and that the dialog Lua snippet parses + `pcall`-degrades
  gracefully. The dialog is built with FA `maui`/`UIUtil`; its actual rendering/click
  is verified only in a live window (no GUI automation in CI) and is fully
  `pcall`-guarded so any UI mismatch logs a warning rather than breaking the menu.
  See `docs/superpowers/specs/2026-07-04-lan-ip-entry-ui-design.md`.
- `build/Debug/opensupcom.exe --full-smoke-test --map "/maps/SCMP_009/SCMP_009_scenario.lua"` completes the lifecycle: front-end, lobby/reload, game, score, return-to-front-end. Its lobby phase now launches through an `InternalCreateLobby` instance and `lobby:LaunchGame(config)`.
- `build/Debug/opensupcom.exe --lobby-flow-test` boots the no-map front-end, triggers the real `ButtonSkirmish()` path, pumps UI control frames, and verifies hosted-lobby callbacks fire.
- `smoke_report.txt` is clean after the full-smoke run: 0 unique issues, 0 total occurrences.

## Implemented Since The Older Plans

Some historical plan checkboxes are stale. The following items from the April full-skirmish plan are already present in code:

- `Control:Disable/Enable/IsDisabled`
- `ItemList:AddItems/ClearItems/SetTitleText`
- single-player lobby `SendData`/`BroadcastData` loopback
- cloak/stealth/sonar stealth unit toggles
- `SetAutoMode` / `GetAutoMode`
- `OnAdjacentTo` callbacks on completed adjacent structures
- death weapon enable/disable methods
- `IssueKillSelf`
- `CreateVisibleAreaAtPoint`
- Movie control MPEG loading/playback path
- lobby slot config wiring for `Human`, `AIPersonality`, `Faction`, `Team`, `StartSpot`, `PlayerColor`, and `ArmyColor`
- `GameOptions.ScenarioFile` launch wiring through `LaunchSinglePlayerSession`, registry launch state, reload, and full-smoke session config
- full-smoke lobby launch now exercises the lobby communication object's `LaunchGame(config)` path instead of directly calling `LaunchSinglePlayerSession`
- headless lobby-flow smoke now exercises front-end `ButtonSkirmish()` through menu animation, lobby creation, host-game callback, and connection-established callback
- score-screen `ReturnToLobby` is registered in shared UI bindings and signals the return-to-lobby path
- front-end fallback globals no longer overwrite real shared UI bindings during no-map menu bootstrap
- UI `SetFocusArmy` is now a real shared binding, including `-1` observer-mode focus normalization
- validated teleport destinations for playable bounds, path passability, and footprint occupancy
- cloak participation in effective vision and weapon target acquisition
- active unit maintenance stall behavior for cloak, radar, sonar, omni, jammer, radar/sonar stealth, stealth fields, water vision, and owner-paid shields
- moho cloak/stealth helpers now update the same intel state used by visibility, blips, and maintenance shutdown
- sim-side focus army normalization for FA Lua's 1-based army ids
- classification of the known FA AI builder `deepcopy` diagnostic below the active log level

## Game Modes / Victory Conditions (2026-07-03)

Victory and defeat are entirely C++-driven in `SimState` (FA's `lua/victory.lua` is
not run). The game modes are enforced with categories matching FA's real
`lua/victory.lua` (verified against retail game data):

- **Assassination** (`demoralization`) — eliminated when the last `COMMAND`/ACU dies.
- **Supremacy** (`domination`) — `STRUCTURE + ENGINEER - WALL`: eliminated when no
  structure or engineer remains (the ACU counts as an engineer; mobile combat units
  and walls do not keep you alive).
- **Annihilation** (`eradication`) — `ALLUNITS - WALL`: eliminated when only walls
  (or nothing) remain.
- **Sandbox** — no elimination. FAF's `decapitation` is treated as an ACU-kill.

Game-over is team-aware: armies are grouped into alliance-connected teams and the
match ends when one team remains (victory) or zero remain (draw). A defeated player
whose ally survives no longer ends the game. On defeat, an army's units are handled
per the `Share` option (`ShareUntilDeath` destroys; `FullShare` → ally; `PartialShare`
→ structures+engineers to ally, rest destroyed; `Defectors` → enemy; `CivilianDeserter`
→ civilian). `FogOfWar=none` reveals the whole map; `CommonArmy`
(`Union`/`Common`/…) pools allied economy; `TeamShareOverflow=enabled` routes wasted
overflow to allies. Covered by `tests/test_victory.cpp`, `test_fow.cpp`,
`test_common_army.cpp`, `test_team_share_overflow.cpp`, `test_handicap.cpp`.
Not yet modeled: FA's 15s allied-victory-request sustain, and `TransferToKiller`
(needs per-unit killer attribution).

## Known Gaps And Risks

- **Multiplayer networking: sync engine done, real transport pending.** Working and
  tested headless: `compute_sync_checksum()` (desync/determinism primitive), the
  tick-keyed `CommandScheduler` (deterministic, lockstep-ready dispatch with a
  per-source confirm gate), serializable `Replay` record/playback, and the
  `LockstepSession` + `INetTransport` that keep sims bit-for-bit in sync, stall on
  a missing frame, and detect desync via exchanged checksums — over both a loopback
  transport and a **real cross-platform `TcpTransport`** (a full lockstep session is
  tested over localhost TCP). Still to build (needs the Vulkan/`osc::lua` build to
  validate): host/join lobby lifecycle + launch barrier, routing every
  `Issue*`/player-input order through `schedule_command`, pipelined command delay,
  and player-drop handling — see `docs/plans/2026-07-03-multiplayer-networking-design.md`.
- Some lobby options are still stored-but-unenforced in C++ (difficulty-tier cheat
  multipliers are consumed by FA's AI Lua rather than the C++ economy; PrebuiltUnits
  needs blueprint/map data). Now enforced: **NoRush** (units confined near their
  start for the first N minutes; `tests/test_no_rush.cpp`), **CommonArmy** (allied
  armies pool mass/energy each tick, off by default; `tests/test_common_army.cpp`),
  per-army **handicap** (`ArmyBrain::set_handicap` scales income; `ArmyGetHandicap`
  now returns the real value; `tests/test_handicap.cpp`), and **TeamShareOverflow**
  (a full teammate's wasted overflow flows to allies with room, off by default;
  `tests/test_team_share_overflow.cpp`).
- Several stubs remain intentionally cosmetic, multiplayer-only, debug-only, or deprecated. They should stay classified so gameplay blockers are not hidden among harmless no-ops.

## Recommended Work Order

1. Keep docs and ignores accurate so local tooling noise does not obscure engine changes.
2. Continue classifying stubs by gameplay impact so harmless UI/multiplayer no-ops do not mask skirmish blockers.
3. Revisit score/lobby polish in the real windowed UI once the remaining gameplay-impacting stubs are classified.
