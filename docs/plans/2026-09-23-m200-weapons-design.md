# M200 — Weapons through FA Lua

Status: design, 2026-09-23. Opens Phase E (gameplay fidelity). Builds on M185
(units and weapons as script-class instances) and Phase D (determinism).

## Why

A player feels combat more than anything else in the game, and ours is still
the engine's own. Retail FA's weapons are Lua state machines
(`/lua/sim/weapon.lua`, `/lua/sim/defaultweapons.lua`, plus hundreds of unit
overrides). They charge, fire racks and muzzle salvos, reload, pack and
unpack, drain energy, and spawn script projectiles that carry their damage.

Our C++ weapon does none of that. It fires one class-less projectile every
`1/RateOfFire` at the nearest enemy, from a bind-pose muzzle, and never
turns a turret. Every retail weapon's state machine is already live (M185
runs its `OnCreate`), but it sits in `IdleState` forever: nothing sends it
`OnGotTarget` or `OnFire`.

## What the survey found

These findings come from reading the engine code, retail's Lua, and unit and
projectile scripts extracted from the retail `.scd` archives.

- **Construction:**
  - `create_unit_core` caches only a handful of weapon blueprint fields. It
    ignores the first muzzle's rack, salvo, charge and reload data, and all
    turret, priority and restriction fields.
  - Weapon script instances exist, and `OnCreate` runs `SetupTurret`,
    `SetWeaponPriorities` and `ChangeState(IdleState)`.
- **Targeting:** nearest valid enemy. `SetTargetingPriorities` is stored and
  never read. `TargetRestrict*`, `MaxHeightDiff`, `FiringTolerance` and the
  attack order's target are ignored.
- **Turrets:** none. `AimManipulator::tick` is empty, so nothing slews,
  writes bones or gates fire.
- **Firing:** C++ (`Weapon::try_fire`), with one projectile per shot and
  racks, salvos, charge, reload and unpacking all ignored.
- **Projectiles:**
  - They are plain tables over `moho.projectile_methods`, with no script
    class and no `OnCreate`. The same table builder appears four times.
  - Retail's `CreateProjectileForWeapon` calls `proj:PassDamageData(...)`, a
    `Projectile.lua` method, so weapon-script firing can't work until
    projectiles are class instances. Death weapons already fail on it.
- **`Damage` argument order is wrong.** Retail calls
  `Damage(instigator, location, target, amount, type)`; ours reads
  `(instigator, target, amount, ...)`. Every retail call therefore does
  nothing: `Projectile.DoDamage`, `CollisionBeam`, and DoT.
- **Weapon callbacks the engine never sends:** `OnFire`, `OnGotTarget`,
  `OnLostTarget`, `OnStartTracking`, `OnStopTracking`, `OnHaltFire` and
  `OnEnableWeapon`. `OnMotionHorzEventChange` never reaches weapons either,
  since the unit event is never raised.
- **Engine methods:** all 14 methods the weapon classes call on `self` are
  bound. The gaps are semantic (`CreateProjectile` ignores its muzzle;
  `CanFire` knows nothing of ammo) and a few missing bindings: `CreateTrail`,
  which 103 of ~180 projectile classes call in `OnCreate`; `SetDamage`;
  `MetaImpact`; and `unit:RecoilImpulse`.

## Slices

### M200a — projectiles are script instances

- One place gives a projectile its Lua object: an instance of its blueprint's
  script class, resolved as units are (`ScriptModule`/`ScriptClass`, else
  `<dir>/<id>_script.lua`'s `TypeClass`) and cached per blueprint.
  - Fallback: the generic `Projectile` class, else the bare
    `moho.projectile_methods`.
  - It sets the usual fields and runs `OnCreate(inWater)`.
  - All four creation paths use it: C++ firing, `entity:CreateProjectile`,
    `weapon:CreateProjectile`, `CreateProjectileAtBone`.
- `weapon:CreateProjectile(muzzle)` spawns at the muzzle bone with the
  weapon's spread, projectile physics and damage, like C++ firing.
- `CreateTrail` is bound; `SetDamage` and `MetaImpact` get minimal bindings.
- `Damage` accepts retail's argument order (and the old one, which tests use).
- **Proof:**
  - A tank's `CreateProjectile('Turret_Muzzle')` returns an instance of its
    projectile class: `DamageData` is set, `PassDamageData` works, and the
    projectile sits at the muzzle.
  - An ACU's death weapon no longer errors.
  - `Damage(nil, pos, unit, 50, 'Normal')` hurts.
  - Four-AI games in the gate stay free of script errors: every shot now runs
    a projectile script.

### M200b — weapons fire through the state machine

- For script-classed weapons, the engine keeps acquisition and the fire
  clock, but instead of firing it sends:
  - `OnGotTarget` when a target is acquired;
  - `OnLostTarget` when it is lost;
  - `OnFire` when the clock expires with a target.
- Retail's state machine then charges, fires racks and salvos through
  `CreateProjectileAtMuzzle`, and reloads.
- Class-less weapons, beams (until they collide) and the C++ silo and
  OverCharge paths (M206) keep the old path.
- Weapons of units under construction stay silent.
- `OnMotionHorzEventChange` is raised.
- **Proof:** a tank beside an enemy walks Idle → FireReady → Firing, its
  projectiles are its class's, a salvo weapon fires its salvo, and a reload
  weapon waits in `RackSalvoReloadState`. A 6000-tick four-AI game is
  error-free, and `data.determinism`, `data.replay_roundtrip` and the
  cross-OS check still pass.

**What building M200b established about Moho:**

- **Blueprint defaults.** Scripts see blueprints the engine rebuilt from its
  typed copies, so a numeric weapon field a `.bp` omits reads as 0. Retail
  depends on this: its `GetDamageTable` adds `DamageRadius`, which 228 of
  its 494 weapons omit, the UEF commander's gun among them.
  `RegisterUnitBlueprint` now fills the fields retail reads unguarded.
- **The fire clock** counts whole ticks, `round(10 / RateOfFire)`, the same
  rounding FAF's DPS calculator uses. It restarts on every `OnFire`, even
  one the state machine ignores. Retail's bombers need `SkipReadyState`
  for exactly that reason: an `OnFire` that only moves the weapon from Idle
  to FireReady costs a whole cycle.
- **`CanFire` includes the unit's `Busy` state**, as FAF's decompiled
  pseudocode has it. `SetBusy` is that state: XSL0111's own script waits on
  `IsUnitState('Busy')`. The firing states set it, which makes a weapon's
  salvo exclusive unless the blueprint says `NotExclusive`.
- **Consequence: retail's reload weapons detour through Idle.**
  `RackSalvoReloadState` asks `CanFire()` while its unit is still Busy, so
  the weapon goes to Idle. Idle's `OnFire` restarts the clock, and the shot
  waits for the next one. That costs up to one fire period, unless an
  unpack, a charge or a reload animation already takes that long:
  `RackSalvoFireReadyState` fires at once when the blueprint has
  `AnimationReload`. Measured with `--weapon-test`: XSL0111 (reload
  animation) fires every 71 ticks against 1/RateOfFire = 67. DEL0204's
  gatling starts a salvo every 91 ticks: reload, unpack and a 3.1 s charge
  cover its 10-tick period. FAF's reload state skips `CanFire` for this
  reason; we match the engine and let each game's scripts decide.
- **Thread latency.** A state's `Main` is forked, so a shot leaves a tick
  after its `OnFire`. Shots are still exactly one period apart.
- **Motion events.** Our ground units reach full speed at once, so a start
  raises `Stopped→Cruise`, then `Cruise→TopSpeed` a tick later, and a stop
  raises `Stopping`, then `Stopped`. Retail's `Unit` script plays move
  sounds and effects on these events and forwards each one to its weapons.

### M200c — target priorities and restrictions

- Targets are chosen by `SetTargetingPriorities` order, then distance.
- `TargetRestrictDisallow`/`OnlyAllow`, the water-only flags,
  `MaxHeightDiff`, `TargetCheckInterval` and the attack order's target are
  honoured.
- Category matching moves where the sim can reach it, or categories are
  compiled when priorities are set.

**What building M200c established:**

- **Categories are copied when set.** FAF's `SetWeaponPriorities` passes a
  recycled table to `SetTargetingPriorities` and clears it right after, so
  Moho must copy it; we kept a reference, which would have lost FAF's
  priorities. `SetTargetingPriorities` now compiles a `CategoryExpr` tree
  that the sim tests without Lua.
- **Unmatched units are not targets.** All 337 retail priority lists end in
  `ALLUNITS`, which only makes sense if a unit outside every priority can't
  be targeted.
- **Range is a cylinder.** FAF's notes call `MaxHeightDiff` "cylindrical
  range": horizontal distance against the radii, height only against
  `MaxHeightDiff`. Our old 3D distance shortened every weapon's reach
  against aircraft.
- **Alliances.** Targets must be enemies by alliance. Before, only a unit's
  own army was spared, so team-mates shot each other.
- **Blueprint lists** such as `TargetRestrictDisallow` read commas as
  alternatives and spaces as a conjunction (`'TACTICAL MISSILE'`).
- **Footprints.** 205 retail units leave `Footprint.SizeX`/`SizeZ` out, and
  retail's `GetSkirtRect` reads them unguarded; the UEF TMD errored in
  `OnCreate`. Moho sizes such a footprint from the unit's `SizeX`/`SizeZ`.
- **Left for later:**
  - Countermeasure weapons (`OnlyAllow` `TACTICAL MISSILE`, `TORPEDO`) now
    have no targets instead of shooting units. Intercepting projectiles
    needs projectile targets.
  - `TrackingRadius` (turrets that track beyond their range) belongs to
    M200d.
  - `HeadingArcCenter`/`HeadingArcRange` are M200d as well.

### M200d — turrets aim

- Aim controllers belong to their weapon and slew heading and pitch toward
  the target at `SetFiringArc` speeds, within limits, using `osc::dmath`.
- They write the turret bones and report `OnTarget` within `FiringTolerance`,
  which gates `OnFire`.
- `OnStartTracking`/`OnStopTracking` fire.
- Muzzles fire from the animated bone.

**What the survey for M200d found.**

- **Aim controllers are stubs.** `tick` does nothing, and `SetHeadingPitch`
  snaps and reports on-target. `CreateAimController` records neither the
  weapon nor the label.
- **No manipulator moves a bone.** Only the skeletal animator writes bone
  matrices, straight into the skinning matrices the renderer reads.
  Rotators and aim controllers change nothing, so radar dishes don't spin.
  `GetPosition(bone)`, weapon muzzles and `CreateProjectileAtBone` all use
  the bind pose.
- **Retail's data.**
  - 284 of 494 weapons are turreted, and every one sets `FiringTolerance`
    (0–60°, mostly 2).
  - `TrackingRadius` multiplies the range, usually by 1.15–1.4.
  - 47 turrets can't yaw (`TurretYawSpeed` 0).
  - 66 weapons have heading arcs.
  - 7 use `TurretDualManipulators`, a torso yaw plus two arms with
    `SetFireControl('Right')`.

The slice splits in two, because a pose pipeline that renders turrets must
also take over how animators write bones, and the golden images depend on
that:

- **M200d-1: aim in the sim.**
  - **Link:** each aim controller knows its weapon and label, and the
    weapon's fire control is the first created or the one `SetFireControl`
    names.
  - **Aiming:** the weapon hands each of its aim controllers the target.
    Each tick the controller turns its heading (at the yaw bone) and pitch
    (at the pitch bone) toward it, at the arc's slew speeds and within its
    limits (a full 360° arc wraps), using `osc::dmath`.
  - **Firing:** it is on target within the weapon's `FiringTolerance`, and
    `CanFire` requires the fire control to be on target. With no target it
    returns to rest after `SetResetPoseTime`.
  - **Tracking:** the weapon gets `OnStartTracking(label)` and
    `OnStopTracking(label)`. `TrackingRadius` widens target acquisition, but
    firing still needs `MaxRadius`. `HeadingArcCenter`/`HeadingArcRange`
    filter targets by the unit's facing.
  - **Sim pose:** each tick a unit's bind-pose locals take its aim, rotator
    and slide deltas (animators aside), composed root-down.
    `GetPosition(bone)`, muzzles and projectile spawns read it, so a shot
    leaves the turned barrel.
- **M200d-2: the pose renders.** One pose per unit per tick:
  1. animators produce local transforms instead of skinning matrices;
  2. every manipulator applies in precedence order;
  3. skinning matrices come from the result.

  Turrets and rotators turn on screen, and animated bones feed the sim pose
  too.

**What building M200d-1 established:**

- **The SCM bone reader was wrong.** It read each bone's quaternion as
  `(x, y, z, w)`, but SCM stores `(w, x, y, z)` as SCA does. It also took
  the name offset for the parent index. Every sim bone transform was wrong:
  `GetPosition(bone)`, muzzles and `CreateProjectileAtBone`. Rendering
  never showed it, because skinning uses only the inverse bind matrices and
  animation builds its own transforms. The first aimed turret turned the
  wrong way.
- **Aim frames.** Heading turns about the yaw bone's own Y axis, and pitch
  about the pitch bone's X axis (negated, so a positive pitch raises +Z).
  Both are measured from the bone's rest pose.
  - Heading and pitch are radians; `SetFiringArc` takes degrees, as
    blueprints give them.
  - Retail only copies heading and pitch between controllers, for example
    the build arm to the gun, so the unit is ours to choose.
- **Precedence (revised in M200d-2).** M200d-1 let the higher-precedence
  manipulator replace a lower one on a shared bone. M200d-2 applies them in
  precedence order, each on top of the last, which is how a walking bot's
  torso aim rides its walk cycle. The UEF commander's OverCharge and main
  gun aim at the same arm, and the one not in use is disabled, so they
  never stack.
- **A disabled aim holds fire.** Retail disables an aim controller together
  with its weapon (OverCharge, `SetWeaponEnabledByLabel`), so a disabled
  fire control can simply block `CanFire`.
- **Found for M201: props are not script instances.** A destroyed unit's
  wreck (`/lua/wreckage.lua` `Wreckage`) has none of `Prop.lua`'s methods,
  so retail's `CreateWreckageProp` errors on `SetReclaimValues`. The gap
  predates M200, and now that units really die in combat, every wreck hits
  it.

**What building M200d-2 established:**

- **One pose, drawn and simulated.** Each tick a unit's manipulators apply
  to its bind-pose locals in precedence order: animators set the bones
  they animate, rotators and aim controllers turn bones on top, sliders move
  them. The pose composes root-down. The renderer's skinning matrices and
  the sim's bone positions (muzzles, `GetPosition(bone)`) both come from it.
- **Animators give local transforms.** An animated bone is expressed
  relative to its SCM parent as the frame poses that parent, or as the bind
  pose places it when the animation doesn't move it. Cross-fades blend these
  local transforms. So turrets turn on screen, rotators spin, and a bone the
  animation leaves alone follows its animated parent instead of staying
  behind at bind pose.
- **At rest costs nothing.** A turret at heading 0, a stopped rotator or a
  slider at home leaves its bone alone, so a unit with nothing moving keeps
  the bind pose and computes no skeleton. A seeded 3000-tick four-AI game ran
  as fast as on M200d-1 (13.2 s against 13.5 s median, within noise).
- **Animations now move sim bones.** Muzzles on an animated arm fire from
  where the animation holds them. `AnimCache` loads synchronously, so every
  machine poses alike.

## Risks

- **Gate failures from script errors.** Every AI mode counts script errors,
  and the gate plays four-AI games, so each slice is proven on long AI runs
  before it lands.
- **AI behaviour moves.** Real salvos, charge and reload times, `SetBusy` and
  unpacking change DPS and how armies fight, so game outcomes and lengths
  shift. That is expected; tests that assert tick-exact outcomes are
  revisited.
- **Latency.** A shot lands a tick or two after acquisition: threads forked
  during entity updates run at the next tick's resume. Whether Moho runs them
  in the same beat is unverified.
- **Cost.** A shot costs a few coroutines, a damage table and the projectile
  script's `OnCreate`. `Sim::threads` and the stress test are profiled before
  and after, and class lookups are cached per blueprint.
- **Determinism.**
  - `OnFire` is sent in entity-id and weapon order.
  - Script `Random` and spread come from `SimRandom`.
  - Turret math uses `dmath`.
  - The determinism, replay and cross-OS checks run on each slice.
