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

**What building M201a established:**

- **Warp-ins clear the ground, as in retail.** Once map props have
  objects, area damage reaches them. A commander's warp-in
  (`unitteleport01`) deals `Force` and `Fire` rings for several seconds, so
  tree groups near every start split into single trees, trees fall, and
  some burn. On Seton's Clutch that is about 3,400 new tree props at the
  start of a four-player game.
- **Prop blueprints need numeric defaults too.** 162 of retail's prop
  blueprints give `ReclaimEnergyMax = ''`, as the default wreck does, and
  `Prop.lua`'s `GetReclaimCosts` does arithmetic with it.
  `RegisterPropBlueprint` now reads such strings as numbers (`''` is 0),
  as Moho's typed blueprints do.
- **Moho asks the reclaimer.** `reclaimer:GetReclaimCosts(target)` returns
  the time in seconds, the energy and the mass. `Unit.lua` answers from a
  unit's build costs and passes a prop's question to `Prop.lua`, which
  applies its blueprint's time multipliers. We ignored those multipliers
  before.
- **Area damage has a direction:** from the blast to the target, level.
  Retail's trees fall along it. `OnDamage` errors used to be only logged;
  they now fail test modes too.
- **A default module beside a `.bp` is looked for before it is imported.**
  Most props have none, and `import` logs every miss.
- **Falling is simplified.** `FallDown`'s motor tips a tree over at once,
  away from the push. Moho animates the fall, which is presentation (Phase
  F).

### M201b: retail wrecks

- **Retail makes the wrecks.** `CreateWreckageProp` works end to end: a `Wreckage` prop with reclaim values, health and the wreck mesh.
- **The engine's unit-as-wreck path goes**, as victory's did in M189. The tests that assert it change deliberately: `test_death_anim` and enhance-wreck Test 9.

**What building M201b established:**

- **The wreck was the easy part; death was not the script's.** Once props
  were script objects, `CreateWreckageProp` needed only two things: mesh
  blueprints that resolve and `TryCopyPose`. The real gap was the death
  itself. `Kill` ran `OnKilled`, but the unit went on moving and firing
  until its death thread destroyed it, and `IsDead()` stayed false.
  Several deaths never reached the script at all:
  - `Destroy()` on a unit with a death animation made the unit its own
    wreck, registered forever.
  - Self-destruct, defeat and crash splash damage started the engine's
    own timed death.
  - Running out of fuel crashed aircraft, which retail never does: its
    `OnRunOutOfFuel` only slows them.
  - Half of all aircraft killed in flight (`DestroyNoFallRandomChance`)
    wait for Moho to drop them and call `OnImpact`, which we never did,
    so they would have hung in the air, dead.
- **Moho's `Kill` passes typed values.** It reads its arguments into C++
  before calling the script, so a bare `Kill()` reaches `OnKilled` as
  `(nil, 'Normal', 0)`. With a nil ratio, `CreateWreckageProp` computes
  `mass - mass * 1` and leaves a worthless wreck.
- **`SetMesh` takes mesh blueprints**: wreck, build and enhancement meshes,
  and a unit's own mesh after construction. None resolved before. They
  fell back to the entity's blueprint, or drew as cubes, and scripted
  units showed as green boxes. A mesh blueprint has no scale, so an
  override takes the entity's own blueprint's scale.
- **Every posed unit was skinned wrong, since M52.** The SCM reader
  transposed the inverse bind matrices. It showed first as a Mech
  Marine's death pose, and its wreck, exploding into wedges. The fix
  is rendering only: the sim never used these matrices.
- **Left:**
  - `AddBoundedProp` (Moho's cap on wreck count) is a no-op.
  - Wreckage draws with a burnt approximation of the Wreckage shader.
  - Retail effect meshes, now resolved, draw with the unit shader until
    Phase F gives them their own.

### M201c: OnImpact

- **Terrain types:** `GetTerrainType` returns `/lua/TerrainTypes.lua`'s entries.
- **Impacts go to the script.** With today's hit detection, the engine classifies each impact (`Unit`, `UnitAir`, `Terrain`, `Water`, `Underwater`) and calls `OnImpact`, and stops dealing damage itself. The engine-fired silo and OverCharge shots, which have no `DamageData`, keep the C++ damage.
- **The script's lifecycle is honoured:**
  - `DestroyOnImpact` and `ImpactTimeout`.
  - `OnEnterWater`/`OnExitWater`, which start torpedoes tracking.
  - `OnLostTarget`, `DestroyOnWater`, and the detonate heights.
- **Bindings fixed:** `SetScaleVelocity`, the `CreateProjectile` arguments, and `CreateChildProjectile`'s blueprint.

**What building M201c established:**

- **Terrain types were in every map and never read.** The SCMAP's
  terrain-type layer, a `TypeCode` per cell, was skipped. Seton's Clutch
  has eleven types, half of them water.
- **Once scripts handle impacts, their side effects arrive too.** Retail's
  impact code creates `VizMarker`s, and those needed intel on plain
  entities; UEA0107 needed `SetThrustingParam` on its thrust controllers.
  Scripts also make random draws of their own, so trees burn and fall
  differently, which changed the SCMP_009 goldens.
- **Flat shots can't burst on the way down.** Artillery with a
  `DetonateBelowHeight` bursts only after rising above that height, so
  until the arc (M201e) its flat shots fly on to the ground.
- **The creation bindings were wrong in ways that hid each other.**
  `CreateProjectileAtBone` took its arguments reversed. `CreateProjectile`
  read an offset as a velocity: the warp-in halo rose, where retail sets it
  1.35 above the commander.
- **Real battles found a desync.** With armies leaving their bases, a
  replayed game parted between Windows and Linux at its first scouting
  run: `GetThreatsAroundPosition` listed cells in hash-map order (fixed in
  #47). `--entity-trace` and `--rng-trace` found it, and stay as tools.

### M201d: collision

- **Swept tests:** each tick's path is swept against the terrain heightfield, the water plane, units (their box or sphere from `SizeX/Y/Z`, `CollisionOffset*` and `SizeSphere`), props, projectiles with health, and shields (sphere or box, `None` when down).
- **Both ways:** the engine calls `OnCollisionCheck` on both parties and honours `SetCollision`, `SetCollideSurface` and `SetCollideEntity`.

**What building M201d established:**

- **A unit's box stands on its offset.** The engine's shape for a unit or
  prop is a box `SizeX` by `SizeY` by `SizeZ` whose base is at
  `CollisionOffsetY`: retail's `GetRandomOffset` reads the height that way.
  Retail sets spheres itself: `AirUnit.OnStopBeingBuilt` for `SizeSphere`,
  and shields at half their `Size`.
- **The scripts decide who meets whom.** `Projectile.OnCollisionCheck`
  refuses its own army, so friends never block a shot. A tree group refuses
  and breaks into trees, so the shot flies on. A wreck refuses units but
  takes shots. A shield takes only enemy shots, and only coming in: units
  inside a dome fire out of it.
- **Shots now miss.** They used to hit anything within 1.5 of the target in
  plan view. Now a shot aimed where a unit was flies past it once the unit
  moves.
- **Firing randomness was 12 times too wide.** The engine turned the shot
  by up to `FiringRandomness` radians, 28 degrees for a typical 0.5. FAF
  measured Moho's spread as a circle `FiringRandomness × distance / 12`
  across (`FixedSpreadRadius`), and shots now land in it.
- **Weapons aim at the middle of a target**, not at its feet: a straight
  shot at a unit's base would graze the ground on the way.
- **Where shots end without a target:**
  - A shot whose time runs out impacts `'Air'` (`'Underwater'` below the
    surface), as FAF documents.
  - Strategic missiles have `CollideSurface = false` (they rise from their
    silos through the ground), so a tracking shot also ends on reaching its
    ground target.
  - Bombs fall at Moho's default gravity. None of retail's bomb blueprints
    has `UseGravity`, and they would otherwise hang where they were
    dropped.
- **Script-made projectiles take their blueprint's physics.** Debris and
  cluster bomblets fall, and effect projectiles stay put: their blueprints
  say `UseGravity = false` and `CollideSurface = false`.
- **Cost.** The sweep is about 5% of sim time in a four-AI game. It asks
  the spatial grid for shapes near the path, and keeps shields and
  experimentals (shapes reaching past 8) on a list of their own.
- **Left:**
  - Weapons don't target projectiles yet, so anti-missile defence waits:
    missiles have shapes, and interceptors would meet them.
  - Beams still hit from the engine (`OnCollisionCheckWeapon`).
  - Bombs drop straight down, without the bomber's speed.

### M201e: ballistic arc

- **The solution:** `BallisticArc` Low or High, from `tanθ = (v² ∓ √(v⁴ − g(gd² + 2hv²))) / (gd)`.
- **Aim and launch:** the aim controller's pitch takes that angle, and the shot leaves with a real vertical velocity.
- **Lifetime** comes from the flight time or the blueprint's `Lifetime`.
- **`LeadTarget` and `MuzzleVelocityReduceDistance`** are honoured.
- **Proof:** artillery lands on its target.

**What building M201e established:**

- **Most "direct fire" is an arc.** 83 retail weapons are `LowArc`, and
  among them is the MA12 Striker's gun, a few degrees up at its range. Only
  `RULEUBA_None` weapons fly straight; they now aim at the target in three
  dimensions, where every shot used to leave level.
- **Arcs need exact gravity.** A shell stepped with velocity-then-position
  lands short of the parabola its angle was solved for. The step now takes
  half a tick's gravity back, so a Lobo's high arc (65.7 degrees at 14 u/s
  over 30) comes down on its target.
- **Leading needs a velocity.** Units didn't have one: each now keeps its
  movement over the last tick. A `LeadTarget` weapon aims where the target
  will be when the shot arrives, refined once.
- **Left:** `MuzzleVelocityReduceDistance` (25 weapons) and a blueprint
  `Lifetime` are not read yet.

### M201f: area damage and shields

- **`DamageArea`:** retail's falloff and rings, and a real direction vector (a tree's `'Force'` reads `direction[1]`).
- **Shields** absorb through `OnGetDamageAbsorption` and `ArtilleryShieldBlocks`, and spill over.
- **Open question:** whether area damage touches projectiles.

**What building M201f established:**

- **FAF's Lua copy of `DamageArea`** (`/lua/sim/DamageArea.lua`, written for
  nukes) is the best description of Moho's. It shows:
  - no falloff: every target within the radius takes the whole amount;
  - distance measured in three dimensions from each target's position, so a
    ground blast leaves aircraft overhead alone;
  - allies spared by alliance, not only the instigator's army;
  - the instigator itself spared unless `damageSelf`;
  - props damaged;
  - projectiles not damaged. That settles the open question.
- **Shields absorb in the engine.** FAF wrote that copy to get past the
  native shield absorption, and retail's `shield.lua` says native code asks
  `OnGetDamageAbsorption` and subtracts the result from the damage to the
  units under the shield. A blast that meets a shield from outside now
  damages the shield, and the units under it take only the rest. A blast
  inside a shield reaches them whole.
- **Shields could not be hit by blasts before.** A shield's position is its
  centre, and a shot landing on the bubble is up to its radius away from
  it. So the engine looks for shields whose shape the blast reaches.
- **Area damage now credits its instigator** in each victim's damage
  record, as direct damage did. Kills by blasts used to go unrecorded.
- **`DamageRing`** spares its inner circle; before, it used only the outer
  radius.

M201a and M201b stand apart from the projectile work. M201c needs the terrain types. M201e is largely independent.

## Risks

- **Damage moves to Lua.** Every hit must count once: `OnImpact` deals it, and the engine stops. Friendly fire appears for the 188 weapons with `DamageFriendly`, so AI outcomes, checksums and replays shift.
- **Thousands of map props.** Each map prop's `OnCreate` and TrashBag costs time at load; this is measured before and after.
- **Script errors in the gate.** Every AI mode counts them. Each slice runs long AI games and the combat integration tests, and uses `cross_os_replay.py --direct` for determinism across OSes.
- **Collision cost.** Sweeping every projectile each tick uses the spatial grid. It is profiled against the stress test.
