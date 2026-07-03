# OpenSupCom Current State

Last reviewed: 2026-07-03

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

- `build/tests/Debug/osc_tests.exe` passes: 128 test cases, 1,726 assertions.
- `build/Debug/opensupcom.exe --help` runs and lists the current CLI surface.
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
not run). As of 2026-07-03 the four FA game modes are distinct and enforced:

- **Assassination** (`demoralization`) — eliminated when the last `COMMAND`/ACU dies.
- **Supremacy** (`domination`) — eliminated when no significant units remain
  (structures + mobile; `WALL` / `INSIGNIFICANTUNIT` do not count).
- **Annihilation** (`eradication`) — eliminated only when every unit is gone.
- **Sandbox** — no elimination.

Game-over is team-aware: armies are grouped into alliance-connected teams and the
match ends when one team remains (victory) or zero remain (draw). A defeated player
whose ally survives no longer ends the game. On defeat, an army's units are handled
per the `Share` option (`ShareUntilDeath` destroys; `FullShare`/`CivilianDeserter`
transfer to an ally/civilian). `FogOfWar=none` now reveals the whole map. These are
covered by `tests/test_victory.cpp`, `tests/test_fow.cpp`, and `tests/test_sync.cpp`.

## Known Gaps And Risks

- **Multiplayer networking is absent** (loopback lobby only). `SimState::compute_sync_checksum()`
  provides the desync/determinism primitive, but transport, a lockstep command
  scheduler, and host/peer lifecycle remain to be built — see
  `docs/plans/2026-07-03-multiplayer-networking-design.md`.
- Some lobby options are still stored-but-unenforced in C++ (NoRush, handicap /
  difficulty tiers, PrebuiltUnits, shared/common-army economy). Cheat multipliers
  are consumed by FA's AI Lua rather than the C++ economy.
- Several stubs remain intentionally cosmetic, multiplayer-only, debug-only, or deprecated. They should stay classified so gameplay blockers are not hidden among harmless no-ops.

## Recommended Work Order

1. Keep docs and ignores accurate so local tooling noise does not obscure engine changes.
2. Continue classifying stubs by gameplay impact so harmless UI/multiplayer no-ops do not mask skirmish blockers.
3. Revisit score/lobby polish in the real windowed UI once the remaining gameplay-impacting stubs are classified.
