# Moho's blueprint write-back: reflected fields in the scripts' tables

Design, 2026-10-23. When Moho registers a unit blueprint it builds its typed copy from the Lua table,
derives what the class derives, and then merges every reflected field of that copy back into the same
table. So once registered, a field the `.bp` leaves out reads in every script as its class
constructor's default. The engine used to fill a few dozen such fields one at a time, wherever a script
had been seen to fail on one. It now fills them from one table, `src/blueprints/moho_reflected_fields.cpp`,
cited field by field to faf-re.

This corrects section 3 of `2026-10-20-moho-blueprint-defaults-design.md`. That section says the
scripts' tables keep the `.bp`'s fields as given and Moho's derived values live only in its C++ copy.
They don't: the copy is merged back into the table.

Path prefixes:

- **FR:** faf-re's `src/sdk/moho/` (the decompiled FAF build of Moho)
- **EN:** this repository's `src/`

Certainty tags: **[read]** read from faf-re code; **[probe]** measured on retail or FAF data;
**[inferred]** concluded from the code, not observed.

---

## 1. Moho's rule

1. **Registration merges the typed copy back** **[read]**:
   - FR `sim/Sim.cpp` `RegisterUnitBlueprintFromState` (0x00528B90) calls `InitUnitBlueprintFromLua`.
   - That function, like `RBlueprint::InitBlueprint` (0x0050DF10), runs `SCR_LuaBuildObject` over the
     table, then the class's `OnInitBlueprint`, then `SCR_RObjectToLuaMerge` (0x004CF0B0) from the copy
     back into the table.
   - The merge happens inside the `RegisterUnitBlueprint` call, so scripts see it as soon as that call
     returns.
2. **The merge writes reflected fields only** **[read]** (FR `lua/CScrLuaObjectFactory.cpp`):
   - A number or bool is written as a number or bool.
   - A struct or a vector becomes a table, made when the destination isn't one already.
   - Anything else is written as its lexical form: an enum as its registered name (`RULEUMT_None`), a
     resource id as its normalized string.
   - Keys the class doesn't reflect stay as the `.bp` gave them. A weapon's rack timings are one
     example: `RUnitBlueprintWeapon` has no rack fields.
3. **Defaults come from the class constructors** **[read]**:

   | Class | Address |
   |---|---|
   | `RBlueprint` | 0x0050DD60 |
   | `REntityBlueprint` | 0x00511C30 |
   | `RUnitBlueprint` (Display, Transport, Defense, Intel) | 0x0051E480 |
   | `RUnitBlueprintGeneral` | 0x0051EE10 |
   | `RUnitBlueprintAir` | 0x0051EF00 |
   | `RUnitBlueprintPhysics` | 0x0051F090 |
   | `RUnitBlueprintWeapon` | 0x0051F4C0 |
   | `RUnitBlueprintAI` | 0x0051F7D0 |
   | `RUnitBlueprintEconomy` | 0x0051F990 |
   | `RProjectileBlueprintPhysics` | 0x0051B650 |

4. **Some fields are derived** **[read]**:
   - `REntityBlueprint::OnInitBlueprint` (0x00512060):
     - An unsized footprint axis is the unit's size rounded up, at least 1.
     - A zero inertia tensor is a solid box's tensor for its size.
   - `RUnitBlueprintPhysics::ComputeDerivedQuantities` (0x0051F260):
     - A moving unit's negative `MaxSpeedReverse` becomes its `MaxSpeed`.
     - A zero `AttackElevation` becomes its `Elevation`.
     - A zero `CatchUpAcc` becomes the larger of `MaxAcceleration` and `MaxBrake`.
   - `RUnitBlueprint::OnInitBlueprint` (0x0051EA40):
     - The `RULEUMT_Air` motion type sets `CanFly`.
     - A flyer's zero `MaxAirspeed` becomes `MaxSpeed`, and a zero `MinAirspeed` becomes `MaxAirspeed`.
     - A zero `StartTurnDistance` becomes `SizeZ` x 3.
     - An unnamed `IconName` becomes the blueprint's id.
5. **Resource ids are normalized** **[read]**: `RResIdType::SetLexical` (0x004A9430, via
   `gpg::STR_NormalizeFilenameLowerSlash`) lowercases ASCII and turns `\` into `/`.

## 2. What the engine does

- **`blueprints/moho_reflected_fields.{hpp,cpp}`:**
  - Three tables of reflected fields (unit, weapon entry, projectile physics). Each row holds the
    field's section path, type, constructor default and the address that sets it.
  - A fourth table lists the reflected unit fields left unfilled, each with its reason (section 4).
  - `apply_unit_reflection` fills only what the table leaves out, then applies rule 4's derivations.
    It lowercases the resource ids:
    - `General.UpgradesTo`, `UpgradesFrom`, `UpgradesFromBase` and `SeedUnit`;
    - `Display.IconName` and `StrategicIconName`;
    - each weapon's `ProjectileId`.
  - It keeps the engine's existing rule that an empty `UpgradesFrom` or `UpgradesFromBase` reads
    `'none'`.
- **`blueprints/blueprint_store.cpp` `register_blueprint`** applies the layer to units and
  projectiles. That is the one path every `RegisterUnitBlueprint` and `RegisterProjectileBlueprint`
  takes. The UI state's copy is made from the filled table (`copy_lua_tables`), so both Lua states see
  the same fields.
- **`lua/blueprint_bindings.cpp` `l_RegisterUnitBlueprint`** no longer fills anything of its own.

### Fills removed because Moho doesn't reflect them

| Field | Why it was filled | Moho | Retail data **[probe]** |
|---|---|---|---|
| Weapon `RackRecoilDistance`, `RackReloadTimeout`, `RackSalvoChargeTime`, `RackSalvoReloadTime`, `WeaponRepackTimeout` (0) | M200b assumed every numeric weapon field reads 0 | not in `RUnitBlueprintWeapon`: nil when the `.bp` leaves it out | Of the weapons with rack bones, these leave them out: death weapons, the Ahwassa's four autocannons (`RackReloadTimeout`), the Loyalist's EMP. The firing states that read them unguarded never run for death weapons; the rest behave as on Moho. |
| `Display.MovementEffects` ({}) | `Unit.lua` indexes it | not reflected | LuaPlus indexes nil without an error |

Every other value the binding used to fill is in the table with the same default:
- the threat levels, `RegenRate`, `Defense.Shield`;
- the intel radii, `CapCost`, the motion types and the drive numbers;
- `MaxSpeedReverse`, `Air.TurnSpeed`;
- a weapon's `Damage`, `DamageRadius`, `RateOfFire` and `Label`.

## 3. The C++ readers

The engine's own readers (`lua/unit_blueprint.cpp`, `sim/weapon.hpp`, `lua/bindings/sim/aibrain.cpp`,
`ui/range_overlays.cpp` and others) take the same defaults when a field is absent, so a filled
default changes nothing they compute. Two cases needed a closer look:

- The derived airspeeds, `StartTurnDistance` and `AttackElevation` are what `read_air` already derives
  from an absent or zero field.
- `MaxSpeedReverse` is what `read_drive` reads. The binding filled the same value before.
- Two weapon defaults hold a different number for the same rule:
  - `MaxHeightDiff` is +inf (`CWeaponAttributes`' `gpg::pInf`), where `sim::Weapon` holds 0 for
    "no limit". `can_target` compares a height difference with it only when it is above 0, and no
    difference is above +inf. It isn't in the sim checksum.
  - `EffectiveRadius` is -1. The range overlay takes `MaxRadius` for one that isn't positive, as it
    did for an absent one.

## 4. Reflected fields left unfilled

Each is listed, with its reason, in `unit_unfilled_fields()`. A unit test checks that each stays nil
on a bare blueprint and is not also in the filled table.

*Done (2026-10-30): all of them are filled now, and `unit_unfilled_fields()` is gone. `CollisionShape`
reads `"Box"`; `BackUpDistance` is derived as `ComputeDerivedQuantities` derives it, and the drive
reader no longer reads the two retail blueprints' `BackupDistance` spelling, which Moho doesn't
reflect (the Brick backs up within 2.7, not 5); a unit's mesh id is completed against `Source`. What
reads differently [probe `bp-typed`]: the Czar's Miscellaneous ring is 300 (`StagingPlatformScanRadius`
before `GuardScanRadius`, `MiscellaneousExtractor`), the 15 retail units without a selection size get
1, and on FAF's data 37 `spawn_dummy_*` aircraft fly at 0 and its Cybran build bots' life bars are
0.1 high. Every retail and FAF ground and naval unit gives its `TurnRadius`, every aircraft and hover
its `Elevation`; units without a size are only in unit tests, which now give the sizes they need.*

- **`General.CommandCaps`, `General.ToggleCaps`, `Physics.BuildOnLayerCaps`** (flag enums), and
  **`CollisionShape`**:
  - Moho writes back the registered name, or a number for several flags combined.
  - The engine's readers parse the `.bp`'s tables (`read_command_caps`, aibrain's `BuildOnLayerCaps`,
    `range_overlays`'s `ToggleCaps`, `build_placement`). This is stage 4b (section 6).
  - *Done (2026-10-29) for the three flag enums (section 6); `CollisionShape` stays unfilled.*
- **`Footprint.OccupancyCaps`, `Footprint.Flags`, `AltFootprint`:** derived from the motion type's
  footprint class; nothing reads them. *Done (2026-10-29) for `OccupancyCaps` and `Flags`, and
  (2026-10-30) for `AltFootprint` (section 5).*
- **`Display.MeshBlueprint`:** Moho completes it from `Source`; the mesh cache resolves an absent one
  itself.
- **`SizeX`, `SizeY`, `SizeZ`** (Moho's 1):
  - Every retail and FAF unit gives them **[probe]**: none of FAF's 606 unit `.bp` files leaves one out.
  - `blueprint_collision_shape` reads an absent size as no collision box. The unit tests' bare
    blueprints rely on that: with a 1x1x1 box, one tank of `test_moho_pathing`'s formation of nine is
    blocked a cell short of its slot.
- **`Physics.TurnRadius`** (Moho's 5): the navigator reads an absent one as 0, which means no turning
  circle. A filled 5 would change turns the probes haven't measured. See
  `2026-10-20-moho-steering-speed-cap-design.md`.
- **`Physics.Elevation`** (Moho's 0): the engine flies an aircraft without one at 18
  (`unit.hpp` `elevation_target_`).
- **`Physics.BackUpDistance`:**
  - `read_drive` derives an absent one as `SizeZ` x 3, which is Moho's derived value.
  - It also honours two retail blueprints' `BackupDistance` spelling, which a filled value would hide.
- **`AI.GuardScanRadius`, `AI.StagingPlatformScanRadius`:**
  - The Miscellaneous range overlay draws no ring for an absent one.
  - The sim's readers already default to Moho's 25 and 300.
- **`LifeBarHeight`** (Moho's 0.1): the life bar reads an absent one as 0, and no script reads it.
- **`SelectionSizeX`, `SelectionSizeZ`:** the selection box sizes an absent one by the mesh.

The projectile layer fills only `Physics`'s twelve `*Range` spreads, as the engine did before.
`Projectile::apply_blueprint_physics` would read a filled `MaxSpeed` of 0 over its preset state.

## 5. Left as the `.bp` gives it

These parts of the write-back aren't ported:

- **Present values in a type the scripts can tell apart.**
  - Moho writes a bool back as a bool: a `.bp`'s `ManualFire = 0` reads `false` afterwards.
  - A string that Moho reads as a number reads back as a number.
- **A mobile unit's footprint class.**
  - `ComputeDerivedQuantities` replaces a mobile unit's footprint with its class, and its skirt is at
    least that class (`2026-10-20-moho-blueprint-defaults-design.md` rules 1 and 2).
  - The engine fills an absent footprint from the unit's size and an absent skirt from that footprint,
    as before. It doesn't clamp present skirts (`read_shape` does).
  - *Done (2026-10-29): `derive_physics` resolves the footprint as `ComputeDerivedQuantities` does,
    from the classes `footprints.lua` specs before the blueprints load. A mobile unit with a ground
    motion type takes its class whole; every unit gets its `OccupancyCaps` (the motion type's, or a
    structure's `BuildOnLayerCaps`) and `Flags`. Every skirt is then at least the footprint and never
    offset outward. Retail's `Unit:GetFootPrintSize` (`sim/Unit.lua`) reads the table's footprint
    (Seraphim build effects, `BlockFormation`), and under FAF formations size a group by
    `max(Footprint.SizeX, SizeZ)` (`categorizeUnits.lua`): a Striker is 1, not 2, and a Valiant
    destroyer 4, not 8.*
  - *Done (2026-10-30): `AltFootprint` too. Its unsized axes are the unit's size rounded up, as the
    footprint's (`REntityBlueprint::OnInitBlueprint`). A mobile unit with an `AltMotionType` gives it
    that motion type's caps and, with a ground footprint, the class nearest its own size, else the
    main footprint; one without takes the main footprint whole. A structure's stays as it is. The sim
    resolves a unit's alt footprint from it too (`resolve_unit_footprints`' `own_alt`).*
- **`MotionType` for a unit that doesn't move.** Moho sets it to `RULEUMT_None` when `MaxSpeed` is 0.
  That is a sim-visible change to present values.
  - *Done (2026-10-30): `derive_physics` does. No retail or FAF unit has a motion type without a
    top speed.*

## 6. Stage 4b: enums as their names

To write back `CommandCaps`, `ToggleCaps` and `BuildOnLayerCaps` as Moho does, the C++ readers first
have to read typed values instead of the `.bp`'s tables:

- `lua/unit_blueprint.cpp` `read_command_caps`;
- `lua/bindings/sim/aibrain.cpp` (`BuildOnLayerCaps`);
- `ui/range_overlays.cpp` (`ToggleCaps`);
- `sim/build_placement.*`.

FAF also copies `CommandCaps` into `CommandCapsHash` before registration. That work is a change of its
own.

*Done (2026-10-29):* `blueprints/flag_enums` reads a flag enum as `SCR_LuaBuildObject` does: a table's
keys whose values are true, or a string of names and numbers joined by `|` (`REnumType::SetLexical`;
a number or a boolean as its string). It writes the value back as `REnumType::GetLexical` does: the
name of the one entry that equals it, else the number (retail probe: `"575"`, `"Land"`, `"3"`, `"0"`).
Registration writes all three so; a value Moho can't read keeps the constructor's (`0`, `0`, `Land`).
The three readers take either form, and `build_placement` gets its layers from aibrain's.
FAF's unit view takes the abilities a unit's `CommandCaps` and `BuildOnLayerCaps` give from those
numbers (`unitviewDetail.lua`, `DecimalToBinary`); with the `.bp`'s table it found none.

## 7. Tests

- **`tests/test_moho_reflected_fields.cpp`:**
  - constructor defaults for a bare unit and for weapon entries;
  - every derivation in rule 4;
  - resource ids lowercased and slashed;
  - rack timings and `MovementEffects` left nil;
  - the unfilled fields nil and disjoint from the filled ones;
  - projectile spreads;
  - both Lua states;
  - the flag enums written back from tables, strings, numbers and booleans, one flag by its name;
  - a mobile unit's footprint class, a structure's caps and the skirts;
  - the engine's readers on a registered blueprint: its orders and toggles, its placement layers,
    the range overlay's toggles.
- **`--bp-reflect-test`** (`data.bp-reflect-test` on retail, `data.bp-reflect_faf` on
  FAF): on retail's data both Lua states read the retail probe's values; on FAF's, its unit view's
  abilities agree with each `.bp`'s `CommandCapsHash`, and `CategorizeUnits` sizes tanks, a Fatboy
  and destroyers by class.
- **`tests/test_blueprint_defaults.cpp`** passes unchanged.
- **`--unitview-sweep-test`** (`data.unitview-sweep-test` on retail, `data.unitview-sweep_faf` on FAF):
  - It selects the commander with `fixtures/armament_detail.prefs`, so the armament detail is on.
  - For every unit blueprint it shows the unit view's detail and enters and leaves the build button's
    rollover. Each call is protected.
  - Before this change **[probe]**:

    | Data | Units swept | Failed | Why |
    |---|---|---|---|
    | FAF | 675 | 219 | nil `MinRadius` and `Description`, an unset `Economy.MaxBuildDistance` |
    | Retail | 568 | 17 | nil `Economy.BuildTime`, one nil `Description` |

  - After: 0 failed on both.
- **`tools/moho_probes/blueprints/bp_diff.py`** takes the table file as an optional third argument. It
  tags each field a `.bp` leaves out as filled, unfilled or neither. A filled field still missing from
  the engine's side is a bug in the layer, and the script then exits 1. It hasn't been run against a
  new beat-0 log for this change.
