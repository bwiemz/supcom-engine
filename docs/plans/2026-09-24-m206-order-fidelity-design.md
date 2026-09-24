# M206 — Order fidelity

Status: design, 2026-09-24. The sixth milestone of Phase E (gameplay fidelity). Units move (M203, M204) and find paths cheaply (M205). What they do on arrival still falls short of Moho for the orders that make FA's late game: missiles, OverCharge, beams, teleports and ferries.

## Why

- **No missile has ever been fired:** tactical missiles and nukes never fire, and nothing builds them.
  - Retail's AI turns a launcher's auto-build on (`SetAutoMode(true)`) and waits for ammunition that never comes.
  - A launch order takes the unit's first weapon, which is often not its missile weapon, and fires nothing.
- **Anti-missile weapons shoot at nothing:** there are 43 of them in retail (TMD, SMD, anti-torpedo), and nothing can target a missile.
- **The economy side:**
  - OverCharge fires the main gun.
  - Teleports cost nothing and take no time, because economy events drain nothing.
- **Beams:** beam weapons have no beam.
- **Build and reclaim ranges** ignore the target's size and `MaxBuildDistance`.

## What the survey found

These findings come from reading retail's `defaultweapons.lua`, `weapon.lua`, `Unit.lua`, `platoon.lua`, the unit scripts and blueprints, FAF's engine annotations (`engine/Sim/Unit.lua`, `UnitWeapon.lua`, `Sim.lua`, `User.lua`), and the engine.

### Silo weapons

- **Counted weapons:** a weapon with `CountedProjectile` fires ammunition the unit stores and builds.
  - **Nuke or tactical:** `NukeWeapon` makes it a nuke; any other counted weapon counts as tactical. This includes anti-nukes, as their retail command caps (`SiloBuildTactical`) and scripts show.
  - **Storage:** `MaxProjectileStorage` caps what the silo builds. The storage counts are the unit's, one for each kind.
  - **Where they are:** 25 retail units carry one.
    - Tactical launchers.
    - Nuke launchers, and nuke subs and battleships.
    - Anti-nukes.
    - The UEF and Seraphim ACUs and the Seraphim SACU, which gain one through an enhancement. The script enables the weapon (`SetWeaponEnabledByLabel`) and adds its command caps.
- **What retail's weapon script does:** it fires a counted weapon itself.
  - `IdleState.OnGotTarget` refuses when `CanFire()` is false.
  - `RackSalvoFireReadyState` goes straight to firing for a counted weapon, "because we won't get another OnFire call".
  - `RackSalvoFiringState` creates the missile at the muzzle, then calls `RemoveNukeSiloAmmo(1)` (after `NukeCreatedAtUnit()`) or `RemoveTacticalSiloAmmo(1)`. The engine must not also take the ammunition.
- **What FAF's notes on the engine say:**
  - The engine's `CanFire` includes `HasSiloAmmo`.
  - Only a weapon with `ManualFire` and without `OverChargeWeapon` serves tactical and nuke orders.
  - A missile is built out of `10 × BuildTime / BuildRate` blocks: one per tick, `BuildTime / BuildRate` seconds.
  - The engine uses the unit's `GetEconomyBuildRate`, `GetEnergyBuildAdjMod` and `GetMassBuildAdjMod` in silo builds. Retail defines all three; adjacent power generators lower a silo's `EnergyBuildAdjMod`.
  - `OnSiloBuildStart(weapon)` and `OnSiloBuildEnd(weapon)` bracket a build, and the unit is in the `SiloBuildingAmmo` state throughout. The Yolona Oss's script watches that state to tell a finished missile from a cancelled one.
  - `IssueSiloBuildNuke` and `IssueSiloBuildTactical` order builds. Retail's orders panel issues one per left-click (as `BuildSiloNuke` and `BuildSiloTactical`: `GetUnitCommandFromCommandCap` maps names, not just prefixes), and a right-click toggles auto mode.
  - `GetMissileInfo` reports each kind's storage count, maximum and builds ordered.
  - An assisting engineer pays at the silo's rate scaled to its own build rate (`UpdateConsumptionValues`).
- **The costs:** a missile costs its projectile blueprint's `Economy`.
  - A strategic nuke: 1,350,000 E, 12,000 M and BuildTime 324,000, or 300 s at a launcher's 1080 build rate.
  - A tactical missile: 3,600 E and 180 M, or 30 s at 80.
- **How the missiles fly:** every silo weapon is unturreted, with no ballistic arc. Their projectiles home (`TrackTarget`), and start slow or at rest: `InitialSpeed` 0 for nukes, `MuzzleVelocity` 0 to 10.
  - A nuke's script flies it straight for 7.5 s with tracking off, then turns it toward its target. It has to leave along its muzzle bone, which points up.
  - The engine aims every shot at its target, which stands in for turret aim. A nuke fired that way would fly flat and hit the ground beside its own launcher.
- **`ManualFire` is written as the number `1`** on every nuke launcher. The engine read only booleans, so no nuke weapon counted as manual.
- **Ground targets:** `weapon:SetTargetGround` takes a position in retail (`seraphimunits.lua`). The engine took a boolean, and weapons had no ground target at all.
- **`TargetType = 'RULEWTT_Projectile'`:** 43 retail weapons target projectiles.
  - The engine never reads the field.
  - It needs to: `TargetRestrictOnlyAllow` limits every one of them to missiles or torpedoes (`TACTICAL MISSILE`, `STRATEGIC MISSILE`, `TORPEDO`), so none takes a unit, and none takes anything else either.

### The rest

- **Economy events:** `CreateEconomyEvent` counts time and drains nothing, and ignores its progress callback. Teleports and OverCharge are free.
- **OverCharge:** the order fires the unit's first ranged weapon, not its OverCharge weapon.
- **Teleport:**
  - **What retail does:** `InitiateTeleportThread` charges an economy event of (mass × `TeleportMassMod` + energy × `TeleportEnergyMod`), for that times `TeleportTimeMod`, then warps.
  - **What the engine does:** it pops the order at once, so the orders queued behind run during the charge.
- **Beams:** a beam weapon fires the engine's projectile. A collision beam never collides, and `OnImpact` fires once, faked.
- **Ranges:** build, reclaim, repair and capture ranges are fixed, measured centre to centre. `MaxBuildDistance` is read nowhere.
- **`Issue*` and the queue:** most of the engine's `Issue*` functions clear the queue. Retail's AI calls `IssueClearCommands` before the ones it means to replace, so Moho's append.
- **Ferry:** there are no beacons. The order re-queues itself like a patrol.

## Slices

### M206a: silo weapons

- **Silo builds:**
  - **Orders:** `IssueSiloBuildNuke` and `IssueSiloBuildTactical` (sim and UI) request a build. They go to the unit's silo, not its command queue, so a build order does not cancel a move. `GetUnitCommandFromCommandCap` maps names as Moho does.
  - **Auto mode:** keeps building while there is room.
  - **Which weapon:** the unit's first enabled counted weapon of the kind.
  - **Cost:** the projectile's cost, at the unit's build rate, drawn through the army's economy. A stall slows the build, and a paused unit's build waits.
  - **Storage:** a build waits while the kind's storage is full.
  - **The script:** gets `OnSiloBuildStart` and `OnSiloBuildEnd`, and the unit is `SiloBuildingAmmo` meanwhile.
  - **Assist:** a guarding engineer adds its build rate, at its share of the cost.
  - **Stopping:** `StopSiloBuild` cancels the build and the builds ordered.
  - **`GetMissileInfo`:** reports true numbers.
- **Launch orders:** `IssueNuke` and `IssueTactical` append to the queue.
  - The order uses the unit's first enabled weapon with `ManualFire` and `CountedProjectile` (and `NukeWeapon` for a nuke), not OverCharge.
  - It aims at the target unit or the ground point. A mobile unit moves into range; a structure out of range drops the order.
  - It waits for ammunition, and ends when the weapon launches.
  - Retail's script fires the missile and takes the ammunition.
- **Weapons:**
  - Counted and manual-fire weapons fire through the script.
  - A manual weapon takes targets only from its unit's order.
  - `CanFire` needs ammunition for a counted weapon.
  - Weapons can hold a ground target: `SetTargetGround(pos)` and `GetCurrentTargetPos`.
  - `ManualFire = 1` counts as manual.
- **The missile's flight:** a counted weapon's missile leaves along its muzzle bone, at the weapon's muzzle velocity, and a shot at rest accelerates along its facing. Other weapons keep aiming at their target until their muzzles are checked (see Risks).
- **Anti-missile weapons:** they fire nothing until M206b gives them missiles to shoot. Their restrictions keep them off units, and an anti-nuke now holds real ammunition, so the test checks that it spends none.
- **Proof:** `--missile-test`:
  - A tactical launcher builds a missile in its build time, for its cost.
  - Auto mode fills storage and stops.
  - An assisted build is faster, and costs the same.
  - A tactical missile launched at a unit kills it; one launched at a point lands there.
  - A nuke rises from its silo and destroys units about its target.
  - An order issued without ammunition waits and fires once the missile is built.
  - A stopped order fires nothing.
  - An anti-nuke holding a missile, beside an enemy, fires nothing.

**What building M206a established:**

- **A missile costs exactly its price.**
  - The silo asks the economy for its build on the tick it starts, and lays the first block on the next tick. So every tick of progress is charged, and the test measures 3,600 E and 180 M for a tactical missile, unassisted or assisted.
  - The progress is kept in double precision: 300 blocks of 1/300 summed in single precision fall short of 1, and finished a tick late.
- **Retail's script handles a cancelled launch:**
  - The stateless `OnLostTarget` packs an unpacking launcher up, so a launch cancelled while the silo opens keeps its missile.
  - A launcher already in its firing state is stopped by `OnHaltFire`, which the engine calls when a manual weapon's order goes.
- **Anti-missile weapons never took units:** the survey was wrong here. Each one's `TargetRestrictOnlyAllow` admits only missiles or torpedoes, and most also fire only at the air layer. So the engine needs nothing to keep them off units, only something to shoot (M206b).
- **The stop paths have to cover the new request:** a silo's request lives beside the script-set consumption, so it has to go wherever that does.
  - A launcher killed mid-build lingers through its death animation, and kept charging its army until its request was cleared on dying.
  - A paused silo's helpers kept paying until assisting required the silo to be running.

### M206b: missile defence

**What a second survey found.** Retail's missile defence is mostly script, and the scripts can't work until projectiles have an identity.

- **Projectiles have no categories.** `EntityCategoryContains` and the filters answer false for anything but a unit. Retail's `Projectile.OnCollisionCheck` then never refuses a collision:
  - a tank shell hits an enemy missile it crosses;
  - torpedoes hit each other;
  - a nuke's `OnImpact` doesn't see that what it met was a projectile, so a nuke shot down would still detonate.
- **The 43 anti-projectile weapons are four kinds** (`RangeCategory = 'UWRC_Countermeasure'`, all single-target):
  - **Homing interceptors:** the anti-nukes' counted `HitAssignedTarget` missiles, the Seraphim TMD, and Aeon and Seraphim anti-torpedo.
  - **Guns:** the UEF Phalanx fires Damage-1 shells at 100 u/s, at missiles of 1–3 health.
  - **Beams:** the Cybran zappers. These are M206c's.
  - **Lures:** the Aeon TMD's flare and the UEF and Cybran depth charges.
    - They are Lua `Flare`/`DepthCharge` entities whose `OnCollisionCheck` calls `SetNewTarget` on an enemy missile or torpedo that touches them.
    - They need the entity's army, which `Entity(spec)` takes from `spec.Owner`.
- **Target categories:**
  - Tactical missiles are `TACTICAL MISSILE`, nukes `STRATEGIC MISSILE`, and torpedoes `TORPEDO`.
  - Interceptors are `ANTIMISSILE` or `ANTITORPEDO`, never `MISSILE`, so the collision rules let them hit.
  - Nukes set `DesiredShooterCap = 1` (a top-level blueprint field), so only one anti-nuke fires at each.
- **`OnCollisionCheck(self, other)`:** `self` is the entity struck and `other` the one hitting it (retail's comments, and its `HitAssignedTarget` test). The engine asked the mover first. So a lure's check ran second, and the mover's side errored on a blueprint-less Lua entity.

**The slice:**

- **Projectiles have their blueprint's categories,** plus `ALLPROJECTILES`.
  - `EntityCategoryContains` and `EntityCategoryFilterDown/Out` accept them.
  - `ALLUNITS` matches every entity except projectiles.
- **Collision checks:** the struck side's `OnCollisionCheck` runs first, and the first refusal ends it. A Lua entity takes its army from `spec.Owner`.
- **Weapons read `TargetType`.** A `RULEWTT_Projectile` weapon targets live enemy projectiles in range.
  - The usual checks apply: its layer caps (a projectile is `Air` above the surface and `Water` below), its restrictions, and `TargetCheckInterval`.
  - It skips a projectile already held by its `DesiredShooterCap` of weapons.
- **Shots:** a leading weapon leads a projectile target. A weapon's `ProjectileLifetime` (or `ProjectileLifetimeUsesMultiplier` × MaxRadius / MuzzleVelocity) overrides its projectile blueprint's `Lifetime`. This holds for every weapon: 139 set the one and 174 the other.
- **Proof:** `--defence-test`:
  - an anti-nuke kills a nuke in flight, which then harms nothing, and a second anti-nuke holds its fire;
  - the Phalanx and the Seraphim TMD stop tactical missiles;
  - the Aeon flare lures an enemy missile and not its own side's;
  - anti-torpedo stops a torpedo;
  - a tank shell passes through an enemy missile.

**What building M206b established:**

- **Most of the defence was already in retail's scripts,** waiting for categories on projectiles and armies on Lua entities.
  - The flare's lure, retail's collision rules and the nuke's "don't detonate when shot" check came to life with them.
  - The engine's own part is the targeting: which enemy projectiles are in reach, and how many weapons take each.
- **The anti-nuke intercepts high.** The nuke is killed 200 above its target, 4 from it horizontally, because the anti-nuke's 90 range is a horizontal cylinder.
- **`DesiredShooterCap` is a top-level field.** The first reading looked in `Defense` and let both anti-nukes fire.
- **The order of collision checks shows only as a side effect.**
  - A flare refuses either way, so whether the struck side is asked first shows only in whether the missile is asked at all.
  - The test pins it to retail's comment: "the thing hitting us has no idea".
- **Torpedoes exposed two naval gaps,** left for a naval slice:
  - A surfaced sub's torpedoes leave just above the water, aimed at their target's centre. They never dive, so anti-torpedo weapons, whose layer caps are water only, can't see them. Moho launches them along their muzzles, as M206a does for silo missiles only.
  - A dived sub sinks only while it moves.
  - The test puts its sub under water.

**Risks:**
- **The category fix is global:** every projectile collision follows retail's rules from now on. The long AI games will play differently.
- **Readings, not measurements:**
  - A projectile's layer, `DesiredShooterCap`'s default (none) and projectile targeting without intel are inferred from scripts, not measured.
  - Comma is union, so the naval TMDs' `'TACTICAL,MISSILE'` admits enemy AA missiles too.

### M206c: beam weapons

**What a third survey found:**
- **Twenty retail weapons are beams.** They come from 15 `DefaultBeamWeapon` classes.
  - **Continuous** (`BeamLifetime` 0): the Monkeylord, GC eye, CZAR and MLG.
  - **Pulsed:** Cerberus and Seraphim point defence, the zappers, the Novax, Hiro cannons and Othuy.
  - **Not a beam:** URL0001's OverCharge is the one other weapon with `BeamLifetime`, and it fires a projectile.
- **Where a beam starts:** `spec.OtherBone` is the muzzle ("bone of weapon's unit to attach to", from the retail engine's own strings). The beam reaches along the muzzle's facing, not toward the target, to `MaximumBeamLength` or else MaxRadius.
- **How often it hits:** a beam checks for collisions, and calls `OnImpact`, every `CollisionCheckInterval + 1` ticks while enabled.
  - This is FAF's documented and tested reading. Its DPS formulas, and its 2014 rebalance that halved beam damage when the delay went to 0, agree.
  - Retail's own comment, "only when the thing it is touching changes", is wrong.
  - `SetBeamFx(fx, true)`, which continuous beams call, checks at once.
- **What stops it:** the first unit, shield met from outside, prop or projectile whose `OnCollisionCheckWeapon(weapon)` allows it (`CollideFriendly`, `DoNotCollideList`), else the ground or the water.
- **The engine's beams did nothing:** they never collided, and never moved from where they were made. `SetBeamFx(true)` faked a `'Terrain'` impact.

**The slice:**
- **Beam pass:** each tick after the units have moved, every beam is posed on its launcher's muzzle. An enabled one is checked when its countdown runs out: its end goes where it stops, and its script hears `OnImpact(type, target)`.
- **Bindings:**
  - `GetPosition(1)` is the beam's end.
  - `Destroy` goes the way of any entity (`OnDestroy`, unregistered).
  - The engine calls a beam's `OnCreate`, which makes its `Trash`.
  - `FireWeapon` on a scripted weapon gives it `OnFire`.
- **Weapons:** beam weapons fire through their scripts. A weapon is a beam weapon when its script makes beams for it.
- **Two fixes the beams needed first:**
  - **Bone scale:** bone world positions ignored the blueprint's `UniformScale` (0.05 on most units), so every muzzle sat about 14 times too far from its unit (see the risks below).
  - **Blast reach:** `DamageArea` measured to units' positions (their feet). A beam's 0.5 blast on a big unit's hull, or a shell on a structure's roof, reached nothing. It now measures to their shapes.
- **Proof:**
  - `--beam-weapon-test`:
    - A Cerberus's three beams kill a tank, without a projectile.
    - A pulsed beam hits 3 times a shot, 3 ticks apart; the Monkeylord's every other tick.
    - The beam runs from the muzzle, past a friendly structure, to its target's face, and its blast hurts the structure.
    - A zapper meets a missile.
    - A Striker's muzzle is 0.4 ahead.
  - `--area-test`: a blast on a structure's roof hurts it.

**What building M206c established:**
- **Plausible outcomes hid the scale bug.** Shots hit because they started closer to their targets. It showed only when a beam, which reaches from its muzzle, stopped at the ground. The defence test's Phalanx and frigate had passed on the wrong geometry. With real muzzles:
  - **The Phalanx:** a lone Phalanx (1 damage every 2 s, not led) hits a 2-health missile but may not stop it. The test now checks the hit.
  - **The frigate:** its anti-torpedo (a 1-second shot at speed 2) reaches only torpedoes at its hull. The test uses a destroyer.
- **Friendly-fire filtering is the struck side's script.** The engine only asks, as for projectiles.
- **Two game-UI goldens changed, and were re-blessed.**
  - **The glow:** a screen-filling white glow over the starting ACU is gone. It was the warp-in's emitters at the unscaled bone positions, near the camera; the scale fix alone accounts for most of the difference.
  - **The trees:** the rest is trees knocked flat at the warp-in. Its blasts (radius 11, 20 and 27) now reach tree groups whose boxes overlap them.

**Risks:**
- **Scale is global:** every muzzle, bone attachment and scripted bone position moves to where the model puts it. Combat changes throughout.
- **Readings, not measurements:** the cadence's phase (a check on the enable tick), and beams stopping at scripted entities (they don't), are readings.

### M206d: economy events, OverCharge, teleport

- **Economy events:** they drain their resources and slow with a stall, and they call their progress callback.
- **OverCharge:** the order fires the OverCharge weapon through its script (`OnEnableWeapon`, the energy drain).
- **Teleport:** the order stays until the warp, and a cancel calls `OnFailedTeleport`.

**What retail's scripts do with them:**
- **`CreateEconomyEvent(unit, energy, mass, time, callback)`** asks the unit's army for its cost over `time` and calls `callback(unit, progress)` as it goes. `WaitFor` on it returns when it is paid.
- **Teleport:** `InitiateTeleportThread` makes an event of mass × `TeleportMassMod` + energy × `TeleportEnergyMod` energy, over that times `TeleportTimeMod` seconds, waits for it, then `Warp`s. An engineer's costs 91 energy over 0.9 s, a Titan's 1290 over 12.9 s, an ACU's 150,000 over 15 s.
- **OverCharge:** the weapon starts switched off. Its `OnEnableWeapon` switches it on and the main gun off; firing switches them back and pauses it for 1/`RateOfFire` (3.3 s). With `EnergyChargeForFirstShot = false` the shot is free and its 5000 energy is drawn over the next second, as the weapon's recharge.

**The slice:**
- **Economy events:** each tick, before the economy, every live event asks its army for cost ÷ time per second. After the economy it moves on as far as its army paid: the lower of the efficiencies of the resources it asks for. Then its callback hears its progress. An event whose unit is gone is cancelled.
- **OverCharge:** the order uses the unit's `OverChargeWeapon`, at a unit. In range, the engine calls the weapon's `OnEnableWeapon` once; the weapon takes the order's target and fires through its script, and the order ends with the shot. Called off before the shot, a weapon still on hears `OnDisableWeapon`.
- **Teleport:** the order holds the head of the queue from `OnTeleportUnit` until the unit warps, so what is queued waits. Removed first, the unit hears `OnFailedTeleport`, which frees it.
- **Proof:** `--charge-test`:
  - An event draws its cost, calling its script ten times a second up to 1; an army that can't pay runs its event slower.
  - An ACU's OverCharge kills a tank, draws 5000 energy, and gives the main gun back; called off, it switches the OverCharge off.
  - An engineer's teleport charges in place with a move queued, warps, then moves; called off, it fails and the engineer can move.
  - Seven mutations (no request, no stall, no callback, no `OnEnableWeapon`, the order popping at once, no `OnFailedTeleport`, no `OnDisableWeapon`) each fail it.

**What building M206d established:**
- **Free events had hidden an economy in the tests.** `--cmd-test`'s enhancement and teleport checks ran on an army with nothing stored; they now give it storage and resources.
- **One unaffordable event starves its army.** The first test draft asked a billion energy on the army it then checked: its store emptied in a tick, and every later check stalled. The test puts that event on another army.
- **A ridge hid a correct shot.** The first OverCharge scene fired over a 1-unit crest and hit it. The shot was aimed right; the test moved to flat ground.

**Risks:**
- **Readings, not measurements:** an event's pace under a stall (the lower efficiency), when a new event first asks (the next economy step), and the engine switching off an OverCharge called off before its shot are readings of retail's scripts, not measurements of Moho.

### M206e: ranges and the queue

- **Ranges:** build, reclaim, repair and capture ranges run from the target's footprint edge, with `MaxBuildDistance` where it is set. The default is to be confirmed against Moho.
- **Guard-assist:** checks distance.
- **The queue:** `Issue*` functions append, as Moho's do.
- **Launch orders:** a mobile unit too close to its target backs away (M206a drops the order).

**What the decompiled engine shows:** [faf-re](https://github.com/Draiget/faf-re) decompiles Moho's unit tasks, with the addresses of each function. It settles what the scripts and FAF's notes left open.
- **Defaults** (`RUnitBlueprint`, `REntityBlueprint::OnInitBlueprint`):
  - `MaxBuildDistance` is 5.
  - A blueprint without a `Footprint` gets `ceil(SizeX)` × `ceil(SizeZ)`.
  - The skirt is at least the footprint, and a positive `SkirtOffset` becomes 0.
- **The gap:** every range is measured as a gap: the ground distance between the two centres, minus the builder's largest footprint side (whole cells), minus the target's largest side.
- **Build** (`CUnitMobileBuildTask`):
  - **In range:** the gap against the new structure's largest skirt side is at most `MaxBuildDistance`. A T1 engineer (footprint 1) builds point defence (skirt 1) from 7 away, and a land factory (skirt 8) from 14.
  - **Out of range, or standing on the site:** the builder moves just clear of the site's skirt, grown by one cell. Still out of range after that, the order fails.
- **Repair** (`CUnitRepairTask`, which also serves a guard's assist): the gap against the target's skirt. It starts within `MaxBuildDistance` and carries on until the gap passes twice that.
- **Reclaim** (`CUnitReclaimTask`):
  - **In range:** the gap against the target's footprint is at most `MaxBuildDistance`, or `AI.GuardScanRadius` if that is larger and the unit is patrolling. A point target counts as 2 × 2.
  - **Out of range:** the unit walks up to the target.
- **Capture** (`CUnitCaptureTask`): the footprint gap, against fixed numbers rather than `MaxBuildDistance`. The unit moves in when the gap is over 5 and gives up beyond 10.
- **Guard** (`CUnitGuardTask`): a guarding engineer stays put while within twice `MaxBuildDistance` of the unit it guards.
- **`Issue*`:** every sim-side `Issue*` passes `clearQueue = false`, so it appends. `IssueClearCommands` clears, and `IssueStop` issues a Stop.
- **Launch orders** (`CUnitFireAtTask`):
  - **Too close:** a mobile launcher backs off along the line from the target through itself, to 1.1 × `MinRadius`. An immobile one gives up.
  - **In range with no missile:** if the silo is neither building nor full, the order asks it for one, and gives up if the silo won't take it.
  - **Firing:** a nuke calls its unit's `OnNukeLaunched`.

**What the engine does instead:**
- **Ranges:** fixed and measured centre to centre (build 6, reclaim 5, repair 6, capture 6), whatever the target's size.
- **Footprints:** blueprint registration fills a missing `Footprint` from the size rounded to nearest (FAF's reading), where Moho rounds up. Props get none.
- **Guard:** a guarding engineer adds its build power from any distance.
- **`Issue*`:** 14 of the functions clear the queue.
- **Launch orders:** a launcher too close drops the order, and one without a missile waits for a build that nothing orders.

**The slice:**
- **Footprints:** a missing one rounds the size up, as Moho's does; props get one; a skirt is at least the footprint.
- **Ranges:**
  - Build, repair, reclaim and capture measure their gaps as above.
  - Out of range, a builder heads just clear of the target's skirt rather than stopping at the edge of its reach.
  - A builder standing on its site moves off it.
  - Repair continues to twice its reach.
- **Guard:**
  - A guarding engineer assists only within reach of what it assists (repair's rule, against the target's skirt), and moves in otherwise.
  - It follows the unit it guards once that unit is more than twice its `MaxBuildDistance` away.
- **The queue:** `Issue*` appends.
- **Launch orders:** too close, a launcher backs off; with no missile, the order asks the silo for one; a nuke calls `OnNukeLaunched`.
- **Proof:** `--range-test`:
  - **Build:** a T1 engineer builds point defence from 7 without moving, walks from 9, and builds a land factory from 14. An ACU (`MaxBuildDistance` 10) builds point defence from 12.
  - **Repair:** it keeps going as its target moves away, and stops past twice its reach.
  - **Reclaim:** within reach it reclaims in place; beyond, it walks up.
  - **Guard:** an engineer guarding an ACU that builds out of the engineer's reach moves in before the build goes faster.
  - **Footprints:** a T2 tank 1.2 long spans 2 cells. An engineer reclaims an oak grove (8 by 10) from 16 without moving.
  - **The queue:** `IssueMove` then `IssueAttack` leaves both orders queued.
  - **Launch orders:** an ACU with the tactical-missile enhancement, ordered at a point too close, backs off and fires. A TML ordered to fire with an empty silo starts building a missile.

**What building M206e established:**
- **The back-off needed the weapon to wait.** The test's launcher is an ACU with the tactical-missile enhancement (`MinRadius` 5); a nuke sub needs 140 of open water. Our manual weapon took the order's target as soon as the order was queued, even too close, and the unpacking weapon held the ACU in place. Moho hands the weapon its target only in range (the fire-at task's Starting state). The weapon now takes it only while the order is within its range band.
- **Missile-test effects:**
  - **An empty launcher's own order builds a missile.** In `--missile-test`, a launch cancelled while waiting leaves its launcher holding two missiles: the one it was given, and the one its order asked the silo for.
  - **Assisting engineers were out of reach.** Three of the four engineers helping a silo stood beyond it, so they walked two ticks before helping. The test now places them within reach.
- **`IssueTransportUnloadSpecific(transports, units, position)` was `IssueTransportUnload` under another name,** reading its position from the unit list. It now reads the third argument. Which units it drops is still not modelled.
- **A path the pathfinder puts off must be asked for again.** A unit walking up to its work sets its goal once, but the navigator keeps a throttled request pending without retrying it. The approach now asks again, as the move orders do.
- **The footprints were already there.** Blueprint registration fills a missing `Footprint` (an earlier fix, for scripts that read it), and the survey missed it. The new fallback changed no test, and a mutation that removed it was not caught. What remains is Moho's rounding up, which differs on 13 retail axes (a T2 tank's 1.2 length is 2 cells, not 1), and footprints for props, which registration leaves alone.

**Risks:**
- **Readings, not measurements:** where `PrepareMove` puts a unit (it isn't decompiled yet), and which unit categories may help (REPAIR and RECLAIM stand in for Moho's builder checks).
- **Factories assisting factories now do nothing.** Moho gives guarding factories their own behaviour, which reads as copying the queue. It isn't modelled, and they no longer lend build power.
- **The queue change reaches every script.** Retail's AI clears before it replaces, so appending should match Moho, but the long AI games will play differently.

### M206f: ferry

- **Beacons:** ferry beacons, and transports that load at the beacon and unload at its destination.

## Risks

- **Missiles now fly.** Long AI games reach nukes and tactical missiles, which play differently. The cross-OS replay and the long AI games check them.
- **Muzzle launch is narrow.** 89 retail weapons are unturreted with homing projectiles: the silo missiles, torpedoes, cruise missiles and anti-missiles. Moho launches all of them along their muzzles. Only the counted ones do yet, because only they need to rise before they turn. The rest are left for when each class is checked.
- **The silo's pace is a reading.** A block per tick and `BuildTime / BuildRate` seconds come from FAF's notes, not a measurement. The test holds the pace, so a correction is one place.
