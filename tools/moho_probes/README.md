# Probes for comparing the engine with the original game

Some of the engine's rules come from faf-re's decompiled Moho, and a few of their consequences surprised us. Only the original game can say whether it behaves the same. These probes ask each question the same way of both: an init file mounts the hooks ahead of the game, the hooks build each scene on Seton's Clutch (SCMP_009), and every answer is a `PROBE ...` line in the log.

Nothing here patches the game's scripts. The hooks wrap two functions to start the scenes and to read the start's selection.

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
- The game never ends on its own. The probes are done when `PROBE choke monkeylord` is in the log, about three and a half minutes in. Then close the game: kill its process (`pgrep -a -i supremecommand`).
- It rewrites its shader caches in the prefix (`AppData/Local/Gas Powered Games/.../cache`). To leave the prefix as it was, copy that folder aside first and back after.
- `init_probes.lua` holds the game's folder twice, once as Linux sees it and once as Wine sees it. Edit both for another machine.

## The questions, and the answers

The original game answered on 2026-10-12: retail FA (Steam) under Proton, run twice. The engine answered on main 3be4d6a5, with the grid's pathing and with `--moho-pathing`; the FormAttack row also with #605.

| Probe | The question | The original | The engine | So |
|---|---|---|---|---|
| `startup-select` | Is the commander selected at the start? Retail's `OnFirstUpdate` selects it once, 3 s in; the warp keeps it unselectable from tick 30 for 2.1 s. | Run 1: at UI tick 28, `selected=1`. Run 2: at tick 30, `selected=0`. | At tick 30, `selected=0` (a windowed run) | It depends on the frame times. Moho's beat clock takes the median of the last ten frame times (`CScApp::Main`, `RollingFrameRates`), so a slow frame loses its excess, and hitches at the start leave the UI's tick behind the wall clock. The engine's clock keeps up. |
| `units-around-point` | Which points does `brain:GetUnitsAroundPoint(cat, pos, r, 'Ally')` find a 2x2 T1 power generator from? Retail's `EngineerTryRepair` asks with r = 1 before each build. | From the centre strictly within r, across the map (height ignored), a circle: r = 1 finds it from 0.99 off but not from 1.0, nor from 0.75 off on both axes; r = 0 finds nothing. `GetNumUnitsAroundPoint` answers the same at every point. | From d = 0 to 4: the 4-unit collision cells of faf-re's `func_GetUnitsAroundPoint`, with no distance test | faf-re's decompile misses the distance test. The engine's cell gather is wrong, and with it the halving of retail's AI since #591. |
| `shore` | At the strait at z 520, whose banks fall about 1.2 a cell, do a hover and an amphibious tank cross, or drive round by land? | Both drive round, arriving at ticks 1196 and 1201, only ever on Land | Both drive round, arriving 1066–1186 ticks in, only ever on Land | The same: the engine's `MaxSlope` reading of `OCCUPY_MobileCheck` holds. |
| `choke` | 24 Strikers through a 4-wide gap in a wall: how many through the gap, through wall cells, stopped short? Does the Monkeylord walk over the wall? | 24 / 0 / 0, and the Monkeylord walks over the wall (x 678.25) | Grid: 18 / 6 / 0, and the Monkeylord goes round (x 649). Moho pathing: 21 / 0 / 3, and over the wall (x 679). | Moho pathing's port misses a rule that gets the last units through; the Monkeylord matches it. |
| `formattack` | Six Strikers given `IssueFormAttack(..., 'AttackFormation', 0)` on a point, beside six given `IssueAttack`: how spread are they at ticks 60, 120 and 200? | Formed: 15.0, 15.0, 13.4. Plain: 13.2, 10.4, 11.3, driving straight on side by side. | Main: both as a plain attack, closing to 4.4. With #605: formed holds 15; plain closes to 4.4. | #605's FormAttack matches. A group's plain attack on a point doesn't close up in the original: the engine's does. |

The probes play the scenes at once, after the commanders' warp-in (tick 80). They make every other army neutral first, because the map's other commanders start near two of the scenes.
