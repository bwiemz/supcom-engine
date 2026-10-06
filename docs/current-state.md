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
| Unit tests (Catch2) | <!-- metric:unit_test_cases -->1,186<!-- /metric --> test cases in a Linux build (Windows leaves out a few POSIX-only ones). CI runs them on GCC, Clang, ASan and MSVC. |
| Data-backed gate on retail (`ctest -L gate`) | <!-- metric:gate_tests -->225<!-- /metric --> tests, each a mode of `osc_integration` playing retail's scripts and data (one per system: `--missile-test`, `--footfall-test`, `--selection-render-test`...), plus the flows below. |
| Golden captures (`ctest -L golden`) | <!-- metric:golden_tests -->5<!-- /metric --> pixel comparisons at 0.1%: FA's game interface at frame 600 (with and without the minimap), retail's skirmish lobby and its map list. |
| Two-process MP tests (`ctest -L mp`, data-free) | <!-- metric:mp_tests -->4<!-- /metric --> |
| Architecture checks (`ctest -L arch`) | <!-- metric:arch_tests -->18<!-- /metric -->: no library cycle or layer reaching up; every serialized type's fields in its serializer; the blueprint-field, Lua-stub and retail-hook baselines; this document's metrics. |
| Static analysis | clang-tidy ratchet at its baseline of <!-- metric:tidy_baseline -->31<!-- /metric --> triaged findings; changed lines follow `.clang-format`; CI builds first-party code with `-Werror`. |
| Retail engine API still unbound | <!-- metric:unbound_globals -->15<!-- /metric --> globals and <!-- metric:unbound_methods -->7<!-- /metric --> methods (`opensupcom --binding-coverage`, ratcheted by `tests/integration/binding_baseline_retail.txt`); 13 are not in retail's engine either, most of the rest are UI-only. |

The counts are a Linux build's, written by `tools/status_metrics.py update --build-dir <build>`; `arch.status_metrics` fails when they are stale.

## Acceptance matrix

Each area, what is accepted and by what. "Last verified" names the validation record of the commit it was last checked on; an area's gate tests also run on every later change.

| Area | Accepted | Evidence | Last verified |
| --- | --- | --- | --- |
| Data and boot | Retail FA (Steam) and FAF data boot; retail's front end, lobby and game interface run; mods (a game's `GameMods`, hooks, the player's mods folder) work as Moho's. | `data.mods_flow`, `data.lobby-flow-test`, `data.uiboot-test`, goldens | [ee8ab53b](validation/2026-10-07-ee8ab53b.md) |
| Long games | Four retail AIs play SCMP_009 for 18,000 ticks with no script error on retail data (seeds 4242, 7777), and with Moho pathing on. On FAF's, an error of FAF's own scripts shows where an AI is defeated (seed 4242 in the last record). | Long runs in the record | [ee8ab53b](validation/2026-10-07-ee8ab53b.md) |
| Determinism | Two processes play a four-AI game identically, compared domain by domain; GCC, Clang and MSVC reach one pinned checksum; Windows and Linux play together in lockstep. The checksum covers the state that decides a unit's next move (#395). | `data.determinism`, `test_cross_platform.cpp`, `test_checksum_coverage.cpp`, CI `cross-os-play` | [ee8ab53b](validation/2026-10-07-ee8ab53b.md) |
| Saves and replays | A save is the whole sim state (every object, the Lua heap), restored and played on; campaign saves load as their operation; replays play back to the same checksums. | `data.save_load`, `data.save_load_campaign`, `data.save_load_replay`, `data.load_flow`, `data.replay_roundtrip`, `data.replay_flow` | [ee8ab53b](validation/2026-10-07-ee8ab53b.md) |
| Campaign | Operations launch as retail's `SetupCampaignSession`; the front end runs from operation select through briefing, operation, score and the next briefing; the tutorial and the outro play. | `data.campaign_flow`, `data.tutorial_flow`, `data.outro_flow`, `data.campaign-test` | [ee8ab53b](validation/2026-10-07-ee8ab53b.md) |
| Multiplayer | LAN lobby (discovery, host, join, launch) and GPGNet (FAF's client protocol) to a lockstep game; desync detected; a vanished or slow peer handled; chat, pause and speed. | `mp.*`, `data.lan_game`, `data.lan_game_quit`, `data.gpgnet_lobby`, `data.gpgnet_game`, `data.gpgnet_game_relayed`; a scripted FAF-client harness | [ee8ab53b](validation/2026-10-07-ee8ab53b.md) |
| Combat | Retail's weapon state machines (racks, salvos, turret slew and tolerance); aim at the target's TargetBones, as Moho's target points (#397); shots leave along their muzzles, spread and sped as Moho's `CreateProjectile` (#405), and fly as its `MotionTick`: thrust along their facing, homing shots turned at their turn rate, lead, zig-zag (#408); projectiles and beams with script-decided collisions; missiles, silos and their defences; shields; damage, armour and veterancy. | `data.{weapon,fire,aim,targeting,projectile,missile,silo,beam-weapon,defence,shield,damage,armor,vet,impact,collide}-test`, `test_target_points.cpp`, `test_collision.cpp`, `test_flight.cpp` | [ee8ab53b](validation/2026-10-07-ee8ab53b.md) (#397, #405, #408 after it) |
| Movement and orders | Moho's drive (acceleration, braking, turning), formations, steering and crowds; move, attack, guard, patrol, ferry, assist and script orders; Moho's order filter. Footprint classes (#398) and Moho's occupation grid and fit test (`COGrid`, `OCCUPY_MobileCheck`/`FootprintFits`, #406) are in, and Moho's pathing by footprint class (its cluster maps, hierarchical search and path navigator, #415–#418) runs behind `--moho-pathing`, off by default (see *Known gaps*). | `data.{move,drive,formation,steer,crowd,path,canpath,patrol,guard-engage,ferry,script-order}-test`, `test_footprint_classes.cpp`, `test_occupancy.cpp`, `test_path_clusters.cpp`, `test_path_search.cpp`, `test_path_navigator.cpp`, `test_moho_pathing.cpp` | [ee8ab53b](validation/2026-10-07-ee8ab53b.md) (#398, #406 after it) |
| Air | Winged attack runs and bombing; idle aircraft land after `AutoLandTime` on a place they reserve, and refuel there (#419, #422); hovering aircraft (gunships, drones) circle their target or their work by their `Circling*` fields (#422); air staging (docking, refuel, repair); carriers; transports (slots, pickup, drop). | `data.{air-attack-run,air-auto-engage,air-turn,air-staging,carrier,carrier-land}-test`, `data.transport-*-test`, `test_air_landing.cpp`, `test_air_circling.cpp` | [ee8ab53b](validation/2026-10-07-ee8ab53b.md) |
| Construction and economy | Placement by layer, adjacency, upgrades, enhancements, reclaim, repair, capture, economy events (teleport, OverCharge), the unit cap. | `data.{build,construction,placement-layers,adjacency,upgrade,enhance,reclaim,repair,capture,economy,unit-cap}-test` | [ee8ab53b](validation/2026-10-07-ee8ab53b.md) |
| Intel and fog | Moho's intel grids (vision, radar, sonar, omni), counter-intel and jammers, blips and remembered structures, LOS. | `data.{intel,unit-intel,counter-intel,jammer,jammer-blip,fow,los}-test` | [ee8ab53b](validation/2026-10-07-ee8ab53b.md) |
| Animation collisions | A walker's footfalls and their damage; a crashing CZAR's or Ahwassa's bones told once each on reaching the ground, with their scripts' crash damage; detector state across a save. | `data.footfall-test`, `test_checksum_coverage.cpp` | #400, after [ee8ab53b](validation/2026-10-07-ee8ab53b.md) |
| Rendering | FA's terrain, water, sky, unit materials, decals, shadows, effects, strategic icons and fog, by FA's own shaders; drawn between the sim's ticks. FA's range overlays, as Moho's `RangeRenderer` draws them: stencil-volume rings on the ground for the selection, the hovered unit, a placement and the active filters (#407). | The `*-render-test` modes (`data.range-render-test` among them), `data.shadow-map-test`, `data.water-render-test`, goldens | [ee8ab53b](validation/2026-10-07-ee8ab53b.md) (#407 after it) |
| UI | Retail's own interface: the MAUI controls, focus and keys, the key map and console, the orders and construction panels, selection (units picked where they are drawn, by a 3D ray and a screen box, #401), the minimap. Build templates (#410) are placed whole, as Moho places them: each structure previewed and ordered from the lead's cell, one that can't stand left out, drags laying copies a span apart. | `data.{gameui,ui,controls,keyboard,keymap,selection-render}-test`, `test_selection_geometry.cpp`, goldens | [ee8ab53b](validation/2026-10-07-ee8ab53b.md) (#401 after it) |
| Audio | FA's XACT data through an app-owned cue engine: interface, voice, music, unit and weapon sounds, as Moho plays them. Checked by data and timing tests only. | `data.audio-test`, `data.audio-data-test`, `data.unitsound-test` | [ee8ab53b](validation/2026-10-07-ee8ab53b.md) |
| Performance | See *Benchmarks* below. | `tools/bench.py` | See below |

## Benchmarks

`tools/bench.py` (Release, four retail AIs on SCMP_009, seed 4242, an RTX PRO 4500 at 1920×1080). These are the baselines `bench.py check` holds later builds to. They were recorded on 2026-10-07 with main's engine at 6d59aa3b, which is main at 75efb749 plus this bench's search counters. The `-moho` scenarios play the same game with `--moho-pathing`, and their reports count the path searches and the cells they expanded.

| Scenario | Game at the end | Measured |
| --- | --- | --- |
| `early`: 6,000 ticks (10 game-minutes) | 517 units, 12,630 entities | 7.4 s of sim; a tick's mean 1.2 ms, p50 0.9 ms, p99 8.0 ms; peak 304 MB |
| `early-moho`: the same, Moho pathing | 581 units, 13,344 entities; 7,902 searches, 1.6 M cells expanded | 10.4 s of sim; mean 1.7 ms, p50 1.2 ms, p99 11 ms; peak 306 MB |
| `late`: 18,000 ticks (30 game-minutes) | 1,418 units, 28,882 entities | 91 s of sim; a tick's mean 5.1 ms, p50 4.4 ms, p99 31 ms; peak 410 MB |
| `late-moho`: the same, Moho pathing | 1,570 units, 26,068 entities; 162,748 searches, 55 M cells expanded | 184 s of sim; mean 10.2 ms, p50 10.3 ms, p99 38 ms; peak 456 MB |
| `render-battle`: 600 frames of tick 6,000 | | CPU `render()` p50 1.2 ms, p95 1.9 ms; GPU 0.49 ms |
| `render-late`: 600 frames of tick 18,000 | | CPU p50 1.6 ms, p95 4.2 ms; GPU 0.75 ms |
| `render-strategic`: the same, zoomed out to icons | | CPU p50 0.95 ms, p95 3.4 ms; GPU 0.34 ms |

Moho pathing costs 1.4× the default's sim time in the early game and 2.0× in the late game. A late-game search expands about 340 cells; an early-game one about 200.

The games differ with it, because the AIs' units move otherwise. From tick 7,000 its ticks cost 2.0-2.3× the default's, for two reasons:
- **More units.** From tick 8,000 it holds 30-40% more units: 1,220 against 870 at tick 10,000.
- **The searches.** In a profile from tick 10,000 (400 samples), the path searches take 26% of its tick, and the navigator's checks along its path another 9%. Half the searches' time is the test for units in the way.

Without those, a unit costs about the same as in the default game (7.6 against 7.0 µs a tick).

Against the 2026-10-06 baselines (b167ab14):
- The default games changed since. The parity and pathing work changed how the AIs play: the late game ends with 1,418 units against 1,666.
- Its ticks are shorter: a mean of 5.1 ms against 5.9 ms.
- The render frames hold within noise.

## Known gaps

What is not yet as Moho does it, or not yet checked. Each is on the roadmap (`docs/ROADMAP.md`, and the external review's work order of 2026-10-06).

- **Pathing by footprint.** By default units still path as points on one 2-unit grid. Moho keeps passability per footprint class, so a big unit can't take a gap a small one can, and its pathing is now ported (#415–#418, `docs/plans/2026-10-07-per-class-pathing-design.md`). It stays behind `--moho-pathing` until games with it on have been watched: the four-AI late game runs 2.0× longer in sim time with it (184 s against 91 s: 162,748 searches, 55 M cells expanded; see *Benchmarks*), and the AIs play differently. Mobile units in the way and formation layers are #427's.
- **Projectile flight.** As Moho's since #405 and #408, but for two things nothing in retail uses: bounce (no projectile has `Min/MaxBounceCount`), and retargeting on a miss, which Moho gates on `AutoInitiateAttackCommand` as well as `ReTargetOnMiss`, a pair no retail weapon has.
- **Needs a person:** a listening pass of the audio; comparison with the original game's look (no reference captures exist here); the hardware cursor on a real display; Steam and the Steam Deck; external testers.
- **Multiplayer:** peers are not authenticated; two players dropping at once can leave the survivors disagreeing; FAF's ICE adapter and client are tested only through stand-ins.
- **Lobby options:** difficulty cheat multipliers are applied by FA's AI scripts, not the engine; `PrebuiltUnits` needs map data.
- **Architecture:** the renderer is being broken into subsystems in small changes: the shadow map, the bloom and the shadow casters are out (#420, #421, #423; the frame's targets in #429). `render()` is down from 756 lines to 143: the frame's CPU updates and each pass (normals, reflection, the scene, the screen's layers, submission) are their own functions. `build_scene`, `init` and `create_pipelines` are still large.

## Game modes and victory

With retail or FAF data, the scenario's own `/lua/victory.lua` decides the game, as in Moho: retail's `BeginSession` hook forks `CheckVictory`, which calls the brains' `OnDefeat`/`OnVictory`/`OnDraw` and then `EndGame`; the engine ends the session (`SessionIsGameOver`, `NoteGameOver`) and retail's score screen ends it for good (`SessionEndGame`). `data.victory-test` plays that through.

For data without a victory script, `SimState::update_victory` enforces FA's modes by `lua/victory.lua`'s categories: Assassination (the last `COMMAND` unit), Supremacy (`STRUCTURE + ENGINEER - WALL`), Annihilation (`ALLUNITS - WALL`) and Sandbox; game-over is team-aware, and a defeated army's units go by the `Share` option. `FogOfWar=none`, `CommonArmy`, per-army handicap, `TeamShareOverflow` and `NoRush` are enforced (`test_victory.cpp`, `test_fow.cpp`, `test_common_army.cpp`, `test_handicap.cpp`, `test_team_share_overflow.cpp`, `test_no_rush.cpp`). Not modelled in that fallback: FA's 15 s allied-victory request, and `TransferToKiller`.

Army statistics use Moho's names and meanings, which retail's score threads read (`Units_History`, `Units_Killed`, `Enemies_Killed`, values built, lost and destroyed, the economy's totals and waste, the unit cap); `GetBlueprintStat` splits them by category.
