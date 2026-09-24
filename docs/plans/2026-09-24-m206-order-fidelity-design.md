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

- **Projectile targets:** weapons whose `TargetType` is `RULEWTT_Projectile` target enemy missiles: TMDs, anti-nukes, anti-torpedo.
  - Missiles have to be targetable: `SetCollisionShape`, `Projectile` categories, `DoTakeDamage`.
  - `HitAssignedTarget` interceptors must reach them.
- **Proof:** a nuke fired at an anti-nuke's ground is shot down, and a TMD stops a tactical missile.

### M206c: beam weapons

- **Beams:** a collision beam per muzzle, following the bone.
  - A raycast every `CollisionCheckInterval`, out to the weapon's range, against units, shields, props, terrain and water.
  - `OnImpact` with retail's types, and `GetPosition(1)` at the beam's end.
- **Weapons:** beam weapons fire through the script. Whether a weapon is a beam is decided by its class, not by `BeamLifetime`.

### M206d: economy events, OverCharge, teleport

- **Economy events:** they drain their resources and slow with a stall, and they call their progress callback.
- **OverCharge:** the order fires the OverCharge weapon through its script (`OnEnableWeapon`, the energy drain).
- **Teleport:** the order stays until the warp, and a cancel calls `OnFailedTeleport`.

### M206e: ranges and the queue

- **Ranges:** build, reclaim, repair and capture ranges run from the target's footprint edge, with `MaxBuildDistance` where it is set. The default is to be confirmed against Moho.
- **Guard-assist:** checks distance.
- **The queue:** `Issue*` functions append, as Moho's do.
- **Launch orders:** a mobile unit too close to its target backs away (M206a drops the order).

### M206f: ferry

- **Beacons:** ferry beacons, and transports that load at the beacon and unload at its destination.

## Risks

- **Missiles now fly.** Long AI games reach nukes and tactical missiles, which play differently. The cross-OS replay and the long AI games check them.
- **Muzzle launch is narrow.** 89 retail weapons are unturreted with homing projectiles: the silo missiles, torpedoes, cruise missiles and anti-missiles. Moho launches all of them along their muzzles. Only the counted ones do yet, because only they need to rise before they turn. The rest are left for when each class is checked.
- **The silo's pace is a reading.** A block per tick and `BuildTime / BuildRate` seconds come from FAF's notes, not a measurement. The test holds the pace, so a correction is one place.
