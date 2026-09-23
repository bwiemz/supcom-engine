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

### M200c — target priorities and restrictions

- Targets are chosen by `SetTargetingPriorities` order, then distance.
- `TargetRestrictDisallow`/`OnlyAllow`, the water-only flags,
  `MaxHeightDiff`, `TargetCheckInterval` and the attack order's target are
  honoured.
- Category matching moves where the sim can reach it, or categories are
  compiled when priorities are set.

### M200d — turrets aim

- Aim controllers belong to their weapon and slew heading and pitch toward
  the target at `SetFiringArc` speeds, within limits, using `osc::dmath`.
- They write the turret bones and report `OnTarget` within `FiringTolerance`,
  which gates `OnFire`.
- `OnStartTracking`/`OnStopTracking` fire.
- Muzzles fire from the animated bone.

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
