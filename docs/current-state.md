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
| Unit tests (Catch2) | <!-- metric:unit_test_cases -->1,104<!-- /metric --> test cases in a Linux build (Windows leaves out a few POSIX-only ones). CI runs them on GCC, Clang, ASan and MSVC. |
| Data-backed gate on retail (`ctest -L gate`) | <!-- metric:gate_tests -->224<!-- /metric --> tests, each a mode of `osc_integration` playing retail's scripts and data (one per system: `--missile-test`, `--footfall-test`, `--selection-render-test`...), plus the flows below. |
| Golden captures (`ctest -L golden`) | <!-- metric:golden_tests -->5<!-- /metric --> pixel comparisons at 0.1%: FA's game interface at frame 600 (with and without the minimap), retail's skirmish lobby and its map list. |
| Two-process MP tests (`ctest -L mp`, data-free) | <!-- metric:mp_tests -->4<!-- /metric --> |
| Architecture checks (`ctest -L arch`) | <!-- metric:arch_tests -->18<!-- /metric -->: no library cycle or layer reaching up; every serialized type's fields in its serializer; the blueprint-field, Lua-stub and retail-hook baselines; this document's metrics. |
| Static analysis | clang-tidy ratchet at its baseline of <!-- metric:tidy_baseline -->31<!-- /metric --> triaged findings; changed lines follow `.clang-format`; CI builds first-party code with `-Werror`. |
| Retail engine API still unbound | <!-- metric:unbound_globals -->16<!-- /metric --> globals and <!-- metric:unbound_methods -->7<!-- /metric --> methods (`opensupcom --binding-coverage`, ratcheted by `tests/integration/binding_baseline_retail.txt`); 13 are not in retail's engine either, most of the rest are UI-only. |

The counts are a Linux build's, written by `tools/status_metrics.py update --build-dir <build>`; `arch.status_metrics` fails when they are stale.

## Acceptance matrix

Each area, what is accepted and by what. "Last verified" names the validation record of the commit it was last checked on; an area's gate tests also run on every later change.

| Area | Accepted | Evidence | Last verified |
| --- | --- | --- | --- |
| Data and boot | Retail FA (Steam) and FAF data boot; retail's front end, lobby and game interface run; mods (a game's `GameMods`, hooks, the player's mods folder) work as Moho's. | `data.mods_flow`, `data.lobby-flow-test`, `data.uiboot-test`, goldens | [e1f275b3](validation/2026-10-06-e1f275b3.md) |
| Long games | Four retail AIs play SCMP_009 for 18,000 ticks with no script error, on retail data (seeds 4242, 7777) and FAF's (7777; 4242 shows one error of FAF's own scripts). | Long runs in the record | [e1f275b3](validation/2026-10-06-e1f275b3.md) |
| Determinism | Two processes play a four-AI game identically, compared domain by domain; GCC, Clang and MSVC reach one pinned checksum; Windows and Linux play together in lockstep. The checksum covers the state that decides a unit's next move (#395). | `data.determinism`, `test_cross_platform.cpp`, `test_checksum_coverage.cpp`, CI `cross-os-play` | [e1f275b3](validation/2026-10-06-e1f275b3.md) (#395 after it) |
| Saves and replays | A save is the whole sim state (every object, the Lua heap), restored and played on; campaign saves load as their operation; replays play back to the same checksums. | `data.save_load`, `data.save_load_campaign`, `data.save_load_replay`, `data.load_flow`, `data.replay_roundtrip`, `data.replay_flow` | [e1f275b3](validation/2026-10-06-e1f275b3.md) |
| Campaign | Operations launch as retail's `SetupCampaignSession`; the front end runs from operation select through briefing, operation, score and the next briefing; the tutorial and the outro play. | `data.campaign_flow`, `data.tutorial_flow`, `data.outro_flow`, `data.campaign-test` | [e1f275b3](validation/2026-10-06-e1f275b3.md) |
| Multiplayer | LAN lobby (discovery, host, join, launch) and GPGNet (FAF's client protocol) to a lockstep game; desync detected; a vanished or slow peer handled; chat, pause and speed. | `mp.*`, `data.lan_game`, `data.lan_game_quit`, `data.gpgnet_lobby`, `data.gpgnet_game`, `data.gpgnet_game_relayed`; a scripted FAF-client harness | [e1f275b3](validation/2026-10-06-e1f275b3.md) |
| Combat | Retail's weapon state machines (racks, salvos, turret slew and tolerance); aim at the target's TargetBones, as Moho's target points (#397); projectiles and beams with script-decided collisions; missiles, silos and their defences; shields; damage, armour and veterancy. | `data.{weapon,fire,aim,targeting,projectile,missile,silo,beam-weapon,defence,shield,damage,armor,vet}-test`, `test_target_points.cpp` | [e1f275b3](validation/2026-10-06-e1f275b3.md) (#397 after it) |
| Movement and orders | Moho's drive (acceleration, braking, turning), formations, steering and crowds; move, attack, guard, patrol, ferry, assist and script orders; Moho's order filter. | `data.{move,drive,formation,steer,crowd,path,canpath,patrol,guard-engage,ferry,script-order}-test` | [e1f275b3](validation/2026-10-06-e1f275b3.md) |
| Air | Winged attack runs and bombing; air staging (docking, refuel, repair); carriers; transports (slots, pickup, drop). | `data.{air-attack-run,air-auto-engage,air-turn,air-staging,carrier,carrier-land}-test`, `data.transport-*-test` | [e1f275b3](validation/2026-10-06-e1f275b3.md) |
| Construction and economy | Placement by layer, adjacency, upgrades, enhancements, reclaim, repair, capture, economy events (teleport, OverCharge), the unit cap. | `data.{build,construction,placement-layers,adjacency,upgrade,enhance,reclaim,repair,capture,economy,unit-cap}-test` | [e1f275b3](validation/2026-10-06-e1f275b3.md) |
| Intel and fog | Moho's intel grids (vision, radar, sonar, omni), counter-intel and jammers, blips and remembered structures, LOS. | `data.{intel,unit-intel,counter-intel,jammer,jammer-blip,fow,los}-test` | [e1f275b3](validation/2026-10-06-e1f275b3.md) |
| Animation collisions | A walker's footfalls and their damage; a crashing CZAR's or Ahwassa's bones told once each on reaching the ground, with their scripts' crash damage; detector state across a save. | `data.footfall-test`, `test_checksum_coverage.cpp` | #400 (open) |
| Rendering | FA's terrain, water, sky, unit materials, decals, shadows, effects, strategic icons and fog, by FA's own shaders; drawn between the sim's ticks. | The `*-render-test` modes, `data.shadow-map-test`, `data.water-render-test`, goldens | [e1f275b3](validation/2026-10-06-e1f275b3.md) |
| UI | Retail's own interface: the MAUI controls, focus and keys, the key map and console, the orders and construction panels, selection, the minimap. | `data.{gameui,ui,controls,keyboard,keymap,selection-render}-test`, goldens | [e1f275b3](validation/2026-10-06-e1f275b3.md) |
| Audio | FA's XACT data through an app-owned cue engine: interface, voice, music, unit and weapon sounds, as Moho plays them. Checked by data and timing tests only. | `data.audio-test`, `data.audio-data-test`, `data.unitsound-test` | [e1f275b3](validation/2026-10-06-e1f275b3.md) |
| Performance | See *Benchmarks* below. | `tools/bench.py` | See below |

## Benchmarks

`tools/bench.py` (Release, four retail AIs on SCMP_009, seed 4242, an RTX PRO 4500 at 1920×1080), recorded on 2026-10-06 with main's engine at b167ab14 (built from d97d43f8, which changes only tests) as the baselines `bench.py check` holds later builds to:

| Scenario | Game at the end | Measured |
| --- | --- | --- |
| `early`: 6,000 ticks (10 game-minutes) | 555 units, 11,926 entities | 6.7 s of sim; a tick's mean 1.1 ms, p50 0.7 ms, p99 9.7 ms; peak 290 MB |
| `late`: 18,000 ticks (30 game-minutes) | 1,666 units, 24,569 entities | 106 s of sim; a tick's mean 5.9 ms, p50 5.1 ms, p99 36 ms; peak 391 MB |
| `render-battle`: 600 frames of tick 6,000 | | CPU `render()` p50 1.3 ms, p95 2.3 ms; GPU 0.64 ms |
| `render-late`: 600 frames of tick 18,000 | | CPU p50 1.4 ms, p95 3.0 ms; GPU 0.65 ms |
| `render-strategic`: the same, zoomed out to icons | | CPU p50 0.85 ms, p95 2.4 ms; GPU 0.34 ms |

Against the previous baselines (2026-09-29/30), the renderer's CPU frame fell from a p50 of 6-8 ms to 1-1.4 ms. The sim's games aren't comparable: the parity work since changed how the AIs play, and the late game now ends with 54% more units (1,666 against 1,081), its ticks about twice as long (a mean of 5.9 ms against 2.9 ms).

## Known gaps

What is not yet as Moho does it, or not yet checked. Each is on the roadmap (`docs/ROADMAP.md`, and the external review's work order of 2026-10-06).

- **Pathing by footprint.** Units path as points on one 2-unit grid; Moho keeps passability per footprint class, so a big unit can't take a gap a small one can. The classes, and the one each unit paths as, are loaded (#398); the occupation grid and Moho's fit test, then per-class pathing, are next (`docs/plans/2026-10-06-footprint-classes-design.md`). Props worth reclaiming don't yet block.
- **Projectile flight.** Shots aim at their target's TargetBones and seabed targets are judged by them (#397), but shots leave aimed at their target rather than along their muzzle, and homing shots steer their velocity, not Moho's orientation-then-thrust (`MotionTick`); zig-zag, bounce and retargeting on a miss are not modelled.
- **Idle aircraft.** They hover where their last order left them; Moho's land after `AutoLandTime` (1 s on most of retail's aircraft) and circle by their `Circling*` fields.
- **Selection.** Units are picked by the ground under the cursor, so an aircraft can't be clicked where it is drawn (fixed in #401, open).
- **Needs a person:** a listening pass of the audio; comparison with the original game's look (no reference captures exist here); the hardware cursor on a real display; Steam and the Steam Deck; external testers.
- **Multiplayer:** peers are not authenticated; two players dropping at once can leave the survivors disagreeing; FAF's ICE adapter and client are tested only through stand-ins.
- **Smaller order gaps:** a repeating factory's queue shows an order split by its trip round the queue as two entries; the AI's influence maps take no false blips from jammers.
- **Lobby options:** difficulty cheat multipliers are applied by FA's AI scripts, not the engine; `PrebuiltUnits` needs map data.
- **Architecture:** the renderer is still one large unit, to be broken into subsystems in small changes.

## Game modes and victory

With retail or FAF data, the scenario's own `/lua/victory.lua` decides the game, as in Moho: retail's `BeginSession` hook forks `CheckVictory`, which calls the brains' `OnDefeat`/`OnVictory`/`OnDraw` and then `EndGame`; the engine ends the session (`SessionIsGameOver`, `NoteGameOver`) and retail's score screen ends it for good (`SessionEndGame`). `data.victory-test` plays that through.

For data without a victory script, `SimState::update_victory` enforces FA's modes by `lua/victory.lua`'s categories: Assassination (the last `COMMAND` unit), Supremacy (`STRUCTURE + ENGINEER - WALL`), Annihilation (`ALLUNITS - WALL`) and Sandbox; game-over is team-aware, and a defeated army's units go by the `Share` option. `FogOfWar=none`, `CommonArmy`, per-army handicap, `TeamShareOverflow` and `NoRush` are enforced (`test_victory.cpp`, `test_fow.cpp`, `test_common_army.cpp`, `test_handicap.cpp`, `test_team_share_overflow.cpp`, `test_no_rush.cpp`). Not modelled in that fallback: FA's 15 s allied-victory request, and `TransferToKiller`.

Army statistics use Moho's names and meanings, which retail's score threads read (`Units_History`, `Units_Killed`, `Enemies_Killed`, values built, lost and destroyed, the economy's totals and waste, the unit cap); `GetBlueprintStat` splits them by category.
