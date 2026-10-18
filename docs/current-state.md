# OpenSupCom Current State

What the engine does today, and the evidence for it. Every claim below points at a test that runs in the gate or at a validation record of an exact commit (`docs/validation/`); claims without evidence are listed as gaps. The phases and their history are in `docs/ROADMAP.md`.

## What This Codebase Is

OpenSupCom is a C++20/CMake reimplementation of the Supreme Commander: Forged Alliance engine ("Moho"). It runs FA's own Lua game code and data, retail (Steam, 3599) or FAForever's, through a virtual file system:

- `src/sim`: the simulation: entities, units, orders, weapons, projectiles, economy, intel, AI brains and platoons, pathing, saves.
- `src/lua`: Lua 5.0 (LuaPlus' dialect), FA's script bootstrap and the `moho` bindings, by class under `src/lua/bindings/`.
- `src/map`: `.scmap` and scenario loading, heightmaps, the pathfinding grid.
- `src/renderer`: Vulkan rendering of the world, the UI and the HUD, from FA's own shaders ported by hand.
- `src/ui`: the MAUI control tree, input dispatch and focus, the key map.
- `src/audio`, `src/video`: FA's XACT audio and MPEG movies.
- `src/app`: the game (`osc::app::run`) and its test modes (`osc_integration`).

Where the scripts leave Moho's rules unclear, they come from the decompiled engine ([faf-re](https://github.com/Draiget/faf-re)), cited in the code by address.

## Measured status

| Metric | Value |
| --- | --- |
| Unit tests (Catch2) | <!-- metric:unit_test_cases -->1,582<!-- /metric --> test cases in a Linux build (Windows leaves out a few POSIX-only ones). CI runs them on GCC, Clang, ASan and MSVC. |
| Data-backed gate on retail (`ctest -L gate`) | <!-- metric:gate_tests -->269<!-- /metric --> tests, each a mode of `osc_integration` playing retail's scripts and data (one per system: `--missile-test`, `--footfall-test`, `--selection-render-test`...), plus the flows below; two save/load flows run on FAF's data instead, where FAF's client is installed. |
| Golden captures (`ctest -L golden`) | <!-- metric:golden_tests -->7<!-- /metric --> pixel comparisons at 0.1%: FA's game interface at frame 600 (with and without the minimap), retail's skirmish lobby and its map list, and a mouse held through the window's input (`--mouse`): a wall dragged out, and a drag formation settled. |
| Two-process MP tests (`ctest -L mp`, data-free) | <!-- metric:mp_tests -->4<!-- /metric --> |
| Architecture checks (`ctest -L arch`) | <!-- metric:arch_tests -->18<!-- /metric -->: no library cycle or layer reaching up; every serialized type's fields in its serializer; the blueprint-field, Lua-stub and retail-hook baselines; this document's metrics. |
| Static analysis | clang-tidy ratchet at its baseline of <!-- metric:tidy_baseline -->30<!-- /metric --> triaged findings; changed lines follow `.clang-format`; CI builds first-party code with `-Werror`. |
| Retail engine API still unbound | <!-- metric:unbound_globals -->12<!-- /metric --> globals and <!-- metric:unbound_methods -->7<!-- /metric --> methods (`opensupcom --binding-coverage`, ratcheted by `tests/integration/binding_baseline_retail.txt`); 13 are not in retail's engine either, most of the rest are UI-only. |

The counts are a Linux build's, written by `tools/status_metrics.py update --build-dir <build>`; `arch.status_metrics` fails when they are stale.

## Acceptance matrix

Each area, what is accepted and by what. "Last verified" names the validation record of the commit it was last checked on; an area's gate tests also run on every later change.

| Area | Accepted | Evidence | Last verified |
| --- | --- | --- | --- |
| Data and boot | Retail FA (Steam) and FAF data boot; retail's front end, lobby and game interface run; mods (a game's `GameMods`, hooks, the player's mods folder) work as Moho's. | `data.mods_flow`, `data.lobby-flow-test`, `data.uiboot-test`, goldens | [adb79465](validation/2026-10-13-adb79465.md) |
| Long games | Four retail AIs play SCMP_009 for 18,000 ticks on retail data (seeds 4242, 7777), with Moho pathing on as well, and on FAF's data. In the last record the retail games had no script errors. FAF's had two: at an AI's defeat FAF's distress thread calls a base manager that its own `DisableAI` cleared (`platoon.lua:1555`), and FAF's transport code reads the blueprint of an enemy dummy unit (`zxa0003`) whose script never sets it (`transportutilities.lua:1025`). Retail's AIs build as many units as before #591 (639 at tick 6,000, seed 4242), since #615 took `GetUnitsAroundPoint`'s units as the original does. | Long runs in the record | [adb79465](validation/2026-10-13-adb79465.md) |
| Determinism | Two processes play a four-AI game identically, compared domain by domain; GCC, Clang and MSVC reach one pinned checksum; Windows and Linux play together in lockstep. The checksum covers the state that decides a unit's next move (#395). | `data.determinism`, `test_cross_platform.cpp`, `test_checksum_coverage.cpp`, CI `cross-os-play` | [adb79465](validation/2026-10-13-adb79465.md) |
| Saves and replays | A save is the whole sim state (every object, the Lua heap), restored and played on; campaign saves load as their operation; replays play back to the same checksums. | `data.save_load`, `data.save_load_campaign`, `data.save_load_replay`, `data.load_flow`, `data.replay_roundtrip`, `data.replay_flow` | [adb79465](validation/2026-10-13-adb79465.md) |
| Campaign | Operations launch as retail's `SetupCampaignSession`; the front end runs from operation select through briefing, operation, score and the next briefing; the tutorial and the outro play. | `data.campaign_flow`, `data.tutorial_flow`, `data.outro_flow`, `data.campaign-test` | [adb79465](validation/2026-10-13-adb79465.md) |
| Multiplayer | LAN lobby (discovery, host, join, launch) and GPGNet (FAF's client protocol) to a lockstep game; desync detected; a vanished or slow peer handled; chat, pause and speed. Each army takes commands from the players Moho's command sources give it, so FAF's union and common armies share control, and a dropped player's army is left to its brain's `AbandonedByPlayer`. | `mp.*`, `data.lan_game`, `data.lan_game_quit`, `data.gpgnet_lobby`, `data.gpgnet_game`, `data.gpgnet_game_relayed`, `data.command-sources-test`, `data.command_sources_faf`; a scripted FAF-client harness | [adb79465](validation/2026-10-13-adb79465.md) |
| Combat | Retail's weapon state machines (racks, salvos, turret slew and tolerance); aim at the target's TargetBones, as Moho's target points (#397); shots leave along their muzzles, spread and sped as Moho's `CreateProjectile` (#405), and fly as its `MotionTick`: thrust along their facing, homing shots turned at their turn rate, lead, zig-zag (#408); projectiles and beams with script-decided collisions; missiles, silos and their defences; shields; damage, armour and veterancy. | `data.{weapon,fire,aim,targeting,projectile,missile,silo,beam-weapon,defence,shield,damage,armor,vet,impact,collide}-test`, `test_target_points.cpp`, `test_collision.cpp`, `test_flight.cpp` | [adb79465](validation/2026-10-13-adb79465.md) |
| Movement and orders | Moho's drive (acceleration, braking, turning), formations, steering and crowds; move, attack, guard, patrol, ferry, assist and script orders; Moho's order filter. Footprint classes (#398) and Moho's occupation grid and fit test (`COGrid`, `OCCUPY_MobileCheck`/`FootprintFits`, #406) are in, and Moho's pathing by footprint class (its cluster maps, hierarchical search and path navigator, #415–#418) runs behind `--moho-pathing`, off by default (see *Known gaps*). | `data.{move,drive,formation,steer,crowd,path,canpath,patrol,guard-engage,ferry,script-order}-test`, `test_footprint_classes.cpp`, `test_occupancy.cpp`, `test_path_clusters.cpp`, `test_path_search.cpp`, `test_path_navigator.cpp`, `test_moho_pathing.cpp` | [adb79465](validation/2026-10-13-adb79465.md) |
| Air | Winged attack runs and bombing; idle aircraft land after `AutoLandTime` on a place they reserve, and refuel there (#419, #422); hovering aircraft (gunships, drones) circle their target or their work by their `Circling*` fields (#422); air staging (docking, refuel, repair); carriers; transports (slots, pickup, drop). | `data.{air-attack-run,air-auto-engage,air-turn,air-staging,carrier,carrier-land}-test`, `data.transport-*-test`, `test_air_landing.cpp`, `test_air_circling.cpp` | [adb79465](validation/2026-10-13-adb79465.md) |
| Construction and economy | Placement by layer, adjacency, upgrades, enhancements, reclaim, repair, capture, economy events (teleport, OverCharge), the unit cap. | `data.{build,construction,placement-layers,adjacency,upgrade,enhance,reclaim,repair,capture,economy,unit-cap}-test` | [adb79465](validation/2026-10-13-adb79465.md) |
| Intel and fog | Moho's intel grids (vision, radar, sonar, omni), counter-intel and jammers, blips and remembered structures, LOS. | `data.{intel,unit-intel,counter-intel,jammer,jammer-blip,fow,los}-test` | [adb79465](validation/2026-10-13-adb79465.md) |
| Animation collisions | A walker's footfalls and their damage; a crashing CZAR's or Ahwassa's bones told once each on reaching the ground, with their scripts' crash damage; detector state across a save. | `data.footfall-test`, `test_checksum_coverage.cpp` | [adb79465](validation/2026-10-13-adb79465.md) |
| Rendering | FA's terrain, water, sky, unit materials, decals, shadows, effects, strategic icons and fog, by FA's own shaders; drawn between the sim's ticks. FA's range overlays, as Moho's `RangeRenderer` draws them: stencil-volume rings on the ground for the selection, the hovered unit, a placement and the active filters (#407). | The `*-render-test` modes (`data.range-render-test` among them), `data.shadow-map-test`, `data.water-render-test`, goldens | [adb79465](validation/2026-10-13-adb79465.md) |
| UI | Retail's own interface: the MAUI controls, focus and keys, the key map and console, the orders and construction panels, selection (units picked where they are drawn, by a 3D ray and a screen box, #401), the minimap. Build templates (#410) are placed whole, as Moho places them: each structure previewed and ordered from the lead's cell, one that can't stand left out, drags laying copies a span apart. | `data.{gameui,ui,controls,keyboard,keymap,selection-render}-test`, `test_selection_geometry.cpp`, goldens | [adb79465](validation/2026-10-13-adb79465.md) |
| Audio | FA's XACT data through an app-owned cue engine: interface, voice, music, unit and weapon sounds, as Moho plays them. Checked by data and timing tests only. | `data.audio-test`, `data.audio-data-test`, `data.unitsound-test` | [adb79465](validation/2026-10-13-adb79465.md) |
| Performance | See *Benchmarks* below. | `tools/bench.py` | See below |

## Benchmarks

`tools/bench.py` (Release, four retail AIs on SCMP_009, seed 4242, an RTX PRO 4500 at 1920×1080). These are the baselines `bench.py check` holds later builds to. They were recorded on 2026-10-12 on main at 19c44e71, and `late-moho` with #619 as well: without it that game exits at tick 15,100, on an `IssueFormAttack` with `'NoFormation'`. The `-moho` scenarios play the same game with `--moho-pathing`, and their reports count the path searches and the cells they expanded.

| Scenario | Game at the end | Measured |
| --- | --- | --- |
| `early`: 6,000 ticks (10 game-minutes) | 641 units, 40,717 entities | 13.5 s of sim; a tick's mean 2.2 ms, p50 1.5 ms, p99 17 ms; peak 408 MB |
| `early-moho`: the same, Moho pathing | 637 units, 42,635 entities; 7,682 searches, 2.3 M cells expanded | 17.9 s of sim; mean 3.0 ms, p50 1.8 ms, p99 17 ms; peak 389 MB |
| `late`: 18,000 ticks (30 game-minutes) | 1,388 units, 38,724 entities | 155 s of sim; a tick's mean 8.6 ms, p50 6.7 ms, p99 44 ms; peak 653 MB |
| `late-moho`: the same, Moho pathing | 1,344 units, 43,731 entities; 163,603 searches, 45 M cells expanded | 250 s of sim; mean 13.9 ms, p50 13.4 ms, p99 52 ms; peak 707 MB |
| `render-battle`: 600 frames of tick 6,000 | | CPU `render()` p50 1.7 ms, p95 3.2 ms; GPU 0.60 ms |
| `render-late`: 600 frames of tick 18,000 | | CPU p50 2.4 ms, p95 5.5 ms; GPU 0.72 ms |
| `render-strategic`: the same, zoomed out to icons | | CPU p50 1.8 ms, p95 4.9 ms; GPU 0.38 ms |

Moho pathing costs 1.3× the default's sim time in the early game and 1.6× in the late game. A search expands about 300 cells. Its two games were recorded again with a unit bouncing back from ground it won't fit, as Moho's motion does.

Against the 2026-10-07 baselines (6d59aa3b), the games have grown:
- **More units early.** Retail's AIs build as they did before #591 again (#615): 641 units at tick 6,000, against 517.
- **Trees.** About 1,800 of SCMP_009's 4,700 tree groups break up by tick 6,000, each into about 19 single trees, which stay as props. That makes 38,662 props at tick 6,000, against the map's 5,300 at the start. The rate matches the original game's: 12 Strikers driven through a forest break 82% of its groups there and 87% in the engine (2026-10-13).
- **The cost.** The default late game takes 155 s of sim against 91 s: a tick's mean is 8.6 ms against 5.1 ms, and it peaks at 653 MB against 410 MB.
- **Where a late tick's time goes** (sampled from tick 15,000 on main 256ce90d):
  - Lua: 57% inclusive.
  - Navigation and steering: 12.5%.
  - Path search: 9%.
  - The Lua collector: 7.5%.
  - Category matching: 6.7%.
  - Props: 0.5%. The extra trees cost almost nothing directly.
- **Retail's own script leaks.** The late game ends with 7,901 live script threads:
  - 4,083 are retail's EngineerManager `EngineerWaiting` chains, which never end for an engineer that died idle. Retail's sim `Unit:IsDead` is Lua (`self.Dead`) and still answers for a destroyed unit.
  - 2,161 are base-monitor alert timeouts, which retail's `BaseMonitorCheck` keeps adding.
  - Both are retail's own, so they are kept.
- These numbers include #607, which made dead Lua threads leave in one pass: −8.8% on 3be4d6a5's late game.

## Known gaps

What is not yet as Moho does it, or not yet checked. Each is on the roadmap (`docs/ROADMAP.md`, and the external review's work order of 2026-10-06).

- **Pathing by footprint.** By default units still path as points on one 2-unit grid. Moho keeps passability per footprint class, so a big unit can't take a gap a small one can, and its pathing is now ported (#415–#418, `docs/plans/2026-10-07-per-class-pathing-design.md`). It stays behind `--moho-pathing`; the evidence for making it the default is in the [pathing record](validation/2026-10-12-moho-pathing-evidence.md): every gate data mode passes with it, water transitions and saves hold, and the late game takes 1.6 times the default's sim time with it (250 s against 155 s on the benchmark; the games differ). Of 24 units sent through a crowded 4-wide gap, the original game gets all across (probed 2026-10-12). With the flag all 24 do too: the path navigator follows the shipped exe, whose units make for the gap before they reach the wall (probed 2026-10-15, `tools/moho_probes/navigator` and `crowd`). Without the flag, 6 of them cut through the wall's corners.
- **Projectile flight.** As Moho's since #405 and #408, but for two things nothing in retail uses: bounce (no projectile has `Min/MaxBounceCount`), and retargeting on a miss, which Moho gates on `AutoInitiateAttackCommand` as well as `ReTargetOnMiss`, a pair no retail weapon has.
- **Engine API:** the Lua calls, arguments, blueprint fields, categories, callbacks and behaviours that retail's scripts and data use and the engine doesn't support, or supports in part, with FAF's additions, each with its effect, evidence and status: `docs/engine-api-gaps.md`.
- **Needs a person:** a listening pass of the audio; comparison with the original game's look (no reference captures exist here); the hardware cursor on a real display; Steam and the Steam Deck; external testers.
- **Multiplayer:** peers are not authenticated; two players dropping at once can leave the survivors disagreeing; FAF's ICE adapter and client are tested only through stand-ins.
- **Lobby options:** difficulty cheat multipliers are applied by FA's AI scripts, not the engine; `PrebuiltUnits` needs map data.
- **Architecture:** the renderer is being broken into subsystems in small changes: the shadow map, the bloom and the shadow casters are out (#420, #421, #423; the frame's targets in #429). `render()` is down from 756 lines to 149: the frame's CPU updates and each pass (normals, reflection, the scene, the screen's layers, submission) are their own functions. `build_scene` is down from 393 to 111: its bone descriptors, terrain strata set, fog texture, normal maps and map decals are their own functions. `init` is down from 378 to 59: the window, the Vulkan instance, surface and device, the command objects, descriptor layouts, samplers and sub-renderers are its stages. `create_pipelines` is down from 361 to 44: its terrain, unit, mesh and shield, decal and UI pipelines are built by a function each. In the sim's bindings, `create_unit_core`, which makes every unit, was 1,330 lines, most of them its blueprint's read; that read is now `src/lua/unit_blueprint.cpp`, a function per part of the blueprint (health, drive, each weapon, economy, intel, transport, footprints, an aircraft's flight...), and `create_unit_core` is 176 lines. The tick's checksum, `SimState::checksum_parts`, was 501 lines; it is now `src/sim/sim_checksum.cpp`, a function per part it hashes, and 74 lines (#611). `Unit::order_guard` is down from 409 lines to 240 (#612), and `OverlayRenderer::update` from 430 to 45 (#613).

## Game modes and victory

With retail or FAF data, the scenario's own `/lua/victory.lua` decides the game, as in Moho: retail's `BeginSession` hook forks `CheckVictory`, which calls the brains' `OnDefeat`/`OnVictory`/`OnDraw` and then `EndGame`; the engine ends the session (`SessionIsGameOver`, `NoteGameOver`) and retail's score screen ends it for good (`SessionEndGame`). `data.victory-test` plays that through.

For data without a victory script, `SimState::update_victory` enforces FA's modes by `lua/victory.lua`'s categories: Assassination (the last `COMMAND` unit), Supremacy (`STRUCTURE + ENGINEER - WALL`), Annihilation (`ALLUNITS - WALL`) and Sandbox; game-over is team-aware, and a defeated army's units go by the `Share` option. `FogOfWar=none`, per-army handicap, `TeamShareOverflow` and `NoRush` are enforced (`test_victory.cpp`, `test_fow.cpp`, `test_handicap.cpp`, `test_team_share_overflow.cpp`, `test_no_rush.cpp`). Not modelled in that fallback: FA's 15 s allied-victory request, and `TransferToKiller`.

Army statistics use Moho's names and meanings, which retail's score threads read (`Units_History`, `Units_Killed`, `Enemies_Killed`, values built, lost and destroyed, the economy's totals and waste, the unit cap); `GetBlueprintStat` splits them by category.
