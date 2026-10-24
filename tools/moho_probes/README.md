# Probes for comparing the engine with the original game

Some of the engine's rules come from faf-re's decompiled Moho, and a few of their consequences surprised us. Only the original game can say whether it behaves the same. These probes ask each question the same way of both: an init file mounts the hooks ahead of the game, the hooks build each scene on Seton's Clutch (SCMP_009), and every answer is a `PROBE ...` line in the log.

Nothing here patches the game's scripts. The hooks wrap two functions to start the scenes and to read the start's selection; `engineer/` also wraps its own engineer's build callbacks, on that unit alone, to log them.

## Running them

**On the engine** (offscreen, headless):

```sh
opensupcom --init tools/moho_probes/init_probes.lua --map /maps/SCMP_009/SCMP_009_scenario.lua \
    --ticks 1800 [--moho-pathing] --user-dir <scratch>
grep PROBE opensupcom.log
```

The start's selection is asked by the interface, which a headless run doesn't have. Ask it with a window (offscreen), e.g. `--screenshot-frame 400 --screenshot <png>` in place of `--ticks`.

**On the original game,** through Steam (Proton) or Wine:

```sh
steam -applaunch 9420 /init 'Z:\<repo>\tools\moho_probes\init_probes.lua' /map SCMP_009 \
    /nomovie /nosound /windowed 1280 800 /log 'Z:\tmp\probes.log'
```

- `/windowed <w> <h>` keeps the game in a window, whatever its options say. Without it, a game set to full screen changes the display's mode.
- The game never ends on its own. The probes are done when `PROBE choke monkeylord` and `PROBE rebuild after-kill tick=800` are both in the log, about three and a half minutes in. Then close the game: kill its process (`pgrep -a -i supremecommand`).
- It rewrites its shader caches in the prefix (`AppData/Local/Gas Powered Games/.../cache`). To leave the prefix as it was, copy that folder aside first and back after.
- `init_probes.lua` holds the game's folder twice, once as Linux sees it and once as Wine sees it. Edit both for another machine.

## The questions, and the answers

The original game answered on 2026-10-12: retail FA (Steam) under Proton, run twice. The engine answered on main 3be4d6a5, with the grid's pathing and with `--moho-pathing`; the FormAttack row also with #605.

| Probe | The question | The original | The engine | So |
|---|---|---|---|---|
| `startup-select`, `startup-clock` | Is the commander selected at the start? Retail's `OnFirstUpdate` selects it once, 3 s in; the warp keeps it unselectable from tick 30 for 2.1 s. `startup-clock` logs the UI's tick against the wall clock from `OnFirstUpdate` on. | Selected in run 1 (at UI tick 28), not in runs 2 and 3 (tick 30). Run 3's clock: `OnFirstUpdate` at tick 0; tick 5 at 0.76 s, then 10 a second. | Main: `OnFirstUpdate` at tick 1, tick 9 at 0.66 s, select at 32–34, never selected. With the median frame clock: tick 0, tick 4 at 0.65 s, select at 29, selected (3 runs). | It depends on the frame times. Moho's beat clock takes the median of the last frame times (`CScApp::Main`, `RollingFrameRates`), so a slow frame loses its excess, and the first frames after the interface appears leave the UI's tick behind the wall clock. The engine had run its sim on each frame's own time. |
| `units-around-point` | Which points does `brain:GetUnitsAroundPoint(cat, pos, r, 'Ally')` find a 2x2 T1 power generator from? Retail's `EngineerTryRepair` asks with r = 1 before each build. | From the centre strictly within r, across the map (height ignored), a circle: r = 1 finds it from 0.99 off but not from 1.0, nor from 0.75 off on both axes; r = 0 finds nothing. `GetNumUnitsAroundPoint` answers the same at every point. | From d = 0 to 4: the 4-unit collision cells of faf-re's `func_GetUnitsAroundPoint`, with no distance test | faf-re's decompile misses the distance test. The engine's cell gather is wrong, and with it the halving of retail's AI since #591. |
| `shore` | At the strait at z 520, whose banks fall about 1.2 a cell, do a hover and an amphibious tank cross, or drive round by land? | Both drive round, arriving at ticks 1196 and 1201, only ever on Land | Both drive round, arriving 1066–1186 ticks in, only ever on Land | The same: the engine's `MaxSlope` reading of `OCCUPY_MobileCheck` holds. |
| `choke` | 24 Strikers through a 4-wide gap in a wall: how many through the gap, through wall cells, stopped short? Does the Monkeylord walk over the wall? | 24 / 0 / 0, and the Monkeylord walks over the wall (x 678.25) | Grid: 18 / 6 / 0, and the Monkeylord goes round (x 649). Moho pathing: 21 / 0 / 3, and over the wall (x 679). | Moho pathing's port misses a rule that gets the last units through; the Monkeylord matches it. |
| `formattack` | Six Strikers given `IssueFormAttack(..., 'AttackFormation', 0)` on a point, beside six given `IssueAttack`: how spread are they at ticks 60, 120 and 200? | Formed: 15.0, 15.0, 13.4. Plain: 13.2, 10.4, 11.3, driving straight on side by side. | Main: both as a plain attack, closing to 4.4. With #605: formed holds 15; plain closes to 4.4. | #605's FormAttack matches. A group's plain attack on a point doesn't close up in the original: the engine's does. |

**Asked on 2026-10-13** (the original twice, the engine on main 904e9d5b and with the `REBUILDER` change):

| Probe | The question | The original | The engine | So |
|---|---|---|---|---|
| `forest` | 12 Strikers driven north through a forest (x 128-192, z 640-704): how many of its 149 tree groups break up, and when? | 137, 69 and 29 groups left at ticks 100, 200 and 400 (232, 1,843 and 2,681 single trees), the same in both runs | 121, 45 and 26 groups (580, 2,480 and 2,821 trees) | As many break up by the end. The engine breaks them up sooner. |
| `rect-order` | Three Strikers made in one cell, then the first warped away and back: in what order does `GetUnitsInRect` give them? | `3 2 1`; away, `3 2`; back, `1 3 2` | The same since #627 | Moho's collision cells list the last linked first. |
| `prop-orient` | The tree groups in `Rect(116, 650, 132, 690)`: their orientations and collision boxes | 34 groups | The same quaternions, and boxes within 0.001, since #628 | A `.scmap` prop's axes are its rotation's columns. |
| `unit-box` | A Seraphim artillery (`CollisionOffsetZ` -1.5) at four headings: its box | The offset turns with it | The same | Units' boxes were right. |
| `rebuild` | A support commander (`REBUILDER`) guards a power generator that is then destroyed, and an engineer guards another. Is it rebuilt? | The support commander's Guard stays, and it goes to the site (2.7 off); the engineer's ends. Nothing is rebuilt. | Before: both Guards end. Now: as the original (0.9 off). | A Guard from `IssueGuard` carries no blueprint (faf-re `cfunc_IssueGuardL`), and `LookForStructureRebuilder` matches on the Guard's blueprint, so a script's Guard never rebuilds. Whether a player's right-click Guard does is not yet known. |
| `scu-assist` | A support commander and an engineer guard a factory building tanks: does each build with it? | The engineer does; the support commander doesn't. The factory's progress is 0.09, 0.20 and 0.45 at ticks 60, 150 and 300. | Before: both did. Now: as the original. The factory's progress is 0.02, 0.05 and 0.11. | Moho's guard task takes up no live unit a `REBUILDER` guards. The low progress in both is the scenes' shared economy, starved of energy (the choke scene's Monkeylord upkeep among them), not the factory: see `economy` below. |
| `scu-follow` | A support commander guards a tank driven 70 north: does it follow? | Yes: 16.7 and 31.0 behind at ticks 100 and 250 | Yes: 18.3 and 23.3 behind | Both follow; the original's lags a little more. |

Found along the way: a land tank made in water 4 deep (at 560, 300) was gone by tick 250 in the original, while the engine's stood there unharmed. `drown/` below asks why.

The probes play the scenes at once, after the commanders' warp-in (tick 80). They make every other army neutral first, because the map's other commanders start near two of the scenes.

## Probes run on their own

The scenes above share one army's economy, so a question about the economy gets an answer of its own. Each folder below has its own `init_probes.lua`. Run it in place of the one above, on both games, the same way.

**`economy/`** puts a T1 land factory with a T1 engineer guarding it through three Strikers, three times over:
- **alone**, on the army's starting economy;
- **stalled**, with energy drained and a Monkeylord's 250 a second upkeep beside it;
- **ample**, after `GiveStorage` and `GiveResource`.

At fixed ticks it logs the factory's progress and energy use, the engineer's focus, and the army's energy and mass (stored / income / requested). It is done at `PROBE econ done`.

**`timing/`** gives a factory three Strikers and an engineer a power generator, with ample resources. It logs their state, work progress, `GetResourceConsumed` and focus: the factory at t=0–12 and t=130–160, and the engineer every tick from t=0 to 40. It is done at `PROBE timing done`.

**`engineer/`** looks closer at how an engineer starts a build. A T1 engineer at (260, 790), heading 0, is told to build a T1 power generator behind it, at (262, 784), with ample resources. Every tick from the order to t=20 it logs:
- the engineer's state, progress, `GetResourceConsumed`, focus and heading;
- its build turret's direction (`GetBoneDirection('Turret')`).

It also logs:
- each build callback as it runs (`OnPrepareArmToBuild`, the open animation's wait, `OnStartBuild`, `OnStopBuilderTracking`), wrapped on that engineer alone. It also wraps `BuildManipulatorSetEnabled`, which neither game called: retail's open-animation wait enables the arm itself;
- where the frame appears.

It is done at `PROBE eng-build done`.

**`drown/`** makes units in water their motion type can't be on, each on its own rows at the first spot as deep as it wants:
- UEF T1 tanks (UEL0201, `RULEUMT_Land`) asked for at 0.2, 0.6, 1.2, 2.5 and 4 deep (the first spot within 0.1; on SCMP_009 they land 0.28, 0.53, 1.14, 2.42 and 4.10 deep);
- in 4 deep, an amphibious tank (URL0203), a hover tank (UEL0203) and an engineer (UEL0105, `AmphibiousFloating`).

Every `WaitTicks(5)`, which is 4 ticks, it logs each one's layer and health until it is gone. The log's `t` counts 5 a step, so its `t=125` is the 100th tick. It is done at `PROBE drown done`.

**`layers/`** asks the same of the motion types `drown/` left out, each made by script at the terrain's height:
- T1 subs of each faction in deep water, and one in water too shallow for a sub;
- a frigate in deep water;
- a T1 scout and two experimental aircraft (Soul Ripper, CZAR) on dry ground.

At ticks 0, 1, 5, 20 and 60 it logs each one's layer, height and health. Its waits are timed so the log's `t` is the tick. It is done at `PROBE layers done`.

**`warp/`** asks where `Warp` puts a unit. Units are made on SCMP_009's dry ground east of x 650 (the frigate in the sea west of x 450), and after 10 ticks each is warped, on its own row:
- a tank 10 over the ground, and one 3 under it;
- a tank and an amphibious unit (Wagner) into deep water at the surface's height;
- a hover tank and a frigate over deep water, to height 0;
- a scout and a CZAR on dry ground, to height 0.

It logs each one's layer, height, health and orders before, at once after, and 1, 5 and 20 ticks after. Then a tank on its way to a point 80 east is warped 20 aside, and logged every 10 ticks until its orders are empty. It is done at `PROBE warp done`.

Asked on 2026-10-14 (`warp` on 2026-10-15): the original once (`engineer` twice, with identical runs); the engine on main 7f09e969, with #642, with #646, with #653, and with #655.

| Probe | The original | The engine | So |
|---|---|---|---|
| `economy` start | energy 4000, mass 650: the commander's own storage | 4200 and 850 before #639; now the same | An army has no storage of its own. |
| `economy` after `GiveStorage('ENERGY')` then `GiveStorage('MASS')` | energy storage unchanged, mass raised | both raised before #639; now the same | `GiveStorage` replaces the army's one extra pair (`cfunc_CAiBrainGiveStorageL`). |
| `economy` alone | 0.0069 progress a tick | the same | The factory builds at the same rate. |
| `economy` stalled | progress at income over requested (about 7%); mass rose 17.2 over the stall | progress the same; mass fell 101 before #640, rises 17.1 now | Moho grants a request wanting both resources the same share of each (`ProcessArmyEconomy`). |
| `timing` factory | the Striker at t=1, `GetResourceConsumed` 0 at t=1–2, progress from t=3, done at t=142, 0 the tick after | progress from t=1 before #642; now identical on all 44 logged ticks | A unit works at last tick's grant (`ResourceConsumed`). |
| `timing` engineer | Building from t=2, the frame at t=10, progress from t=12 | Building t=1, frame t=6, progress t=8 with #642; identical on all 41 logged ticks with #646 | Moho's mobile build task starts later: see `engineer`. |
| `engineer` site | the frame at (262.5, 784.5) | (262, 784) before #646; now the same | The build task snaps its site to its footprint's cell, a script's order as the UI's. |
| `engineer` start | `OnPrepareArmToBuild` after t=1, Building from t=2 | after t=0 before #646; now the same | The task's first tick only prepares (faf-re's `TASKSTATE_Preparing` returns 1). |
| `engineer` arm | the open animation's wait ends after t=6; the turret turns 36° a tick from t=7 (36°, 72°, 108°, 144°) and settles at 155° at t=11; `OnStartBuild` after t=9 | the wait ended after t=4, the arm turned in one tick, `OnStartBuild` after t=5, before #646; all 32 logged rows the same now | Manipulators move before a unit's tasks (`Unit::MotionTick`), and `SetAimingArc`'s slews are degrees a second. |
| `drown` layers | every tank, however shallow the water, and the amphibious tank on the Seabed; the hover tank and the engineer on the Water | all on Land before #653; now the same | A unit takes the layer of the place it is made (`Entity::GetStartingLayer`). |
| `drown` tanks | 12 health off every 4 ticks (1% of 300 a tick); gone on the 100th tick | unharmed before #653; all 232 logged rows the same now | A surface unit on a layer its motion type can't be on loses 1% a tick (`CUnitMotion::CalcMoveCommon`). |
| `drown` others | the amphibious tank, the hover tank and the engineer unharmed | the same | They are on layers their motion types allow. |
| `layers` subs | on the Sub layer at the surface −1.5, even in water too shallow for a sub; the Seraphim sub surfaces by t=60 | the same (its surfacing within 0.06) | A sub starts at its depth (`IUnit::CalcSpawnElevation`). |
| `layers` frigate | on the Water at the surface from t=0 | on the seabed (5.05) before #655; now the same | A ship starts at the water's surface. |
| `warp` heights | the tanks warped over and under the ground stand on it; the tank and the Wagner in deep water on the Seabed at the ground under it; the hover tank and the frigate on the Water at the surface; the scout in the Air 18 over the ground; the CZAR on Land at the ground | each where Warp's height put it, on its old layer, before #672; now the same | `Unit::Warp` takes the place's layer with none asked for (`GetStartingLayer`) and its height (`CalcSpawnElevation`), as a unit made there. |
| `warp` after | the tank in deep water drowns (297, 285, 240 at ticks 1, 5, 20); the CZAR is in the Air from tick 1, at 19.94, 24.88, 37.15 | the same | A warped unit is in the same state as one made there. |
| `warp` mover | its move order is gone within 10 ticks; it coasts 2.04 and stops | it went on to its goal before #672; now its order ends, and it coasts 1.53 | Warp aborts the navigator's move, and the move task takes the abort as a failure (`CUnitMoveTask::OnEvent`). The coast differs with the unit's braking, not Warp. |
| `layers` aircraft | the scout in the Air, 18 over the ground under it; the Soul Ripper and the CZAR on Land at the ground, in the Air and climbing from t=1 | the experimentals in the Air at full height before #655; now the same through t=5, within 0.25 after | `GetStartingLayer` puts an experimental aircraft on Land; it lifts off with no order. The scout's drift and the later climb are air physics, not ported. |

**`crowd/`** asks how a crowd gets through a gap. It uses `choke`'s scene: a wall along z 175 from x 650 to 790, with a gap 4 wide at x 718–722, and 24 Strikers sent to (720, 200). It runs that scene six times, one after another:
- v0 as `choke` places the crowd;
- v1–v5 with each spawn point moved up to 0.8 by a fixed sine.

Every 2 ticks it logs each Striker's position and the length of its order queue. After 400 ticks it logs those that never crossed. It is done at `PROBE tr-all-done`.

**`navigator/`** asks which cells a lone Striker's navigator heads for on the way to the gap. With no crowd, Strikers start one at a time from (730, 154), (742, 158) and (700, 168). Every tick it logs:
- the position;
- `navigator:GetCurrentTargetPos()`, the path navigator's target cell centre;
- `GetStatus()`.

It is done at `PROBE target-all-done`.

Asked on 2026-10-15: the original once each; the engine on main 44d01b33 and with the path navigator ported from the binary.

| Probe | The original | The engine | So |
|---|---|---|---|
| `crowd` stopped short | 0, 0, 0, 0, 1, 0: one Striker of 144 stands south of the gap (720.99, 171.11) with its order ended | 1, 2, 2, 2, 3, 1 on main; 0 in all six now | On main, the crowd's units were pinned against the wall's face. |
| `crowd` the wall | 139 of 143 Strikers first reach z 173.8 at x 716–723, at the gap (the others at 703–715); at most 6 at the wall's face at once; none but the one stopped short stands still more than 22 ticks | on main 73 of 144 at the gap and the rest along the face from 711 to 731, up to 15 at the face, some still for 272–312 ticks; now 140 of 144 at the gap (the others at 724–727), at most 6, none still more than 16 | Moho's units make for the gap before they get to the wall. |
| `navigator` targets | from (730, 154): (724, 160) at t=3, (724, 171) at t=36, (721, 174) at t=53, (720, 198) at t=75, the goal at t=113. Likewise from the other two starts. | on main: (724, 160), then (724, 192), the far side's portal, through the wall, until the wall refuses it at t≈70; now the original's cells (but one: (720, 199) for (720, 198)), one to seven ticks sooner. (The original's Striker barely moves for its first ten ticks or so, turning; the engine's sets off sooner.) | Moho's navigator advances the target only halfway to it, and looks down the way every 7 ticks. Where nothing ahead can be reached straight, it asks the way cell by cell. faf-re's text differs from the shipped exe in these, and in five more rules: see the engine's `path_navigator.cpp`. |

**`carto/`** asks how the main view looks cartographic. It works from the interface's side (a hook on `gamemain.lua`'s `OnFirstUpdate`):
1. Past the opening's camera, it resets the main view to the whole map (`GetCamera('WorldCamera'):Reset()`) and dumps a frame.
2. It makes the main view cartographic (retail's `ToggleMainCartographicView`) and dumps a frame again.

It is done at `PROBE carto done`.
- **The original** writes the frames with its `dump_Frame` console command as `SCFrame_<time>_<n>.bmp`, into the folder `PROBE_DUMP_DIR` names. It doesn't make that folder, so `mkdir /tmp/carto_dump` first.
- **The engine** has no `dump_Frame`. Capture it with `--screenshot-frame 1500 --screenshot <png>`.

Run both at the same size: `/windowed 1600 900` on the original, the size of the engine's captures.

Asked on 2026-10-16: the original once; the engine with #677.

| Probe | The original | The engine | So |
|---|---|---|---|
| `carto` colours | deep water (48, 94, 121), its shallows (94, 135, 150), the lowland (80, 68, 72) and (80, 68, 73): the most common colours of the frame | the same colours, in the same order | The height ramp, its water lanes and the contour bands match the port of `Cartographic`. The water is the decompile's two blues: colour [1] never shows. |
| `carto` framing | the whole map 640 pixels high: the zoom limit is 1.4 times the map | 900 pixels: 1.0 times | Retail's `minimap.lua` sets its *own* camera's limit to 1.0 (`GetCamera('MiniMap')`). The engine's `GetCamera` gave every name the main view's camera, so this cut the main view's. It is fixed in the PR that added this probe: only `WorldCamera` is the engine's camera. |
| `carto` lines | the contour lines, the shore's bands and the small contour islands | the same shapes, aligned within a pixel once framed alike (the camera fix), except at sub-pixel steps along the lines | The edge detect matches. |
| `carto` deposit icons | no halo round the mass and hydrocarbon icons in the first run. Retail's `bloom_render` option is off by default, and a fresh `/prefs` gets that. With `ren_bloom 1`, ten frames 0.3 s apart show a halo pulsing across the icons as a wave. | a halo, its glow the same 0.6·sin²(t − 10·α) wave | The same. The engine's captures keep bloom on, while a player's game applies retail's option. |

**`drive/`** asks how a lone ground unit starts and ends a move. A Striker made at (300, 790) facing +z is driven to (300, 750), then (330, 750), a quarter turn, then back to (300, 750), a half turn. Every tick it logs its position, its orientation and whether it is `Moving`. It is done at `PROBE drive done`. Its hooks also set `sim_KeepAllLogFiles` from the interface, so the original run with `/synclog <dir>` keeps a file a beat: without it Moho deletes each beat's file unless that beat went out of sync. Beat 0's holds every blueprint as Moho filled it in; each later one the units' motion steps (`MoveTo`, `DriveToNextWaypoint`) and positions.

**`buildreach/`** asks where a builder out of reach of its site stops and starts to build: an engineer 9 west of a point defence's site (the engine's range test 9), one 10 north of a power generator's, and one 12 west of a land factory's, each logged until it builds and 20 ticks on. It is done at `PROBE reach done`.

Asked on 2026-10-15 and 2026-10-20: the original three times (`drive`, once with `/synclog`) and once (`buildreach`); the engine on main 9f454781 and with the steering speed cap, coast and build move of `docs/plans/2026-10-20-moho-steering-speed-cap-design.md`, `--moho-pathing` on for `drive`.

| Probe | The original | The engine | So |
|---|---|---|---|
| `drive` straight | 0.34 a tick faster to 3.40, then 3.40; 9 degrees a tick | the same | The drive's acceleration, top speed and turn rate match. |
| `drive` start | from rest, turns from t=4 and drives from t=23 (east leg 13), 1 degree off its heading. In `/synclog` its nav tick is empty on the order's tick and the next, then `UpdateCurrentPosition()`. | on main drives from t=18, 46 degrees off; now from t=23 (13), 1 degree off | The path comes two ticks after the order (Moho's path queue). A Striker (`RotateOnSpot`) stands till its goal is within acos 0.98 of its heading: Moho's steering speed cap. |
| `drive` end | done at t=143, 105, 114 still at about 2.0, in the goal's cell; coasts 2.04, 1.29, 0.69, 0.21, still at (300.49, 750.68) | done at t=144, 105, 114; at rest within 0.03 of the original's points | The move ends on entering the goal's cell, then the unit coasts as `CalcMoveCommon` damps it: 0.8 of its speed less its brake a tick. |
| `buildreach` point defence | builds from (606.03, 110.52), 3.47 from the site | within 3 on main (the range test's bound then); 3.04 now (3.30 with `--moho-pathing`) | Moho sends it to the cell nearest the site clear of its skirt grown by a cell, touching counting (`PrepareMove(1, site, skirt)`), and it builds once in that cell. |
| `buildreach` power generator | builds from (640.50, 107.13), 3.37 from the site | 3.03 now | The same rule. |
| `buildreach` land factory | builds where it stands, 12 from the site, on t=2 | the same | Its skirt is 8: the gap, 3, is within its `MaxBuildDistance`. |

**`drivegrid/`** asks how every ground unit drives, the same legs for each. The land units (not air, able to move) go in batches, each on a lane of its own on the flat ground at x 584–704, z 70–120: made side-on to the lane and driven 40 down it (a turn at the start), back 30 up it (a half turn), to a point 3 behind it (backing up, within `BackUpDistance`), and 6 on (a short hop, below top speed). Each tick it logs every unit's position, heading and `Moving` state, and once each the blueprint fields its drive reads. It is done at `PROBE dg done`.
- `drive_diff.py retail.log opensupcom.log` compares the two logs leg by leg: the turn, its side, the top speed and the tick the leg ends.
- Asked on 2026-10-21 and 2026-10-22: of 337 legs of 85 ground units, top speeds off the original's fell from 47 to 20, ends off its tick from 39 to 10, and turns the wrong way from 68 to 15, with the spline states of `docs/plans/2026-10-22-moho-reverse-gears-design.md`.

**`blueprints/`** compares every unit blueprint with the original's. Run with `/synclog <dir>`, the original writes a file a beat, and its hooks set `sim_KeepAllLogFiles` from the interface so that it keeps them (without it Moho deletes each beat's file unless that beat went out of sync). Beat 0's file holds every blueprint as Moho filled it in, after its constructors and `OnInitBlueprint`: the defaults a `.bp` leaves out included. On the engine, its other hook logs each blueprint as the scripts see it, a `PROBE bp <id> <path>=<value>` line a field; a few ticks are enough. Then:

```sh
python tools/moho_probes/blueprints/bp_diff.py <dir>/<date>/beat00000.log opensupcom.log \
    src/blueprints/moho_reflected_fields.cpp
```

It lists, per field, the units whose values differ, then each field the `.bp` files leave out with the value Moho gives it, for the engine's code to supply. With the third argument, each of those is tagged `[filled]` or `[unfilled]` by the engine's table of reflected fields (`docs/plans/2026-10-23-moho-blueprint-reflection-design.md`); a `[filled]` one still missing is a bug in that table's layer. Beat 0 prints Moho's C++ copy, whose footprint and skirt are derived (its footprint class's), while the scripts' tables keep the `.bp`'s; those fields are listed however the engine's units hold them.

Asked on 2026-10-15: the original once; the engine on main 9f454781. 383 of 48,722 fields the two share differed (nearly all the footprint and skirt), and six defaults were not Moho's. `docs/plans/2026-10-20-moho-blueprint-defaults-design.md` ports four of them.

| Field | The original | The engine | So |
|---|---|---|---|
| a mobile unit's footprint | its `footprints.lua` class's (134 of 134 ground-mobile units; almost all 1x1) | `ceil(SizeX/Z)`, e.g. 1x2 | `ComputeDerivedQuantities` replaces the footprint with its class's. Fixed by the collision-cone change (M203f), which changes who yields in steering. |
| its skirt | at least the class's footprint (268 of 268 axes) | at least the old footprint | The same function. Fixed with the footprint (M203f). |
| `BackUpDistance` left out | `SizeZ * 3` (495 of 495) | 0 | Fixed with the note. |
| `DesiredShooterCap` left out | 3 (278 of 287 projectiles) | 0 | Fixed with the note. |
| `FiringTolerance` left out | 0.01 (37 of 37 weapons) | 0 | Fixed with the note. |
| `LifeBarSize` left out | 1 (12 of 12) | 0 | Fixed with the note. |
| `TurnRadius` left out | 5 (419 of 419) | 0 | Not changed: without Moho's spline its effect on turns is unmeasured. |

**`steer/`**, **`cone/`**, **`corner/`** and **`crowdgap/`** ask how Moho resolves a meeting, and how a unit wedged at a wall ends. Their hooks set `sim_KeepAllLogFiles` from the interface, so the original run with `/synclog <dir>` keeps its beat files, in which Moho logs each resolution: `ResolvePossibleCollision(unit, other)`, then `collide.` and `within cone.` or `not within cone.`
- `steer`: the engine's steer test 3, a Pillar and an engineer of one army head-on, half a cell apart sideways, 24 each side of (640, 110) and then 40. Done at `PROBE steer done`.
- `cone`: pairs meeting at several angles: overtaking (one army, two), crossing at 90 degrees (two armies, one), at 60 and at 120, and head-on (two Strikers, an engineer and a Striker). Done at `PROBE cone done`.
- `corner`: a Striker at the end of a wall of `ueb5101` two cells thick, ordered to a goal whose line clips the wall's end, from four places. Done at `PROBE corner done`.
- `crowdgap`: `test_moho_pathing`'s crowd of twenty through a gap, with Strikers: a wall two cells thick with a gap of 6, the twenty 5 by 4 west of it, each sent to its own place east of it. Done at `PROBE crowdgap done`.

Asked on 2026-10-20: the original once each, with `/synclog`; the engine with the cone of `docs/plans/2026-10-21-moho-steering-cone-design.md`, `--moho-pathing` on.

| Probe | The original | The engine | So |
|---|---|---|---|
| `steer` | the engineer, the Pillar coming at it, keeps its speed and veers 3.44 (near) and 3.67 (far) off its line; the Pillar stops a few ticks, 0.74 and 1.11 off its line; done in 263 and 433 ticks | 3.03 and 4.08; the Pillar stops, 0.74 and 1.11; 262 and 432 ticks. Before: the engineer stopped. | Moho's cone: the unit the other comes at steps aside, beside the other on its own side, and the other stops (retail 0x0059DA00). |
| `cone` | 11 resolutions: within the cone exactly when the unit is in the other's forward cone (dot 0.707; borderline crossings at 0.70 are not) | the same test | faf-re's decompile reads the cone the other way round. Where each unit then drives fits the binary's step aside in 6 of 8 (a slow unit turning and one backing aside). Overtaking is the path navigator's: the faster unit's target cell moves 4 aside on the order's third tick. |
| `corner` | from 0.002 below the wall's end it moves 0.02 by tick 9, then stands still; its order ends on tick 40. From 0.3 below or more it drives out. | stands still from the start; its order ends on tick 33. From 0.3 and 0.5 below it drives out on the original's line to the thousandth; toward the goal further north, within 0.35 of it. Before, a bounced unit could work itself free of such a place, or rock in it with its order (`test_moho_pathing`). | A refused step leaves the unit where it was (`CalcMoveCommon`), so its navigator sees it still and gives the move up after 30 ticks. |
| `crowdgap` | all 20 arrive, by tick 300, every one by the gap; no two nearer than 0.76 | all 20 (also with the synthetic tank's physics given to the Striker, a blueprint hook not kept); 15 by the gap and 5 round the wall's end, two within 0.39 (main: 16 and 4, 0.29) | All arrive, as the original's: `data.crowdgap-test` holds it. The detours and the near passes are a gap of the engine's crowd, on main as well. |

**`guardassist/`** asks, tick by tick, how an engineer guarding a T1 land factory helps it build. It runs `economy`'s stalled setup (the store emptied, and a Monkeylord's upkeep beside it, so the factory crawls and the help can be seen) and two scenes in turn, each cleaned up after:
- **far**: the factory at (300, 790) building two Strikers, and the engineer ordered to guard it from (306, 808), 18 south of it and out of reach, so it walks up;
- **near**: a factory at (340, 790) and the engineer already in reach of its skirt, at (332, 790): west of it, off the side its units roll out of.

Every tick from the order, t=0 to 330, it logs a row for each scene:
- the engineer's position, its distance to the factory's skirt (`gap`, a rectangle from the factory's blueprint) and to the unit under construction (`ugap`);
- its unit states (`Moving`, `Guarding`, `Building`, `Repairing` and others, joined by `+`, or `idle`);
- its focus and the factory's, named `<blueprint>#<k>` in the order the probe first sees them, because entity ids differ between the games;
- its and the factory's work progress, and the army's energy and mass requested.

It also logs the engineer's `OnStartBuild`, `OnStopBuild`, `OnStartRepair` and `OnStopRepair`, as they run, wrapped on that engineer alone (only those the scripts define). Its `t` is the loop index of a `WaitTicks(1)` loop, since the original's sim has no `GetGameTick`. It is done at `PROBE assist done`.

Asked on 2026-10-22: the original twice, the two runs matching; the engine on main c9055933 and with the guard's walk to its build place.

| Probe | The original | The engine | So |
|---|---|---|---|
| `guardassist` far, the walk | moves from tick 1; its move ends on tick 82, entering the cell (306, 796) it coasts out of to (306.47, 796.73); Guarding from then | moved to (303.3, 801.9), 14 from the factory's centre, where its reach met the skirt; now its move ends on tick 78 at (305.50, 796.54) | Moho's guard hands a factory's build to a repair task, which sends the engineer to the cell nearest the factory clear of its skirt grown by a cell (`PrepareMove(1, site, skirt)`) and begins when that move ends. |
| `guardassist` far, the help | `OnStartBuild` on tick 89, focus from tick 90, never dropped to tick 330 | `OnStartBuild` on tick 41, then 101 stops and 101 more starts, the focus gone every second tick; now one start, on tick 82, focus from 83, held to 330 | From its place the unit under construction is within twice the engineer's reach (`ugap` 9.10 in the original). |
| `guardassist` near | stands 4.0 from the skirt, within its `MaxBuildDistance`, and still walks 2.3 to the cell (334, 790); its move ends on tick 20; `OnStartBuild` on tick 26, focus from 27 | never moved; `OnStartBuild` on tick 7; now its move ends on tick 20, `OnStartBuild` on tick 28, focus from 29 | The walk comes first within reach as well. The place is clear of the skirt only with its centre snapped to the footprint's cells, (340.5, 790.5) for the factory spawned at (340, 790). |
| `guardassist` still apart | `idle` on tick 0, `Moving+Guarding` from tick 1; `GuardBusy` from the tick after the move ends; `OnStartBuild` 7 (far) and 6 (near) ticks after it | `Guarding` on tick 0, no `GuardBusy`; 4 (far) and 8 (near) ticks after the move | The engine derives `Guarding` from the order queue for every order, `GuardBusy` it does not have, and its builder arm aims for a time of its own; the cell where a tie in distance goes (306 against 305, 790 against 789) is the higher in the original and the lower here. |
| `guardassist` factory progress | 0.003 on tick 3, 0.007 on tick 10, 0.018 on tick 30 | 0.001, 0.004, 0.015 | After the first tick both add 0.0005 a tick (the stalled economy); the original's first tick adds about 0.003 more. `economy` shows it in its stalled scene, 0.017 against 0.014 on tick 30, not in its alone scene. |

**`buildbots/`** asks, tick by tick, what Moho does with a helper whose `BuildRate` is 0: the Cybran build bot, `ura0001`, which a Cybran engineer's scripts spawn and order to guard the unit it builds (`SpawnBuildBots`). Resources are ample, and eight scenes run in turn at (300, 790), each cleaned up after:
- **cybran**: a Cybran T1 engineer builds a power generator; its scripts spawn the bot;
- **frame**: a UEF T1 engineer builds a power generator, and a bot the probe made is ordered to guard the frame once it stands;
- **builder**: the same, the bot ordered to guard the engineer;
- **control**: the same, a UEF T1 engineer in the bot's place;
- **repair**, **reclaim**: a bot ordered to repair a power generator at half health, or to reclaim one;
- **guarded**: a bot ordered to guard a power generator at half health;
- **silo**: a bot ordered to guard a tactical missile launcher building a missile.

Every tick from the order (t=0 to 360 for the builds, 120 for the others) it logs the builder's state and progress, the work's fraction and health, and the first bot's state, focus, progress and position. It logs the bots' `OnStartBuild` (with its order), `OnStopBuild`, `OnStartReclaim`, `OnStopReclaim` and their build effects as they run, wrapped on the scene's own bots alone, since the other armies' Cybran builders spawn bots too. It is done at `PROBE bots done`.

Asked on 2026-10-23: the original twice, the two runs matching; the engine on this branch's base and with a helper's build rate setting its pace only.

| Probe | The original | The engine | So |
|---|---|---|---|
| `buildbots` frame, builder, cybran | `OnStartBuild(frame, 'Repair')` 7, 13 and 6 ticks after the order or the bot's spawn; `Guarding+Repairing+GuardBusy`, focus on the frame, progress the frame's; `OnStopBuild` at completion (frame, builder) | no callback: the guard only followed; now `OnStartBuild` 1, 7 and 0 ticks after, held to completion, then `OnStopBuild` (frame, builder) | A helper's build rate sets its pace, not whether it helps. The frame grows at the engineer's rate alone in both games, done on tick 258. |
| `buildbots` repair, guarded, silo | `OnStartBuild(unit, 'Repair')` on tick 1 (repair) and 7 (guards), `Repairing`, held to tick 120; health unchanged, the missile at the launcher's own pace (0.0033 a tick) | refused; now on tick 1 for all three, held, adding nothing | The same rule for a repair and for a silo's missile. |
| `buildbots` still apart | the bot helping the engineer names its order `Repair`; the control engineer `MobileBuild`. The bot's guard of the frame ends at completion (`idle`); its guard of the engineer goes on. The bot in `cybran` hears `OnStopBuild` before its builder's scripts destroy it. A repairing bot's progress is the unit's health (0.500). A zero-rate reclaim starts (`OnStartReclaim`, `Reclaiming`) and takes nothing. | `MobileBuild` for both; both guards go on; no `OnStopBuild`, the bot destroyed first; progress 0; the reclaim refused | Left for later, with the guard's start latency (`guardassist`). The Cybran engineer reclaims a tree on its site before it builds (ticks 3 to 39); the engine builds at once. |

**`stallstart/`** asks, tick by tick, what a factory's first build ticks get from a starved economy. A T1 land factory is given one Striker, four times in turn, each factory cleaned up after:
- **ml**: `economy`'s stalled setup, the energy store emptied and a Monkeylord's upkeep (250 a second, energy only) starving the army before the order;
- **midbuild**: the same upkeep, the store set to 300 at the order, so the stall begins while the factory builds;
- **dual**: the energy store emptied and two other factories already building, so only requests that want both resources share the stall;
- **solo**: energy ample and the mass store emptied, the factory the only consumer.

Every tick from the order, t=0 to 40, it logs the factory's work progress, its unit's fraction complete, its `GetResourceConsumed` and consumption (energy/mass a second), the other consumer's `GetResourceConsumed` (the Monkeylord, the first helper, or -1), and the army's energy and mass (stored / income / requested / usage), to six places. Its `t` is the loop index of a `WaitTicks(1)` loop. It is done at `PROBE stall done`.

Asked on 2026-10-23: the original twice, the two runs matching; the engine on main c9055933 and with Moho's `Materialize` for every build step.

| Probe | The original | The engine | So |
|---|---|---|---|
| `stallstart` economy | every grant, request, usage and `GetResourceConsumed` as the engine's to six places | the same | The economy serves the requests as Moho's (`ProcessArmyEconomy`); only the progress differed. |
| `stallstart` ml | the first build tick adds 0.003333 (1/300: the Striker's first hit point of 300), then 0.000531 a tick (the 7.4% grant) | 0.000531 from the first tick; now the same | Moho's `Unit::Materialize` brings a unit under construction level with its health before each step adds to both, and the unit starts at 1 hit point. A step smaller than 1/MaxHealth first lifts the fraction to the health's share; from then the fraction follows the health a step behind. |
| `stallstart` midbuild | full rate to tick 13, 0.003334 on the stall's first tick, then the 19% grant's step a tick late, then 7.4% | 0.001355 on the stall's first tick; now the same | The same rule. |
| `stallstart` dual, solo | dual: 0.003333 on the first tick (a 35% grant). Solo: its first tick at a 50% grant adds 0.003571, above the lead, and its second 0.003334 (25%) | 0.002506; 0.001786 on solo's second; now the same | The same rule. `economy`'s stalled scene follows: 0.017 against 0.014 on tick 30 before, now the same. |
| `stallstart` store | the army's mass store a hundredth lower on 18 rows (651.90 against 651.91 from the 21st tick of `ml`); on the stall's edge (`midbuild` ticks 13 and 14) the energy used 25.360226 and the grant 0.189592 | 25.360224 and 0.189591; now the same | Moho keeps the store in 32-bit floats, and takes each grant off it in turn; the engine now does both. |
