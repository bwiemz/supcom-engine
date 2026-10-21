# Moho's blueprint defaults, from retail's /synclog beat 0

Design, 2026-10-20. Retail FA's `/synclog <dir>`, with `sim_KeepAllLogFiles` on, writes a file a beat.
Beat 0's holds every blueprint as Moho filled it in: after its constructors, `OnInitBlueprint` and
`ComputeDerivedQuantities`, so with every default a `.bp` leaves out. Compared field by field with the
engine's blueprints for all 568 retail units, it showed six fields where the engine's value was not
Moho's. This note ports four of them; the footprint and skirt are left (section 5).

Path prefixes:

- **FR:** faf-re's `src/sdk/moho/` (Draiget/faf-re, the decompiled FAF build of Moho)
- **EN:** this repository's `src/`, as of 9f454781

Certainty tags: **[read]** read from faf-re code; **[probe]** read from retail's beat 0.

---

## 1. Moho's rules

1. **A mobile unit's footprint is its class's** (FR `resource/blueprints/RUnitBlueprint.cpp`
   `RUnitBlueprintPhysics::ComputeDerivedQuantities`, 0x0051F260) **[read]**. A unit with a ground
   motion type takes the nearest class of `footprints.lua` (`RRuleGameRules::FindFootprint`), and the
   class replaces `mFootprint` whole, its size included. Ranges, formations and steering priority
   (`IsHigherPriorityThan`) read that size.
2. **Its skirt is at least that footprint** (the same function) **[read]**:
   `SkirtSize = max(SkirtSize, footprint size)`, with `SkirtSize` 0 when the `.bp` leaves it out.
3. **`Physics.BackUpDistance` left out is three times the unit's length** (the same function; the
   constructor 0x0051F090 sets -1) **[read]**: `SizeZ * 3`.
4. **`DesiredShooterCap` left out is 3** (FR `entity/REntityBlueprint.cpp`, the constructor 0x00511C30)
   **[read]**: at most three weapons shoot at a projectile at once (FR `ai/CAiAttackerImpl.cpp`).
5. **A weapon's `FiringTolerance` left out is 0.01 degrees** (FR `resource/blueprints/RUnitBlueprint.cpp`,
   `RUnitBlueprintWeapon`'s constructor 0x0051F4C0) **[read]**.
6. **`LifeBarSize` left out is 1** (FR `entity/REntityBlueprint.cpp`, the constructor) **[read]**.

## 2. What retail holds

`tools/moho_probes/blueprints` **[probe]**: retail's beat 0, and the engine's blueprints as its scripts
see them, compared by `bp_diff.py`.

| Field | Retail (beat 0) | Engine at 9f454781 |
|---|---|---|
| Mobile unit footprint | its class's (134 of 134 ground-mobile units; almost all 1x1) | `ceil(SizeX/Z)`, e.g. 1x2 |
| Mobile unit skirt | at least the class's footprint (268 of 268 axes) | at least the old footprint |
| `BackUpDistance` left out | `SizeZ * 3` (495 of 495) | 0: never backs up |
| `DesiredShooterCap` left out | 3 (278 of 287 projectiles) | 0: no cap |
| `FiringTolerance` left out | 0.01 (37 of 37 weapons) | 0 |
| `LifeBarSize` left out | 1 (12 of 12) | 0 |

## 3. The engine

- **EN `lua/unit_blueprint.cpp` `read_drive`** reads an unset `BackUpDistance` as `SizeZ * 3` (rule 3).
- **EN `sim/projectile.hpp`**, **EN `sim/weapon.hpp`** and **EN `lua/unit_blueprint.cpp`
  `read_life_bar`** take Moho's defaults (rules 4 to 6).

The blueprint tables the scripts see are left as the `.bp` gives them: Moho's derived values live in
its C++ copy, which is what beat 0 prints, so `bp_diff.py` lists the footprint and skirt fields however
the engine's units hold them.

## 4. Tests

- `test_favors_water.cpp`: an unset `BackUpDistance` is three times the unit's length; an unset
  `LifeBarSize` is 1.
- `test_blueprint_defaults.cpp`: an unset `FiringTolerance` is 0.01 and `DesiredShooterCap` 3.

## 5. Left

- **The footprint and skirt (rules 1 and 2).** `resolve_footprints` already resolves the class, for the
  occupation grid and pathing, but `footprint_size_x/z` and the skirt keep the `.bp`'s size. Taking the
  class's (a Pillar 1x1, not 1x2) changes who yields in steering (`IsHigherPriorityThan` reads it), and
  with it `data.steer-test` test 3: a Pillar and an engineer head-on, where retail's engineer steps
  aside and its Pillar stops a moment. That is Moho's collision cone, which retail's `/synclog` shows
  faf-re's decompile reads the other way round; with it ported, one tank of `test_moho_pathing`'s crowd
  through a gap ends wedged at the wall's end. They go together in a change of their own.
- **`Physics.TurnRadius`** left out is 5 in retail (419 units) and 0 here. The engine's drive reads it
  for its speed cap (`docs/plans/2026-10-20-moho-steering-speed-cap-design.md`), but without Moho's
  spline a new default alone changes turns the probes have not measured.
