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

**On the original game,** under Wine or Proton, from its `bin` folder:

```sh
wine SupremeCommander.exe /init 'Z:\<repo>\tools\moho_probes\init_probes.lua' \
    /map SCMP_009 /nomovie /log 'Z:\tmp\probes.log'
```

- This opens the game's window. Choose windowed mode in its options first if you like.
- Let it run about three minutes of game time, then read the `PROBE` lines in the log.
- `init_probes.lua` holds the game's folder twice, once as Linux sees it and once as Wine sees it. Edit both for another machine.

## The questions, and the engine's answers

The engine's answers below were measured on main 3be4d6a5 (2026-10-12), with the grid's pathing and with `--moho-pathing`; the FormAttack row also with #605.

| Probe | The question | The engine | What the original's answer decides |
|---|---|---|---|
| `startup-select` | Is the commander selected at the start? Retail's `OnFirstUpdate` selects it once, 3 s in, inside the warp's 2.1 s `SetUnSelectable` window. | `selected=0` (a windowed run) | If the original selects it, the engine's start timing (beat clock against UI clock) differs from Moho's. See the [3be4d6a5 record](../../docs/validation/2026-10-12-3be4d6a5.md). |
| `units-around-point` | How far does `brain:GetUnitsAroundPoint(cat, pos, 1, 'Ally')` reach for a T1 power generator? Retail's `EngineerTryRepair` asks so before each build. | Found from d = 0 to 4, not from 5: the 4-unit collision cells of faf-re's `func_GetUnitsAroundPoint` | If the original stops at d = 1 or 2, the cell gather is wrong, and with it the halving of retail's AI since #591. |
| `shore` | At the strait at z 520, whose banks fall about 1.2 a cell, do a hover and an amphibious tank cross, or drive round by land? | Both drive round, arriving 1066–1186 ticks in, only ever on Land | If the original's cross, the engine's `MaxSlope` reading of `OCCUPY_MobileCheck` is too strict for them. |
| `choke` | 24 Strikers through a 4-wide gap in a wall: how many through the gap, through wall cells, stopped short? Does the Monkeylord walk over the wall? | Grid: 18 / 6 / 0, and the Monkeylord goes round (x 649). Moho pathing: 21 / 0 / 3, and over the wall (x 679). | Units stopping short follow faf-re's `CAiPathNavigator` as ported. If the original's all get through, the port misses a rule. |
| `formattack` | Six Strikers given `IssueFormAttack(..., 'AttackFormation', 0)` on a point, beside six given `IssueAttack`: how spread are they at ticks 60, 120 and 200? | Main: the same as the plain attack (spread 10.7 → 4.4). With #605: they hold a line 15 wide; the plain group closes to 4.4. | Whether Moho's FormAttack keeps its slots all the way in, as #605's does. |

The probes play the scenes at once, after the commanders' warp-in (tick 80). They make every other army neutral first, because the map's other commanders start near two of the scenes.
