# The FAF regression run

Status: three runs, 2026-09-25: the first, a second after the fixes, and an 18,000-tick third. It runs FAForever's current game Lua (`develop`) on the engine, which has been built against retail FA since it moved to Linux (M175+).

## Why

The engine was first built against FAForever's data, on Windows, and has run retail FA's since. FAF's game code has moved on: its Lua, its AI and its unit scripts call engine API that retail's never did, and in new ways. A run against FAF finds what retail can't:
- crashes on inputs retail never gives;
- missing bindings;
- engine answers of the wrong shape.

FAF is the game most players play.

## How to run it

No FAF client is needed. FAF's game code is the [FAForever/fa](https://github.com/FAForever/fa) repository. `tools/faf_regression.py` does the rest:
1. clones it (shallow, about 2.5 GB, into `~/.cache/osc-faf`);
2. writes an init file that mounts it over retail FA's art and sounds, as FAF's own `init_faf.lua` mounts its built packages: FAF first, then only the retail archives FAF allows (`allowedAssetsScd`);
3. plays a four-AI game;
4. tallies each distinct warning and error.

```
tools/faf_regression.py --engine build/linux-release/opensupcom \
    --fa-path "<Steam>/Supreme Commander Forged Alliance" --ticks 3000
```

## First run: what it found

Four AIs on SCMP_009, 3,000 ticks.

**The crash, fixed with this run's harness.** The engine segfaulted between ticks 1,000 and 2,000. FAF's AI calls `IssueClearCommands(scout)` with one unit; Moho takes a unit as well as a list. The engine walked the unit's own table as if it were a list. It found its `Brain` and `PlatoonHandle` fields, took their C++ pointers (`_c_object`) for units, and called through them.

A second hazard of the same kind was in blip tables. They kept a raw pointer to their unit, which dangles once the unit is freed, and AI scripts keep blips for ticks.

Now:
- a lone unit is a list of one;
- a blip names its unit by id: `unit:GetBlip`'s, and those the sim hands an AI's `OnIntelChange`;
- the handles `Issue*` and the sim's other list and target readers take (`extract_entity`) are checked against the entity registry (`EntityRegistry::holds`) before use. An entity stays held until its `OnDestroy` has run, so a script's handle to it still resolves there. A method called on a unit (`check_entity`) still trusts its own table, which unregistering nulls.

`--issue-handles-test` holds all three.

**Engine API gaps, after the crash fix.** The game ran its 3,000 ticks. The counts are Lua errors over the game.

| Count | Where FAF fails | Cause |
|---|---|---|
| 141 | `StructureUnit` OnCreate: `EulerToQuaternion(...) * self:GetOrientation()` | FAF multiplies quaternions through the engine's vector metatable, which it extends with `__mul` (`getmetatable(Vector2(0,0))`). The engine's quaternions carry no metatable. Also, `EulerToQuaternion` has x and z swapped against Moho's (faf-re `MathReflection.cpp`: `(roll, pitch, yaw)`). |
| 313 | factories: `RollOffPoint` nil, `BuildEffectBones` nil | Follows from the row above: they are set later in the `OnCreate` that failed. |
| 42 | `CommandUnit`: aim manipulator `SetHeadingPitch` | Missing method, with `GetHeadingPitch`. |
| 42 | `UEB1101` (power generator): slider `SetGoal` | Missing slide-manipulator methods. |
| 3 | `Prop.SetPropCollision`: `centery` nil | A caller passes fewer collision values than FAF expects. |
| 2 | `DefaultProjectileWeapon.GetWeaponRoF`: `RateOfFire` nil | To be traced. |
| 1 | `userInit.lua`: `OpenURL` | Missing UI global. |

`OrientFromDir` is also a stub that returns the identity; FAF and retail both use it. It is to be fixed with the quaternion work.

## Second run: what the rows turned out to be

Two of the first run's diagnoses were wrong, and both mistakes came from reading error messages at face value. FAF gives `nil` a metatable, so `nil:Method()` fails with "attempt to call method `X' (a nil value)" instead of "attempt to index a nil value". A "missing method" can therefore be a nil receiver. The way to tell is to probe the call site: mount a patched copy of the FAF file first in a copy of the harness's init, and `LOG` the receiver.

| Row | What it was | Fixed by |
|---|---|---|
| `EulerToQuaternion(...) * GetOrientation()` (141), and the factory fallout (313) | As diagnosed. Quaternions now carry the one vector metatable, `EulerToQuaternion` uses Moho's formula, and `OrientFromDir` builds `COORDS_Orient`'s frame. | #97 |
| Slider `SetGoal` (42) | Fallout of the row above: UEB1101 makes its sliders later in the `OnCreate` that failed. | #97 |
| `SetHeadingPitch` (42) | `self.rightGunLabel` was nil, so FAF looked up a weapon labelled nil. FAF's ACUs and SCUs set it in their class's `__init`, and the engine never ran a unit's `__init`. Moho makes an entity's object by calling its class (`CScriptObject::CreateLuaObject`). | #98 |
| `OpenURL` (1) | Missing UI global. It now opens a URL only when its scheme is in the init file's `protocols` list, as Moho's does, through the system's handler and never a shell. | #99 |
| `centery` (3) | FAF's `wreckage.lua` reads `bp.CollisionOffsetY` unguarded, and 398 of its 606 units omit it. Moho's blueprints carry `REntityBlueprint`'s default of 0. | #105 |
| `RateOfFire` (2) | The UEF T1 transport's guidance system has none. `RUnitBlueprintWeapon`'s default is 1. | #105 |

The fixes also closed gaps that retail shares with FAF:
- `CreateStorageManip`: every storage building's script errored there (#100).
- `IncreaseBuildCountInQueue`: the factory queue's left-click (#101).
- `Issue*` return values and `IsCommandDone`: factory roll-off (#102).
- Animators play at rate 1, and `WaitFor` on them returns. Before, every retail `WaitFor` on an animator nobody had set a rate for hung for ever (#103).

**The run on main after #100** (3,000 ticks, four AIs): the game reaches tick 3,000 with 244 units alive, and the only script errors left are `centery` ×2, `RateOfFire` ×2 and `IncreaseBuildCountInQueue` ×1. With #101 and #105 as well, **FAF's game plays its 3,000 ticks without a script error**. What the harness still reports are FAF's own blueprint-check warnings: "Overriding the x axis of collision box", and a missing preferences file in its `Blueprints.lua`.

## Third run: 18,000 ticks

A long game reaches the late game: T3 units and experimentals, with more than 1,600 units alive. Run on main plus the fixes still open (#103–#105 and #107–#110), merged locally.

| Count | Where FAF failed | What it was | Fixed by |
|---|---|---|---|
| 11,561 | `Unit.OnMotionHorzEventChange`: a nil weapon | The UEF T1 mobile AA's first weapon has no `Label`. FAF keys `WeaponInstances` by it, and the nil key stopped `Unit.OnCreate` at "table index is nil" (367×), leaving the table half built. Moho's `Label` string defaults to `""`. | #105 |
| 72 | `MobileUnit.DestroyAllTrashBags`: `Destroy` on nil | Fallout from the same `OnCreate` failure. | #105 |
| ~550 | `GetVelocity` on units, and FAF's cached `moho.unit_methods.GetVelocity` | Units had no `GetVelocity`. Moho's is per tick, and so is its projectile `GetVelocity`, where ours was per second: retail's Miasma shell and split missiles depend on that. | #108 |
| 5 | Cruise missiles: `Physics.MaxSpeedRange` nil | The projectile `*Range` defaults (`RProjectileBlueprintPhysics`). | #109 |
| 11 | Bombers: `UnitGetTargetEntity` nil | Units had no `GetTargetEntity`: the attack order's target, as Moho's attacker's desired target. | #110 |

With all of these in, the 18,000-tick game has **no script errors**. What remains:
- FAF's own blueprint-check warnings.
- Three "It was a unit!" warnings: FAF's shield collider is told of a collision with a unit, which it doesn't expect.
- About 635 AI warnings, "Invalid location - Large Expansion Area N". Builder conditions still ask about an expansion base after FAF's `DeadBaseMonitor` removed it. Whether FAF does the same on Moho, or our manager teardown leaves builders running, is not yet known.

## Fourth run: Sung Island, and the engine's own override

On 2026-09-26, main after M206r–M206t and the aircraft fixes (#120–#123) played 18,000 ticks on Sung Island (SCMP_010, four AIs, seed 7), an island map, and on SCMP_009.

| Count | Where FAF failed | What it was | Fixed by |
|---|---|---|---|
| 2 | `navutils.lua` `PathToWithThreatThreshold`: `originSection` nil | The engine's own Lua. Since March it had replaced FAF's `NavUtils.CanPathTo` with one that always says yes, from when FAF's navigation mesh (`NavGenerator`) didn't build on the engine. It builds now. So a Water path from a point on land passed the check, and FAF indexed the section it doesn't have. It also told every AI reachability check "yes". A probe of the call site showed the function's source was a `do_string` chunk, not FAF's file. | removing the override |
| 17 | `projectile.lua`: `SetNewTargetGroundXYZ` nil | FAF's engine adds the three-number form of `SetNewTargetGround`, and its `OnTrackTargetGround` calls it. | the binding |

With both, the Sung Island and SCMP_009 games have **no script errors** at 18,000 ticks. The AI's "Invalid location - (Large) Expansion Area" warnings remain: about 1,700 without the fix and 1,400 with it, depending on how each game goes.

## Next

- Longer and wider runs: 18,000 ticks, other maps and other seeds. The first 3,000 ticks cover the early game only: no experimentals, and little T3.
- `--fail-on-errors` in the harness, once #101 and #105 merge, so that a regression fails the run.
- FAF's UI states (its lobby and front end) are untested. The harness plays a skirmish from the command line.

Each fix must also leave retail's gate green: retail and FAF both run on the same engine.
