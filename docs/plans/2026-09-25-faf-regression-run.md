# The FAF regression run

Status: first run 2026-09-25. It runs FAForever's current game Lua (`develop`) on the engine, which has been built against retail FA since it moved to Linux (M175+).

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

## Next

Fix the rows above, one PR each, and rerun until FAF's game plays without Lua errors. Each fix must also leave retail's gate green: retail and FAF both run on the same engine.
