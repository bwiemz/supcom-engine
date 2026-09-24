# M201: projectiles and props through FA Lua

Status: design, 2026-09-23. This is the second milestone of Phase E (gameplay fidelity). It follows M200, where weapons fire through their scripts and projectiles became script instances.

## Why

M200 made weapons fire the way retail does. Their shots still land the way this engine alone decides:

- **Impact ignores the script.** A projectile deals its own damage in C++, so its script's `OnImpact` never runs. That means no impact effects, no sounds, no buffs, no `DamageFriendly`, and no damage-over-time.
- **Hits ignore height.** A hit is decided by horizontal distance, so shells pass through hills.
- **Artillery has no arc.** Every launch is flat.
- **Props are not script objects.**
  - Map props have no Lua object at all.
  - Every other prop is a bare engine table.
  - Retail's `CreateWreckageProp` errors on its first `Prop.lua` call.
  - A destroyed unit becomes its own wreck instead.

Now that units die in combat, every wreck hits these gaps.

## What the survey found

These findings come from reading the engine, retail Lua and retail blueprints.

### Projectiles

**Flight** (`projectile.cpp`):
- **What works:** Euler integration, blueprint gravity, acceleration up to `MaxSpeed`, homing at `TurnRate`, `StayUnderwater`, and orientation that follows velocity.
- **Stored but never used:** detonate heights, `DestroyOnWater`, `CollideSurface`, the collision flag, zig-zag, and angular velocity.

**Impact:**
- **A hit ignores height, terrain, water, shapes, shields and props.** It happens when the projectile comes within 1.5 of the target, or of its ground point, measured horizontally.
- **A timed-out projectile vanishes** without an impact.
- **Damage is applied in C++.** The engine calls `DamageArea` or `Damage` itself, always with friendly fire off.
- **The engine never calls** the script's `OnImpact`, `OnEnterWater`, `OnExitWater`, `OnLostTarget` or `OnCollisionCheck`.

**Launch** (`Weapon::launch`):
- The velocity is horizontal, toward the target, with no vertical component.
- `BallisticArc` is never read. 83 retail weapons are `LowArc` and 16 are `HighArc`.
- Lifetime is `d/v + 2`.
- Only seven `Physics` fields are read.

**Other bindings are wrong:**
- `entity:CreateProjectile` and `CreateProjectileAtBone` take their numbers as a velocity, where retail passes an offset and a direction.
- `CreateChildProjectile` ignores its blueprint, though retail's 26 calls all pass one.
- `SetScaleVelocity` scales movement. Retail uses it for draw scale, so an effect that shrinks a projectile reverses it instead.

### Retail's side

**`Projectile.OnImpact(type, target)`** runs the whole impact:
1. `DoDamage`, with `DamageArea` including `DamageFriendly` and `DamageSelf`, or damage over time.
2. `DoMetaImpact` and impact buffs.
3. A per-type sound.
4. Effects chosen by type from the terrain type.
5. The destroy, unless `DestroyOnImpact=false` or `ImpactTimeout` says otherwise.

**The types retail handles** are `Water`, `Underwater`, `UnitUnderwater`, `Unit`, `UnitAir`, `Terrain`, `Air`, `Projectile`, `ProjectileUnderwater`, `Prop` and `Shield`. Anything else logs an error.

**Blocker:** retail's `GetTerrainEffects` indexes `GetTerrainType(x, z).FXImpact[type]`. Our `GetTerrainType` returns only `{Name='Default'}`, so every `OnImpact` would throw. It must return the entries of retail's `/lua/TerrainTypes.lua`, which trees' uprooting needs as well.

### Props and wrecks

**How props get their Lua objects:**
- `CreateProp` and `CreatePropHPR` are near-duplicates. Their metatable is a `__prop_class` global that nothing sets, so they get the bare `moho.prop_methods`.
- Map props get no Lua object. `GetReclaimablesInRect` and `DamageArea` skip them, and reclaiming one pops it at once.

**The classes retail's 334 prop blueprints name:**

| Class | Blueprints | Where it comes from |
|---|---|---|
| `Tree` | 71 | `ScriptModule='/lua/proptree.lua'` |
| `TreeGroup` | 40 | `ScriptModule='/lua/proptree.lua'` |
| `Wreckage` | 1 (`DefaultWreckage`) | `/lua/wreckage.lua` |
| `Prop` | the rest | the default, `/lua/sim/Prop.lua` |

**The engine's own wreck path:**
- `entity_Destroy` on a unit with a death animation goes to `begin_dying`.
- `tick_dying` then marks the unit itself `is_wreckage`, and the unit stays registered for good.
- The renderer's wreck look keys off `is_wreckage`, which the C++ `SetMaxReclaimValues` sets. `Prop.lua` overrides that method.

**Reclaim's fields differ from retail's:**
- Ours reads `MaxMassReclaim`, `MaxEnergyReclaim` and `TimeReclaim` straight off the table.
- `Prop.lua` keeps `MassReclaim` and `ReclaimTimeMassMult`, has no `TimeReclaim`, and answers `GetReclaimCosts(reclaimer)`.
- Its `SetMaxReclaimValues` takes four arguments, where ours takes three.

**Missing prop methods:**
- `Kill`, which `Prop.OnDamage` calls.
- `FallDown` and its `Whack`, used by trees.
- `SinkAway`, `CreatePropAtBone`, and the global `SplitProp`.
- `AddBoundedProp`, `TryCopyPose` and `GetTerrainTypeOffset` are stubs.

### Shields

Projectiles and shields don't interact at all. `DamageArea` hits a shield like any other entity, with no absorption, and it also destroys nearby live projectiles; whether Moho does that is unverified.

## Slices

### M201a: props are script instances

- **One builder** serves `CreateProp`, `CreatePropHPR` and map props. Map props get their objects once the sim's Lua is up, before `BeginSession`.
- **Class resolution** follows `ScriptModule`/`ScriptClass` with lowercased ids, falling back to `/lua/sim/Prop.lua` `Prop`, the same pattern M200a used for projectiles. `OnCreate` runs.
- **Bindings:** `Kill` (at entity level), `CreatePropAtBone`, `SplitProp` and `SinkAway`, plus `FallDown`, which returns a motor whose `Whack` knocks the tree over.
- **Reclaim** reads `Prop.lua`'s fields through `GetReclaimCosts`, and `GetReclaimablesInRect` sees map props.
- **The wreck look** keys off the prop's class or the reclaim values the script set.
- **Tests:**
  - wreck-test Test 1 moves to the four-argument semantics.
  - reclaim-test gains assertions: it currently asserts nothing.
  - A new `--prop-test` checks classes, `OnCreate`, map-prop objects and tree falls.

### M201b: retail wrecks

- **Retail makes the wrecks.** `CreateWreckageProp` works end to end: a `Wreckage` prop with reclaim values, health and the wreck mesh.
- **The engine's unit-as-wreck path goes**, as victory's did in M189. The tests that assert it change deliberately: `test_death_anim` and enhance-wreck Test 9.

### M201c: OnImpact

- **Terrain types:** `GetTerrainType` returns `/lua/TerrainTypes.lua`'s entries.
- **Impacts go to the script.** With today's hit detection, the engine classifies each impact (`Unit`, `UnitAir`, `Terrain`, `Water`, `Underwater`) and calls `OnImpact`, and stops dealing damage itself. The engine-fired silo and OverCharge shots, which have no `DamageData`, keep the C++ damage.
- **The script's lifecycle is honoured:**
  - `DestroyOnImpact` and `ImpactTimeout`.
  - `OnEnterWater`/`OnExitWater`, which start torpedoes tracking.
  - `OnLostTarget`, `DestroyOnWater`, and the detonate heights.
- **Bindings fixed:** `SetScaleVelocity`, the `CreateProjectile` arguments, and `CreateChildProjectile`'s blueprint.

### M201d: collision

- **Swept tests:** each tick's path is swept against the terrain heightfield, the water plane, units (their box or sphere from `SizeX/Y/Z`, `CollisionOffset*` and `SizeSphere`), props, projectiles with health, and shields (sphere or box, `None` when down).
- **Both ways:** the engine calls `OnCollisionCheck` on both parties and honours `SetCollision`, `SetCollideSurface` and `SetCollideEntity`.

### M201e: ballistic arc

- **The solution:** `BallisticArc` Low or High, from `tanθ = (v² ∓ √(v⁴ − g(gd² + 2hv²))) / (gd)`.
- **Aim and launch:** the aim controller's pitch takes that angle, and the shot leaves with a real vertical velocity.
- **Lifetime** comes from the flight time or the blueprint's `Lifetime`.
- **`LeadTarget` and `MuzzleVelocityReduceDistance`** are honoured.
- **Proof:** artillery lands on its target.

### M201f: area damage and shields

- **`DamageArea`:** retail's falloff and rings, and a real direction vector (a tree's `'Force'` reads `direction[1]`).
- **Shields** absorb through `OnGetDamageAbsorption` and `ArtilleryShieldBlocks`, and spill over.
- **Open question:** whether area damage touches projectiles.

M201a and M201b stand apart from the projectile work. M201c needs the terrain types. M201e is largely independent.

## Risks

- **Damage moves to Lua.** Every hit must count once: `OnImpact` deals it, and the engine stops. Friendly fire appears for the 188 weapons with `DamageFriendly`, so AI outcomes, checksums and replays shift.
- **Thousands of map props.** Each map prop's `OnCreate` and TrashBag costs time at load; this is measured before and after.
- **Script errors in the gate.** Every AI mode counts them. Each slice runs long AI games and the combat integration tests, and uses `cross_os_replay.py --direct` for determinism across OSes.
- **Collision cost.** Sweeping every projectile each tick uses the spatial grid. It is profiled against the stress test.
