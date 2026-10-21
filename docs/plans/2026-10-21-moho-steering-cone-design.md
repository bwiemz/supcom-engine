# Moho's collision cone, its step aside, and a refused step

Design, 2026-10-21. Moho gives a mobile unit its footprint class's size, and its steering ranks units by
that size (`IsHigherPriorityThan`). Taking the class's size (a Pillar 1x1, not 1x2) changed
`data.steer-test` test 3, a Pillar and an engineer head-on, and showed that the engine's resolution of a
meeting was not Moho's: retail's engineer steps aside while its Pillar stops a moment, where the
engine's engineer stopped. This note ports the footprint size, Moho's collision cone and step aside, and
the motion's refused step, which the new step aside led into.

Path prefixes and sources:

- **FR:** faf-re's `src/sdk/moho/` (Draiget/faf-re, the decompiled FAF build of Moho)
- **EXE:** retail FA 3599's `SupremeCommander.exe` (Steam), read as machine code at the given address.
  FAF's build is laid out differently: faf-re's addresses are about 0x6AD0 lower here.
- **EN:** this repository's `src/`, as of 9f454781

Certainty tags: **[exe]** read from the retail binary; **[read]** read from faf-re code; **[probe]**
measured in retail FA with a Lua probe and its `/synclog`.

---

## 1. Moho's rules

1. **A mobile unit's footprint is its class's** (FR `resource/blueprints/RUnitBlueprint.cpp`
   `RUnitBlueprintPhysics::ComputeDerivedQuantities`) **[read]**, its skirt at least that; retail's
   `/synclog` beat 0 holds the class's size for all 134 ground-mobile units **[probe]**
   (`docs/plans/2026-10-20-moho-blueprint-defaults-design.md`).
2. **The cone** (EXE `ResolvePossibleCollision`, 0x0059DA00; FR 0x00596F30) **[exe]**: with `D` the unit
   from the other, normalized (current positions), and `Vc` the other's velocity, normalized, the unit is
   *within the cone* when `D . Vc > 0.707`: the other is coming at it. faf-re's decompile tests the
   other's heading against the way *to* it, the other way round. Retail's `/synclog` logs every
   resolution: all 11 in the probes below fit the binary's test, none faf-re's **[probe]**.
3. **Within the cone** **[exe]** the other is told to stop (`SetCol(5)`, unless both are in one
   formation, with faf-re's naval and footprint rules), and the unit makes for a point `sep` (the two
   blueprints' larger sizes and 0.5) from a base:
   - driving (some velocity) and facing the other (`D . forward < 0`), with `Vo` its velocity and `S`
     the other's side axis turned toward the unit:
     - going the other's way (`Vo . Vc > 0.707`): from itself, along `normalize(D + S)`;
     - meeting it (`Vo . Vc < -0.707`): **from the other**, along `S` -- beside the other, on its side;
     - else: from itself, along `normalize(Vo + D)`;
   - else: from itself, along `Vc` turned a right angle (half one in a formation), to the side the
     cross product of `D` and `Vc` gives (constant quaternions about +y built at start-up from
     -pi/4, pi/4, -pi/8, pi/8, read at EXE 0x00C0F260..0x00C0F3D3).
4. **The point is placed** **[exe]** by `func_TryBuildStructureAt` (EXE 0x00631CD0, FR 0x0062B200) with
   border 1 and no coercion: where the unit's footprint fits (`func_LocationIsFree`: map caps, then the
   occupation grid), snapped to the footprint's cells; else the first place that fits a cell at a time
   in square rings round it (900 tries); none when the point is within a cell of the playable area's
   edge. Without a place the unit is left on its way.
5. **Not within the cone** the unit stops (`SetCol(4)`) **[exe]**.
6. **A refused step** (FR `unit/CUnitMotion.cpp` `CalcMoveCommon`) **[read]**: a step onto ground the unit
   won't fit is undone, the unit put back where it was; the push (`AddImpulse`, top speed x 0.01) goes
   to its velocity, which its next spline node replaces. The path navigator counts a tick the unit's
   position doesn't change, and gives the move up after 30 (FR `ai/CAiPathNavigator.cpp`
   `UpdateCurrentPosition`) **[read]**.

## 2. What retail does

All four probes are in `tools/moho_probes`; their hooks keep the original's `/synclog` beat files.

- **`steer`** **[probe]**: the engine's test 3 on SCMP_009. Retail's engineer, the Pillar coming at it,
  keeps its speed and veers 3.44 (near) and 3.67 (far) off its line; the Pillar stops for a few ticks and
  keeps its line within 0.74 and 1.11. `/synclog`: `ResolvePossibleCollision(engineer, Pillar)`,
  `collide.`, `within cone.`
- **`cone`** **[probe]**: pairs meeting at several angles. Every resolution in the log fits rule 2
  (borderline crossings at 0.70 come out "not within"); rule 3's point, from the logged positions, lies
  where the unit then drives in 6 of 8 within-cone meetings (dot of the two ways 0.98 or more); the two
  others are a slow unit turning (on line 20 ticks on) and one backing, whose facing the log doesn't
  give. Overtaking is the path navigator's: the faster unit's target cell moves 4 aside on the order's
  third tick.
- **`corner`** **[probe]**: a Striker at a wall's end, ordered past it so that its line clips the wall,
  moves 0.02 by tick 9 and stands still to the fourth decimal; its order ends on tick 40, 31 still ticks on (rule 6).
- **`crowdgap`** **[probe]**: `test_moho_pathing`'s crowd through a gap, with Strikers: all 20 arrive in
  retail (by tick 300), every one by the gap, no two nearer than 0.76; in the engine all 20 arrive too,
  also with the synthetic tank's physics given to the Striker in a blueprint hook.

## 3. The engine

- **EN `lua/unit_blueprint.cpp` `resolve_footprints`** sets the footprint size and skirt from the class
  (rule 1).
- **EN `sim/steering.cpp` `resolve`** takes rules 2 to 5. One engine path stays: a unit ahead going its
  way (`D . Vc < -0.707`) is stepped round as before, since retail overtakes by its path navigator, which
  the engine's steering doesn't (a `shortcut:` until it does). `place` is rule 4.
- **EN `sim/navigator.cpp` `drive_moho`** no longer bounces a refused unit back 0.01 x its top speed; it
  stays (rule 6). The bounce (#622) let a wedged unit rock for ever: a step accepted, the next refused and
  bounced, so its position always changed and its navigator never gave up. `choke-test` gets 24 of 24
  across without it, as retail does.

## 4. Tests

- `data.steer-test` test 3: the engineer steps aside without stopping, the Pillar stops and keeps its
  line, both arrive (both pathings). It failed on the code before ("engineer held").
- `test_unit_blocking.cpp`: head-on, the unit the other comes at makes for a point beside the other on
  its own side, and the other stops. It fails on the code before (no one steps aside).
- `test_favors_water.cpp`: a mobile unit's footprint and skirt are its class's.
- `test_moho_pathing.cpp`: a tank wedged at a wall's end (the `corner` probe's place) stands still and
  gives its move up after 30 still ticks (tick 32; retail's on tick 40, its 0.02 first). It fails on the code before,
  where the bounce works the tank free and it drives on to its goal.
- `data.crowdgap-test` (gate, both pathings): the `crowdgap` probe's twenty Strikers all arrive, as in
  retail. It replaces `test_moho_pathing`'s synthetic crowd of twenty, whose "every one arrives" no
  retail run backed: with this change one of its tanks is pushed to the wall's end of the gap, where
  the navigator's straight run (by cells, as the binary's) clips the wall and, as retail's Striker
  there, it stops and gives up.

## 5. Left

- **A crowd through a gap** (`data.crowdgap-test`, logged): retail's twenty all cross by the gap, no two
  nearer than 0.76; with `--moho-pathing` the engine's send 5 round the wall's end and let two come
  within 0.39 (main: 4 and 0.29), and with the grid 2 and 0.13 (main the same).
- **Overtaking by the navigator** (the target cell moved aside), in place of the engine's steering
  sidestep for a unit ahead going its way.
- **Probe positions.** Moho bases a step aside on the unit's next spline node; the engine, without the
  spline, on its position.
- **The type-5 stop** is the engine's `hold` (brake at twice its brake, five ticks still); retail's Pillar
  stood two to three ticks.
