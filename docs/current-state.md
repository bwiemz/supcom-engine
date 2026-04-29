# OpenSupCom Current State

Last reviewed: 2026-04-29

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

- `build/tests/Debug/osc_tests.exe` passes: 103 test cases, 1,581 assertions.
- `build/Debug/opensupcom.exe --help` runs and lists the current CLI surface.
- `build/Debug/opensupcom.exe --full-smoke-test --map "/maps/SCMP_009/SCMP_009_scenario.lua"` completes the lifecycle: front-end, lobby/reload, game, score, return-to-front-end.
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
- validated teleport destinations for playable bounds, path passability, and footprint occupancy
- cloak participation in effective vision and weapon target acquisition
- sim-side focus army normalization for FA Lua's 1-based army ids
- classification of the known FA AI builder `deepcopy` diagnostic below the active log level

## Known Gaps And Risks

- `GameOptions.ScenarioFile` still needs tighter end-to-end wiring from lobby/UI selection into reload.
- Cloak energy-drain and maintenance behavior still need integration; cloak now affects vision and weapon targeting, but the economy side remains incomplete.
- Several stubs remain intentionally cosmetic, multiplayer-only, debug-only, or deprecated. They should stay classified so gameplay blockers are not hidden among harmless no-ops.

## Recommended Work Order

1. Keep docs and ignores accurate so local tooling noise does not obscure engine changes.
2. Finish the remaining cloak economy behavior and any other active-unit maintenance toggles that affect real match balance.
3. Tighten lobby-to-reload `GameOptions.ScenarioFile` handling and then revisit score/lobby polish.
4. Continue classifying stubs by gameplay impact so harmless UI/multiplayer no-ops do not mask skirmish blockers.
