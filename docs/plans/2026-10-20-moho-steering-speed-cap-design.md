# Moho's steering speed cap, its coast and a builder's move off its site

Design, 2026-10-20. A ground unit's straight run already matched retail: acceleration, top speed and
turn rate to the hundredth. The start and the end of each move did not. A Striker set off 37 to 46
degrees short of its heading, where retail's pivots first; after a move it braked at `MaxBrake`, where
retail's damps its speed as well. This note ports the rules behind both from faf-re, with what they
touched on the way: a warp's leftover speed and a builder's move off its site.

Path prefixes:

- **FR:** faf-re's `src/sdk/moho/` (Draiget/faf-re, the decompiled FAF build of Moho)
- **EN:** this repository's `src/`, as of 9f454781

Certainty tags: **[read]** read from faf-re code; **[probe]** measured in retail FA with a Lua probe.

---

## 1. Moho's rules

1. **The speed cap** (FR `ai/CAiPathSpline.cpp` `ComputeSteeringSpeedCapFromParams`, 0x00699760)
   **[read]**. The spline's generator caps each node's speed from the heading before the node's turn:
   - with `Physics.RotateOnSpot`, while slower than its `RotateOnSpotThreshold` share of its top speed,
     a unit stands until its goal is within acos 0.98 (11.5 degrees) of its heading;
   - else it goes as fast as lets it turn onto its goal along the circle through it tangent to its
     heading (radius `distance / 2 sin error`), at half its `TurnRate`; on a circle at least its
     `TurnRadius` wide (`TurnRadius / turnMult`, none for 0), at that radius a tick.
2. **The coast** (FR `unit/CUnitMotion.cpp` `CalcMoveCommon`, the branch with no spline node) **[read]**:
   under 0.001 a tick a unit stops; else its velocity is damped to 0.8 and braked by up to `MaxBrake`
   (`MaxAcceleration` if none), so a slow one goes back a fifth of its speed before it stops.
3. **A warp** (FR `unit/CUnitMotion.cpp` `CUnitMotion::Warp`, 0x006B93D0) **[read]** advances the unit's
   coords twice: its last and current positions are both the new one, and it has no velocity left.
4. **A builder off its site** (FR `unit/tasks/CUnitMobileBuildTask.cpp`, its preparing state) **[read]**:
   out of reach of its site, standing on it, or where it won't fit, it goes to the place
   `Unit::PrepareMove(1, site, skirt)` finds from the site's centre, a cell at a time, whose cell keeps
   clear of the site's skirt grown by a cell on every side, touching counting (FR `unit/core/Unit.cpp`
   `IsPrepareMoveCandidateValid`, `ShouldTestPrepareMoveCellAgainstSkirt`).

## 2. What retail does

`tools/moho_probes/drive` **[probe]**: a Striker on SCMP_009 driven 40 north, 30 east and 30 back west,
logged every tick; retail's three runs gave identical logs, one with `/synclog`.

| Leg | Run | Turns from | Drives from | Heading error then | Done | At rest |
|---|---|---|---|---|---|---|
| north | retail | 4 | 23 | 1 deg | 143 | 300.47, 750.89 |
| | engine before, `--moho-pathing` | 4 | 18 | 46 deg | 139 | 300.47, 750.87 |
| | engine now, `--moho-pathing` | 4 | 23 | 1 deg | 144 | 300.47, 750.86 |
| east | retail | 4 | 13 | 1 deg | 105 | 330.07, 750.50 |
| | engine now, `--moho-pathing` | 4 | 13 | 1 deg | 105 | 330.04, 750.50 |
| west | retail | 4 | 23 | 1 deg | 114 | 300.98, 750.50 |
| | engine now, `--moho-pathing` | 4 | 23 | 1 deg | 114 | 300.97, 750.50 |

After a move retail's Striker coasts 2.04, 1.29, 0.69, 0.21 a second, then stands, which rule 2 gives.

`tools/moho_probes/buildreach` **[probe]**: an engineer 9 west of a point defence's site starts to build
3.47 from it, one 10 north of a power generator's 3.37: rule 4's cell, entered and stopped in.

## 3. The engine

- **EN `sim/navigator.cpp` `Navigator::drive`** asked for `top * cos(error)`, at least 30% of top for a
  unit that can't pivot, and pivoted a unit that can only past 45 degrees. It now takes rule 1, from
  the heading before the tick's turn; the old `PIVOT_ANGLE` and `TURNING_SPEED` go. Backing up, the
  cap reads the error from its tail, the way it goes: from its nose a goal just behind is 180 degrees
  off, which held a pivoting unit still (`tools/moho_probes/drivegrid`: Cybran, Aeon and Seraphim
  T1 mobile artillery stood with their orders until the navigator gave up; retail's get there).
- **EN `sim/unit.cpp` `Unit::coast`** braked at `MaxBrake` alone; it now takes rule 2, signed so the
  last fifth goes backward.
- **EN `sim/unit.cpp` `Unit::warp`** and the Teleport order's own jump kept the unit's speed, so it
  coasted on from the new place; both clear it (rule 3).
- **EN `sim/prepare_move.cpp`** takes `PrepareMove`'s search step and skirt rect, both optional.
  **EN `sim/unit_orders.cpp`**'s mobile build sends a builder by rule 4, where it had gone to a point of
  its own beside the skirt that `prepare_move` could then put back on the site: hemmed in by another
  skirt, braked by rule 2, it stopped on its site and its arm never reached the build.

## 4. Tests

- `data.drive-test` test 2 holds a pivoting Striker still till its goal is within 12 degrees; test 4,
  with Moho's pathing, checks its coast by rule 2. Both fail on the code before.
- `data.range-test` test 9: a builder out of reach builds from 2.9 to 4 (retail 3.47; it had been
  1.5 to 3). Test 12b, the commander on its own site, builds.
- `data.formation-test` test 5 measures a unit's pace over a second, not one tick's step: pivoting
  Strikers now crowd the Lobos behind them, whose own drive stays at 2.8 while pushes add to a step.
- `test_hull_facing.cpp` counts a hull as facing its target as `rotate_yaw_toward` does, within 0.81
  degrees; `test_cross_platform.cpp`'s checksum is this change's, from MSVC.

## 5. Left

- **The spline itself.** Moho moves a unit node to node along a spline it generates 20 ticks ahead,
  with start states (`PPS_4`/`PPS_5`/`PPS_6`: reversing and `BackUpDistance`, coasting to a stop), and
  its steering predicts collisions on both units' nodes (FR `ai/CAiSteeringImpl.cpp`
  `PredictCollisionForSteerings`). The engine drives tick by tick and predicts on `path_ahead`, a
  sampled polyline: two models of one path. Porting the spline as the one source would close both.
- **Without `--moho-pathing`** a unit still finds its path on the order's tick (turns from tick 2, not
  4) and stops on the exact point, not in the goal's cell.
- **`Physics.TurnRadius`** omitted is 5 in retail's blueprints and 0 here; rule 1 reads it.
