# Winged attack runs, bomb release and idle auto-engage: Moho vs the engine, and a port plan

Design, 2026-10-05: parity group 1 of `docs/plans/2026-10-05-parity-ratchet-triage.md` (a bomber given an attack order never drops). The research is below; §6.8 is the order of work.

Path prefixes used below:

- **FR:** faf-re's `src/sdk/moho/` (Draiget/faf-re, the decompiled FAF build of Moho)
- **EN:** this repository's `src/`, as of 461f3230 (line numbers drift as the port lands)
- **BP:** retail FA's `units/` (from the Steam install's `gamedata/units.scd`)

Certainty tags:

- **[read]** means the rule is read directly from faf-re code.
- **[inferred]** means I reasoned it out from faf-re code or physics.
- **[doubt]** means faf-re's own code or comments suggest the decompile may be wrong.

---

## 0. Summary

1. **Moho never parks an aircraft at weapon range.**
   - For any `Air.CanFly` unit, the attack task sends the navigator **to the target itself**. It does not go to weapon range.
   - Once the aircraft is within `max(weapon range, Air.EngageDistance)`, the task gives the attacker a *desired target* and then sleeps.
   - From then on, `CUnitMotion::CalcMoveAir` runs `ComputeAirCombatTactics` every tick for `Air.Winged` units. This is an 8-state machine: None, Combat, NormalTurn, CombatTurn, CombatTurnB, Realign, BreakOff, ReturnToMap.
   - The machine overrides the steering, the speed and the altitude. It also raises `UNITSTATE_MakingAttackRun` in Combat, NormalTurn and BreakOff.
2. **Bomb release** is in `UnitWeapon::CanFire`, Winged branch. It needs:
   - `MakingAttackRun`;
   - a ballistic **release point**, `CalcBombDrop`, which is the target position minus the aircraft's velocity times the fall time;
   - the release point within `BombDropThreshold` of the aircraft.

   faf-re **as written** fires when the release point is *farther* than the threshold. That contradicts the physics, and faf-re's own probe comments complain about "bombs in any direction". A model of both readings shows faf-re's version drops at 39 u and misses by 3 to 11 u. The swapped reading drops at 27 u and carpets the target. I recommend the swapped reading (§3.3).
3. **Bombs inherit the launcher's velocity** when the projectile blueprint has `RealisticOrdinance`, which all 13 retail bomb projectiles do.
   - The horizontal velocity is re-aimed at the target, or at its predicted position. Its magnitude is the launcher's speed, and its vertical part is 0.
   - FiringRandomness does not affect the bomb's path.
4. **Idle auto-engage** works like this. A weapon with `AutoInitiateAttackCommand` finds the best enemy within `max(TrackingRadius·MaxRadius, MaxRadius)`. If its unit's queue is empty, or holds only an attack on a dead entity, the weapon issues a queue-clearing Attack command at that enemy. All 39 retail users of the flag are aircraft.
5. **The engine** does none of this:
   - `order_attack` parks aircraft at weapon range and hangs there.
   - The bomb gate tests the distance to the *target* against BombDropThreshold, whose engine default is 25 where Moho's is 1.5.
   - Bombs get `MuzzleVelocity` (0), so they fall straight down.
   - Nothing issues auto-attacks.
6. **Main risk: the engine's flight model.** The engine turns aircraft kinematically at TurnSpeed rad/s with no lag. With that model, Moho's state machine circles the target too tightly (radius v/ω = 14 u for UEA0103) and never lines up a second run:
   - the plain kinematic model gives 1 salvo in 90 s;
   - a Moho-like PD yaw and velocity lag gives 2 to 5 salvos in 90 s.

   §6.3 and §7 cover this.

---

## 1. Moho: a Winged aircraft attacking (ground target and air target)

### 1.1 Pipeline

| Step | What happens | Citation |
|---|---|---|
| Attack task created | Sets `UNITSTATE_Attacking`; for `Air.CanFly`, `mIsGrounded = 0` [read] | FR unit/tasks/CUnitAttackTargetTask.cpp:379, :454-456 |
| Navigation goal | `Update()`: a grounded unit gets `SetWeaponGoal` (a weapon-radius box). A flier skips that branch and gets `SetPosGoalFromWorldPosition(target pos)` for a static target, or `SetDestUnit(entity)` for a mobile one. **It flies at the target itself.** [read] | CUnitAttackTargetTask.cpp:791-846 (793 grounded test, 802 static, 838 SetDestUnit) |
| Engage | `TaskTick` Processing: while `!TargetIsWithinWeaponAttackRange && !IsWithinHorizontalDistance(Air.EngageDistance)` it keeps navigating. Mobile targets are re-goaled when they move more than 2 u for fliers (10 u otherwise). [read] | CUnitAttackTargetTask.cpp:1283-1316 |
| Desired target | Once in range or within EngageDistance: `UpdateAttacker(&mTarget)` → `CAiAttackerImpl::SetDesiredTarget`. With no overcharge weapon (`mWeapon == nullptr`) the task returns `-2`, which stages the thread until an attacker or command event wakes it. [read] | CUnitAttackTargetTask.cpp:1338-1343; FR ai/CAiAttackerImpl.cpp:735-770; return codes per memory `moho-task-timing` |
| Navigation is not aborted | In the Complete state only `CanFly == 0` units `AbortNavigation()`. A flier keeps its goal. [read] | CUnitAttackTargetTask.cpp:1350-1353 |
| Flight every tick | `MotionTick` calls `CalcMoveAir` every tick for every `Air.CanFly` unit, moving or not. [read] | FR unit/CUnitMotion.cpp:4415-4423 |
| Tactics | `CalcMoveAir` clears `MakingAttackRun`. If the attacker's desired target `HasTarget && CanAttackTarget && Air.Winged`, it runs `ComputeAirCombatTactics`, which **overwrites** the desired velocity. With no desired target it resets `mCombatState = None` and `mSustainedTurnTicks = 0`. [read] | CUnitMotion.cpp:4086, 4089-4101 |
| `CanAttackTarget` is not a range test | It checks only the target type, layer caps and the disabled flag. Tactics therefore keep running on loops far outside weapon range. [read] | FR unit/core/UnitWeapon.cpp:3335-3379; CAiAttackerImpl.cpp:800-820 |
| Weapon target while out of range | `CAcquireTargetTask` gives a `CanFly` unit's weapon the desired target even when it is too close or out of range. [read] | FR unit/tasks/CAcquireTargetTask.cpp:458-466 |
| Fire | `CFireWeaponTask::Execute`: `CanAttackTarget && UnitWeapon::CanFire && CheckSilo && !TargetIsTooClose` (TargetIsTooClose enforces MaxRadius). The fire clock is `10 / RateOfFire` ticks. [read] | FR unit/tasks/CFireWeaponTask.cpp:195-252 |
| End | Target dies → attacker event → task Complete or state 5. The destructor calls `attacker->Stop()` and `navigator->AbortMove()`. For a flier, `AbortMove` stops at `EstimateAirAbortStopPosition`. [read] | CUnitAttackTargetTask.cpp:857-920, 473-499; FR ai/CAiNavigatorAir.cpp:271-302 |

The combat state is persistent sim state. `mCombatState`, `mCombatStateTimeoutTick` and `mSustainedTurnTicks` are serialized at FR CUnitMotion.cpp:983-986 and 1069-1072, and initialised to 0 at :768-770 [read].

### 1.2 States

The enum is at FR ai/EAirCombatState.h:22-32: `0 None, 1 Combat, 2 NormalTurn, 3 CombatTurn, 4 CombatTurnB, 5 Realign, 6 BreakOff, 7 ReturnToMap`. Only Combat, NormalTurn and CombatTurn are IDA-confirmed names; the other five names are faf-re's proposals. The numeric values are confirmed (EAirCombatState.h:14-20) [read].

### 1.3 Per-tick decision: `ComputeAirCombatTactics`

FR CUnitMotion.cpp:3586-3807. The RNG is the sim's Mersenne stream. `RandomUniformIntRange(lo, hi)` is half-open, `lo + ((hi-lo)·u32) >> 32` (CUnitMotion.cpp:320-334). `FloorSecondsToTicks(s) = floor(s·10)` (FR math/GridPos.cpp:36-41).

Inputs, all [read]:

- `heading` is the airframe's horizontal forward, unit length (CalcMoveAir, CUnitMotion.cpp:~3832-3835).
- `targetPos = target.GetTargetPosGun()`.
- `d = targetPos − unitPos`, with `targetDist = |d|` in **3-D** and `horzDist = |d.xz|`.
- `maxAirSpeed = moveSpeedMult · Air.MaxAirspeed` (:3597).
- `breakOffTrigger = breakOffTriggerMult · Air.BreakOffTrigger`.
- `breakOffDist = breakOffDistanceMult · Air.BreakOffDistance` (:3616-3617). The mults come from Lua `SetBreakOffTriggerMult` and `SetBreakOffDistanceMult`.

```
prev := mCombatState
rollBreakOff():                                   // :3620-3625
    state := BreakOff
    lo := ceil(breakOffDist / maxAirSpeed * 10); hi := int(lo * Air.RandomBreakOffDistanceMult)
    timeout := tick + RandomUniformIntRange(lo, hi)            // [lo, hi), ticks
if prev == ReturnToMap and !map.IsWithin(pos, 5): keep ReturnToMap        // :3628-3632
else:
    hdot := dot(normalize(d.xz), heading)
    aligned := hdot > (prev == CombatTurn ? 0.0 : 0.866)                  // :3643-3644
    if target is on LAYER_Air and !map.IsWithin(pos, 0): state := ReturnToMap; done   // :3646-3652
    if (prev == Combat and breakOffTrigger > targetDist)                  // 3-D!   :3656
       or (Air.BreakOffIfNearNewTarget and prev == None and breakOffDist > horzDist)  // :3658
       or (prev == NormalTurn and hdot < 0)                               // :3660
       or mSustainedTurnTicks > floor(Air.SustainedTurnThreshold * 10):   // :3664-3669
        rollBreakOff(); done
    timeoutActive := timeout >= tick                                      // :3678
    if timeoutActive and (!aligned or prev == BreakOff): keep state       // :3679-3680
    elif !aligned:
        roll := true
        if prev == NormalTurn:      // (the timeoutActive arm is unreachable here)
            if dot(normalize(d), heading) >= 0: roll := false             // :3683-3690
        if roll:
            state := RandomUniformIntRange(CombatTurn, BreakOff)          // {3,4,5}   :3694
            timeout := tick + RandomUniformIntRange(floor(RandomMin..*10), floor(RandomMax..*10))  // :3695-3697
    else:
        state := Combat                                                   // :3700
        if target on LAYER_Air and it moved this tick
           and dot(target forward, heading) > 0: state := NormalTurn      // :3701-3716 (chasing its tail)
```

Outputs by state (`switch` at :3721-3806) [read]:

| State | MakingAttackRun | Desired direction | Speed | Turn-time effects |
|---|---|---|---|---|
| Combat (1), NormalTurn (2) | **set** (:3724) | At the target. If the target is mobile, at `PredictAheadBomb(precision)`, where precision = `Air.PredictAheadForBombDrop` if > 0 and the target is not air, else 1.0 (:3726-3746) | `maxAirSpeed`. NormalTurn against a *moving* air target uses `max(MinAirspeed, targetDist)` (:3750-3757; faf-re's text reads it the other way; a retail probe's interceptor trails a bomber at its distance) | Turn gain `+= wingBlend` (CalcWingedOrientation :3256-3257) |
| CombatTurn (3), CombatTurnB (4) | – | At the target | `Air.MinAirspeed` (not multiplied by moveSpeedMult) (:3761-3770) | `++mSustainedTurnTicks`. **CombatTurn only**: yaw clamp is `CombatTurnSpeed` (:3161) and turn gain `+= TightTurnMultiplier·wingBlend` (:3254-3255) |
| Realign (5) | – | At the target | `maxAirSpeed` (:3772-3780) | `++mSustainedTurnTicks` |
| BreakOff (6) | **set** (:3783) | **Straight on** (current heading) | `maxAirSpeed` (:3782-3790) | `mSustainedTurnTicks = 0` |
| ReturnToMap (7) | – | Map centre | `maxAirSpeed` (:3792-3802) | `mSustainedTurnTicks = 0` |

**Altitude** comes from `CalcDesiredTargetElevation` (CUnitMotion.cpp:3008-3051) [read]:

- It samples the terrain at `unitPos + outDesiredVelocity`, which in Combat is the target's own position.
- **Air target:** the target unit's `Physics.Elevation` plus that terrain sample. The result is clamped to at least `0.5 · own Elevation + terrain` (`kAirTargetMinimumElevationScale = 0.5`, :69; :3027-3039).
- **Ground target:** `Physics.AttackElevation` in **Combat only**, and `Physics.Elevation` otherwise (:3049). `AttackElevation` defaults to `Elevation` when it is 0 (FR resource/blueprints/RUnitBlueprint.cpp:665-667).

### 1.4 How the airframe follows: the control law

[read] for the formulas, [inferred] for their effective behaviour.

- **Yaw.** `CalcWingedOrientation` clamps the per-tick yaw step to `maxTurnSpeed·0.1`, where maxTurnSpeed is CombatTurnSpeed in CombatTurn and TurnSpeed otherwise. It then builds the *desired* orientation **10×** that step ahead (:3161-3175). The desired nose therefore leads the real one by at most TurnSpeed **radians**.
- **Torque.** `ComputeAirControl` applies a PD torque: `turnGain·rotationError + KTurnDamping·(−ω)` (:3534-3535). Here `turnGain = KTurn / transportLoadFactor` (:3417) plus the wingBlend increments listed above.
- **Thrust.** It is along the **current nose**: `heading · limitedSpeed · forceScale`. In states ≤ NormalTurn, `forceScale = max(dot(desired, heading), 0.5)` (:3097-3130). Acceleration is `KMove·thrust + movementDamping·(−v)` (:3519-3523).

[inferred] Per unit inertia this reduces to:

- `ω' = KTurn_eff·err − KTurnDamping·ω`, with `err ≤ TurnSpeed`;
- `v' = KMove·s·f·nose − damp·v`, with damp ≈ KMove at top speed (CalcAirMovementDampingFactor, :2964-2995).

For UEA0103 (KTurn 0.7, KTurnDamping 1) the steady yaw rate is about 0.49 rad/s, with a 1 s lag on both yaw and velocity. **This is not the engine's "TurnSpeed rad/s, instantly".** faf-re's air physics still carries TEMPORARY PROBE blocks for spin and runaway bugs (CUnitMotion.cpp:4243-4300), so treat its exact flight paths as unverified [doubt].

### 1.5 What it looks like

[inferred, model in Appendix A]

**UEA0103 against a structure.** The relevant blueprint values are in BP UEA0103_unit.bp `Air` and `Physics`:

- MaxAirspeed = MinAirspeed = 10, TurnSpeed = CombatTurnSpeed = 0.7, TightTurnMultiplier 0;
- EngageDistance 50, BreakOffTrigger 20, BreakOffDistance 18, BreakOffIfNearNewTarget true;
- PredictAheadForBombDrop 3, Elevation 18, with AttackElevation defaulting to 18.

The sequence:

1. It flies at the target, on the navigator, in state None.
2. At 50 u the desired target is set. It is aligned, so it enters **Combat**, which raises MakingAttackRun.
3. It releases at about 27 u out (§3).
4. It continues until the **3-D** distance is below 20. With about 17 u of height difference, that is about 10 u horizontal.
5. **BreakOff**: it flies straight for a random 18 to 26 ticks.
6. Then, if the target is not ahead, it rolls one of {CombatTurn, CombatTurnB, Realign} with a timeout of 30 to 59 ticks.
7. CombatTurn returns to Combat as soon as the target is in the front hemisphere (threshold 0). CombatTurnB and Realign need it within 30°. When the timeout expires unaligned, it re-rolls.
8. More than 100 cumulative turning ticks force another BreakOff.

**UEA0102 against an air target.** MaxAirspeed 15, MinAirspeed 10, TurnSpeed 1.5, TightTurnMultiplier 1.02, BreakOffTrigger 15, BreakOffDistance 5, EngageDistance 50, and no BreakOffIfNearNewTarget.

- It is the same machine, with BreakOff lasting 4 to 5 ticks.
- Chasing a moving target that faces the same way puts it in **NormalTurn**. NormalTurn breaks off only when the target falls behind (`hdot < 0`).
- It leads the target by 1 s (PredictAheadBomb with precision 1).
- Its altitude follows the target's cruise altitude.
- It returns to the map if it leaves.

### 1.6 Non-Winged fliers (gunships), for completeness

`ComputeAirControl` picks the "hover family" when `!Air.Winged` (:3431-3453).

- With a target it calls `CalcCirclingOrientation` (:3266-3381). This circles at `weapon MaxRadius × U[CirclingRadiusChangeMinRatio 0.6, Max 0.9)`, times `CirclingRadiusVsAirMult` against air.
- Altitude is `AttackElevation ± U(CirclingElevationChangeRatio 0.25 · AttackElevation)`.
- The direction flips randomly when `CirclingDirChange` is set. These parameters are re-rolled every `[CirclingFlightChangeFrequency·10, ·20)` ticks.
- `turnGain *= CirclingTurnMult` (3.0) (:3449).

**This is out of scope here.** The engine's park-at-range roughly stands in for it.

---

## 2. (merged into §1)

---

## 3. Moho's bomb release

### 3.1 `UnitWeapon::CanFire`, Winged branch

FR unit/core/UnitWeapon.cpp:3111-3251 [read]. The checks run in this order:

1. The unit is not stunned or `Busy`, and a CanFly unit must be on `LAYER_Air` (:3123-3127). `NeedUnpack` and AboveWater/BelowWater checks follow (:3129-3148).
2. `!Air.Winged` → `return mCanFire` (:3150-3152).
3. **Speed gate** for `AutoInitiateAttackCommand` weapons: `|vel per tick|·10 ≥ moveSpeedMult · MaxAirspeed · 0.25`, else false (:3154-3169).
4. `!NeedToComputeBombDrop` or no target → `return mCanFire` (:3171-3173).
5. Not `UNITSTATE_MakingAttackRun` → false (:3175-3179).
6. Target position: `GetTargetPosGun`. If `Air.PredictAheadForBombDrop > 0` and the target is mobile, use `PredictAheadBomb(PredictAheadForBombDrop)` instead (:3181-3191).
7. `CalcBombDrop(targetPos)` gives the release point. If it is invalid (NaN), return false (:3192-3196).
8. `dist = |release.xz − unit.xz|` (:3198-3201).
9. The threshold comparisons. See §3.3, and note the [doubt] there.
10. Heading checks. `forward` is the airframe's +Z axis, and both dot products use only the XZ components and are not normalized:
    - `releaseDot = (release − unit)·forward`; if it is > 0, return false (:3229-3238).
    - `targetDot = (target − unit)·forward`; if it is < 0.866, return false (:3240-3244). `kBombDropHeadingDotThreshold` is 0.866 at :372.

    Otherwise `return mCanFire`.

### 3.2 `CalcBombDrop`

FR unit/core/Unit.cpp:14478-14492 and helper :1975-2043 [read].

- `v = Entity::GetVelocity() · 10`, which is per second. `GetVelocity` is `(cur − last) · scale`, per tick (FR entity/Entity.cpp:2809-2816).
- `g` is the sim gravity. `up = −g/|g|`.
- `h = (unit − target)·up` is the height above the target. `vy = |v·up|`. Note that this is a **magnitude**, so the sign is lost.
- `disc = (h ≥ 0 ? 2 : −2)·|h|·|g| + vy²`. If `disc < 0`, the result is NaN.
- `t = (vy − √disc)/|g|`. If `t < 0`, `t = (vy + √disc)/|g|`. If it is still `< 0`, the result is NaN.
- `release = target − (g·t²/2 + v·t)`.

In the horizontal plane this is `release.xz = target.xz − v.xz·t`. It is the point where the aircraft must be for a bomb dropped now, carrying the aircraft's velocity, to land on the target.

For UEA0103 at 18 u and 10 u/s: `t = √(36/4.9) = 2.71 s`, so the release point is **27.1 u** short of the target.

### 3.3 The threshold comparisons, a faf-re inversion [doubt]

faf-re as written (UnitWeapon.cpp:3211-3227):

```
if (BombDropThreshold >= 2*dist) return false;   // "too close"
if (BombDropThreshold <  dist)   return mCanFire; // far: fire, no heading check
// thr/2 < dist <= thr: heading checks (3.1 step 10)
```

This fires whenever the aircraft is *farther* than the threshold from the release point, and refuses exactly *at* the release point. faf-re's own probe comment on that arm reads: "this arm returns WITHOUT any heading check … If the reported 'bombs in any direction' is this arm …" (:3218-3221). The file also carries a BOMBDIAG sink (:376-397).

The inferred correct reading [inferred] swaps the two outer results:

```
if (dist > BombDropThreshold)       return false;     // not at the release point yet
if (2*dist <= BombDropThreshold)    return mCanFire;  // right on it
// thr/2 < dist <= thr: fire only if the release point is behind or abeam
// (releaseDot <= 0) and the target is still ahead (targetDot >= 0.866)
```

All three arms are reachable and meaningful under this reading.

Evidence: the Appendix A model, with UEA0103 starting 60 u out on a straight approach.

| Reading | Fires at | First salvo lands (distance from target) |
|---|---|---|
| Inferred | 27.0 u | 1, 3, 5, 7, 9 u: a carpet across and beyond the target |
| faf-re as written | 39 u (as soon as the target enters MaxRadius 40) | 11, 9, 7, 5, 3 u **short**, then again at 19 u and 1 u |

**Recommendation:** port the inferred reading. Add a test that bombs land on the target, and record the decision in memory so it is not "fixed" back to faf-re.

### 3.4 `PredictAheadBomb(precision)`

FR Unit.cpp:14423-14469 [read].

- It starts from `pos`. Each step rotates the per-tick velocity about +Y by `impulse.y · 0.1` rad, where `impulse` is the physics body's angular velocity. It then adds the velocity's XZ, for `precision·10` steps, with the last step pro-rata. Y is unchanged.
- Every mobile unit gets a physics body in the CUnitMotion constructor (CUnitMotion.cpp:843). For land units the angular term is presumably 0 [inferred], so the prediction is `pos.xz + v_per_s.xz · precision`.
- With PredictAheadForBombDrop 3, the prediction covers 3 s, which is about the bomb's fall time.

### 3.5 Bomb initial velocity

FR projectile/Projectile.cpp:498-548, in the projectile constructor [read].

If the projectile blueprint has `Physics.RealisticOrdinance` and there is a launcher:

- `mVelocity = launcher.GetVelocity() · 10`, which is per second (:500-506).
- If the projectile has a target and the launcher is a unit:
  1. Take the aim point: `GetTargetPosGun`, or `PredictAheadBomb(launcher's PredictAheadForBombDrop)` if that is > 0 and the target is mobile (:508-521).
  2. Set the horizontal velocity to point from the **launcher** at the aim point, with **length = |inherited 3-D velocity|**.
  3. **y becomes 0.** `mVelocity.y += (0 − mVelocity.y)` at :538. faf-re's comment "Y lane is left untouched" contradicts its own code.
- `jitter = mAABox.Min.z`. If it is > 0, ±jitter is added to x and z (:544-548). The field identification looks dubious [doubt]. For a box centred on the projectile this value is ≤ 0, so the term is a no-op. Skip it.

Otherwise, without RealisticOrdinance, the velocity is the launch orientation's forward times a random InitialSpeed (:553-580).

**FiringRandomness only jitters the launch *orientation*** (FR UnitWeapon.cpp:4013-4026). The RealisticOrdinance branch does not use that orientation, so FiringRandomness does not change a bomb's path. With MuzzleVelocity 0, the post-create rescale is skipped (:4049-4062).

Retail projectiles with RealisticOrdinance: 13, all of them bombs. The list is AIFBombGraviton01, AIFBombQuark01, CIFNeutronClusterBomb01/03, CIFProtonBomb01, SBOInferno…, SBOOhwalli…, SBOOtheTacticalBomb01/02, SBOZhanaseeBomb01, TIFNapalmCarpetBomb01/02 and TIFSmallYieldNuclearBomb01. None of them sets `UseGravity`, so they fall at the default (memory `moho-projectile-semantics`).

The salvo itself is Lua's. UEA0103 has `MuzzleSalvoSize 5`, `MuzzleSalvoDelay 0.2` and `RateOfFire 0.5`, which is a 20-tick fire clock. Each bomb takes the launcher's position and velocity at the moment it leaves, so the carpet spreads about 2 u per bomb along the run.

### 3.6 Blueprint defaults that matter

FR resource/blueprints/RUnitBlueprint.cpp [read].

- **Weapon:** `NeedToComputeBombDrop 0`, `BombDropThreshold 1.5`, `AutoInitiateAttackCommand 0`, `TrackingRadius 1.0`, `TargetCheckInterval 3.0` (:1024-1078).
- **Projectile:** `RealisticOrdinance 0` (FR resource/blueprints/RProjectileBlueprint.cpp:144).
- **Air** (:370-414):

| Field | Default | Field | Default |
|---|---|---|---|
| Winged | 0 | MinAirspeed | 0, then → MaxAirspeed |
| TurnSpeed | 1.0 | CombatTurnSpeed | 1.0 |
| StartTurnDistance | 0, then → SizeZ·3 | TightTurnMultiplier | 1.0 |
| SustainedTurnThreshold | 10 | EngageDistance | 0 |
| BreakOffTrigger | 0 | BreakOffDistance | 0 |
| BreakOffIfNearNewTarget | 0 | KMove | 1 |
| KMoveDamping | 1 | KTurn | 3 |
| KTurnDamping | 3 | RandomBreakOffDistanceMult | 1.5 |
| RandomMinChangeCombatStateTime | 3 | RandomMaxChangeCombatStateTime | 6 |
| PredictAheadForBombDrop | 0 | | |

- **Derived** in `OnInitBlueprint` (:764-775):
  - CanFly when MotionType is Air;
  - MaxAirspeed → Physics.MaxSpeed when 0;
  - MinAirspeed → MaxAirspeed when 0;
  - StartTurnDistance → SizeZ·3 when 0.
- Moho's Air blueprint has **no** `AccelerateRate` field (ctor :370-414).

---

## 4. Moho's idle auto-engage (`AutoInitiateAttackCommand`)

**`CAcquireTargetTask::CheckAutoInitiate`** (FR unit/tasks/CAcquireTargetTask.cpp:873-908) [read]. It returns true only for a flagged weapon, and only when:

- the unit has **no current command**; or
- the current command is the **only** one, is `Attack` or `FormAttack`, has an entity target, and that entity is null, dead or destroy-queued.

**`CAcquireTargetTask::Execute`** (:357-612) [read] runs these steps in order:

1. Reschedule every `ceil(TargetCheckInterval·10) + 1` ticks, which waits `ceil(TCI·10)` ticks.
2. `ManualFire` → done. A unit being built, or a disabled weapon, waits. `StopOnPrimaryWeaponBusy` and `PrefersPrimaryWeaponTarget` are handled.
3. Evaluate the attacker's desired target and set its state. CanFly units bypass the range check (:458-466).
4. If `Attacking && ((flag && !CheckAutoInitiate()) || NeedUnpack || CanTarget)` → wait (:488-492).
5. Keep a still-valid target (:495-504). Hold fire clears the target and waits (:507-514).
6. `searchRadius = max(TrackingRadius·MaxRadius, MaxRadius)` (:550-556).
7. `FindBestEnemy` (:578) → if there is none, fall back to the desired target or clear (:579-588).
8. `!CheckAutoInitiate()` → just `SetTarget(best)` (:590-595).
9. Otherwise `SetTarget(best)` and **`IssueCommandToSelectedUnits(sim, {unit}, Attack(entity best), clearQueue = true)`** (:597-611).

FindBestEnemy differences for flagged weapons (FR ai/CAiAttackerImpl.cpp) [read]:

- It does **not** skip candidates that are inside MinRadius or have no firing solution, for mobile units (:1026-1035).
- It does **not** apply the ×4 penalty to out-of-solution candidates (:1059-1067).
- It considers only enemy recon blips, skipping BENIGN, attached or being-built air units, blacklisted and DoNotTarget units, and ground units off the playable rect. Target priorities decide, then distance.

When the desired target is cleared, flagged weapons are rescheduled to 2 ticks (CAiAttackerImpl.cpp:381-388).

Users of the flag, from a survey of BP: **39 units, all `RULEUMT_Air`**. 26 are Winged; 13 are not (UAA0203, UAA0310, UEA0104, UEA0203, UEA0305, URA0203, URA0401, XAA0305, XEA0002, XRA0105, XRA0305, XSA0104, XSA0203).

Idle bombers therefore auto-attack ground units within 50 u (UEA0103: 1.25 × 40). Idle UEA0102 fighters attack air units within 31.25 u, with TargetCheckInterval 0.3, which is a 3-tick cadence.

---

## 5. The engine today

| Area | Behaviour | Citation |
|---|---|---|
| Attack order | Finds the longest enabled weapon range (40 for the UEA0103 bomb). Beyond it, it navigates to the target; inside it, it calls `navigator_.abort_move()` and returns Hold. An aircraft with an idle navigator is never moved again, so **it hangs in place**. `target_id == 0` (attack-ground) pops at once. | EN sim/unit_orders.cpp:332-371 (335-338, 343-350, 358-368) |
| Weapon target | The attack order's target comes first if `can_target`. `can_target` requires `dist ≤ MaxRadius·max(1, TrackingRadius)`, so on a long loop the target is dropped and retaken, which **churns OnLostTarget/OnGotTarget**. | EN sim/weapon.cpp:291-307, 232-280 (264-265); target-mark callbacks at 199-221 |
| Bomb gate | `can_fire` and `try_fire`: **horizontal distance to the target** ≤ `bomb_drop_threshold`. No release point, no MakingAttackRun, no speed gate. | EN sim/weapon.cpp:113-117, 431-438 |
| Threshold default | 25, against Moho's 1.5 | EN sim/weapon.hpp:79; loaded at EN lua/sim_bindings.cpp:534-546 |
| Bomb velocity | `need_compute_bomb_drop` → horizontal × `muzzle_velocity`, which is 0 for UEA0103, so the bomb **falls straight down**. FiringRandomness is applied to bombs too. `target_entity_id = 0`; lifetime 10 s; gravity defaults on. | EN sim/weapon.cpp:529-537, 564-565, 578, 588-591, 618-620 |
| Lua bombs | `weapon:CreateProjectile` → the same `Weapon::launch` | EN lua/bindings/sim/weapon.cpp:232-262 |
| Projectile blueprint | No `RealisticOrdinance` read | EN sim/projectile.cpp:349-399; sim/projectile.hpp:97-103 (GRAVITY 4.9 at :48) |
| Auto-engage | None. `update_targeting` only picks a weapon target within reach; `AutoInitiateAttackCommand` is never read. | EN sim/weapon.cpp:309-355 (grep finds no reader) |
| Flight model | Kinematic. Yaw toward the waypoint at `turn_rate_rad·turn_mult` rad/s with no lag; speed ramps to `max_airspeed·speed_mult` at `accel_rate`; altitude ramps to `elevation_target_` at 5 u/s over `air_floor`; clamped to the playable area. | EN sim/navigator.cpp:408-526 (435, 456-464, 471-480, 489) |
| Air blueprint read | MaxAirspeed, TurnSpeed (rad/s), FlyInWater and AccelerateRate. Fallbacks: accel = 0.5·MaxAirspeed; turn = 1.5 (Moho's default is 1.0). **Not read:** Winged, MinAirspeed, CombatTurnSpeed, Engage/BreakOff fields, PredictAheadForBombDrop, AttackElevation, K* gains. | EN lua/sim_bindings.cpp:1002-1049 |
| Break-off mults | `SetBreakOffTriggerMult` and `SetBreakOffDistanceMult` are stored and saved, but **unused** | EN lua/bindings/sim/unit.cpp:1667-1677; sim/unit.hpp:559-562, 1364-1365; sim/state_io_entities.cpp:1043-1044 |
| Velocity | `Unit::velocity()` = last tick's displacement per second, set after each unit update | EN sim/sim_state.cpp:1463-1475; sim/unit.hpp:628-632 |
| Unit states | "Attacking" is derived from the queue head. Other states live in a string set that is saved. | EN lua/bindings/sim/unit.cpp:241-243, 257; sim/unit.hpp:530-533; state_io_entities.cpp:927 |
| Separation | Air boids only while `navigator_.is_moving()` | EN sim/unit.cpp:563-590 |
| Existing test | `--air-turn-test` asserts exactly 0.15 and 0.07 rad/tick turns, which pins the lag-free model | tests/integration/runner/integration_tests.cpp:12655-12730; registered in test_modes.cpp:194 and data_tests.cmake:31 |

---

## 6. Port plan (minimal, in engine terms)

### 6.1 Blueprint fields to read, with Moho defaults

- **Unit Air fields.** Read these into a new `AirCombatRules` on `Unit`, next to `StagingRules`, at EN lua/sim_bindings.cpp:1002-1049. Read them only for `is_air_unit()`.

| Field | Default | Note |
|---|---|---|
| `Air.Winged` | false | |
| `Air.MinAirspeed` | → MaxAirspeed when 0 | |
| `Air.CombatTurnSpeed` | 1.0 | rad/s |
| `Air.TightTurnMultiplier` | 1.0 | |
| `Air.SustainedTurnThreshold` | 10 | |
| `Air.EngageDistance` | 0 | |
| `Air.BreakOffTrigger` | 0 | |
| `Air.BreakOffDistance` | 0 | |
| `Air.BreakOffIfNearNewTarget` | false | |
| `Air.RandomBreakOffDistanceMult` | 1.5 | |
| `Air.RandomMinChangeCombatStateTime` | 3 | |
| `Air.RandomMaxChangeCombatStateTime` | 6 | |
| `Air.PredictAheadForBombDrop` | 0 | |
| `Air.KTurn` | 3 | only with §6.3b |
| `Air.KTurnDamping` | 3 | only with §6.3b |
| `Air.KMove` | 1 | only with §6.3b |
| `Air.KMoveDamping` | 1 | only with §6.3b |
| `Physics.AttackElevation` | → Elevation when 0 | |

  Also consider changing the TurnSpeed fallback from 1.5 to Moho's 1.0. That is a separate, tiny parity fix.
- **Weapon:** `AutoInitiateAttackCommand` (default false), read at EN lua/sim_bindings.cpp near :534. Change the `bomb_drop_threshold` default at EN sim/weapon.hpp:79 from 25 to **1.5**.
- **Projectile:** `Physics.RealisticOrdinance` (default false). Add it to `Projectile::BlueprintPhysics` (EN sim/projectile.hpp:97-103) and read it in `apply_blueprint_physics` (EN sim/projectile.cpp:349-399).

### 6.2 New runtime state

All of it must be saved and deterministic.

- **`Unit::AirCombat air_combat_`.**
  - It holds `u8 state` (0..7, Moho's numbering), `u32 timeout_tick` (absolute; `SimState::tick_count()` is saved at EN sim/state_io.cpp:264/423) and `i32 sustained_turn_ticks`.
  - If §6.3b is taken, it also holds `f32 yaw_rate` and a `Vector3 air_velocity`.
  - `MakingAttackRun` goes in the existing `unit_states_` string set, via `set_unit_state("MakingAttackRun", …)`. It is cleared and set each tick in the order step, so Lua `IsUnitState('MakingAttackRun')` works, and it is already saved.
- **`UnitCommand::engaged`** (bool, runtime). Moho's "desired target is set" is sticky until the order ends. Save it in `StateIO::save/load(UnitCommand)` (EN sim/state_io_entities.cpp:296-358) like `approached`. Do not add it to the network codec.
- **`Weapon::auto_initiate_attack_command`** (a blueprint flag, saved like `need_compute_bomb_drop` at state_io_entities.cpp:463/523). Also a transient `auto_initiate_target`, which is set and consumed inside one unit update and need not be saved.
- **Save format.** Save and load `AirCombatRules`, `air_combat_` and the weapon flag in EN sim/state_io_entities.cpp next to the air block (:1058-1079 and the matching load), and **bump `kVersion`** (EN sim/state_io.cpp:27).
- **Determinism rules:**
  - All random draws go through `registry.sim_random().next_int(lo, hi − 1)`, which is half-open like Moho's. Guard `hi ≤ lo → lo` without drawing; Moho draws even then, but the engine's RNG is not bit-compatible anyway.
  - Use `osc::dmath` for atan2, sin and cos, as update_air does.
  - `std::sqrt`, `std::ceil` and `std::floor` are IEEE-exact. Implement `FloorSecondsToTicks` as `std::floor(s·10)`.
  - Draw in unit-update order, which is registry id order. No hash-map iteration.

### 6.3 The flight step

**(a) Required.** Split `Navigator::update_air` (EN sim/navigator.cpp:408-526) into two parts:

- a waypoint wrapper;
- a new `fly_air_step(Unit&, dt, terrain, Vector3 desired_dir_xz, f32 target_speed, f32 turn_rate, f32 target_alt_abs)` holding steps 1-8.

update_air then calls `fly_air_step(dir to waypoint, max_airspeed·speed_mult, turn_rate_rad·turn_mult, floor+elevation_target_)`. Existing behaviour and `--air-turn-test` stay unchanged.

**(b) Strongly recommended for winged combat only.** Add the PD lag from §1.4, in the combat path only:

- `yaw_rate += (KTurn_eff·clamp(err, ±turn_rate) − KTurnDamping·yaw_rate)·dt`, with `KTurn_eff = KTurn + wingBlend` in Combat and NormalTurn, and `+ TightTurnMultiplier·wingBlend` in CombatTurn;
- velocity relaxes toward the nose: `v += (KMove·s·f·nose − KMove·v)·dt`.

Without this, the Appendix A model never sets up a second bomb run for UEA0103: 1 salvo in 90 s against 2 to 5 with the lag. Keeping it combat-only leaves `--air-turn-test` and all move orders untouched. This step is [inferred]: it approximates `ComputeAirControl`, it is not a port of it.

*Done (2026-10-10): replaced by `ComputeAirControl` itself. Attack runs and circling drive the attitude controller of ordinary flight; a retail probe's interceptor and bomber runs match tick by tick up to their first random draw.*

### 6.4 `order_attack` (EN sim/unit_orders.cpp:332-371)

Add a Winged branch ahead of the current range logic:

```cpp
if (is_air_unit() && air_rules_.winged && target_id != 0) {
    // CanAttackTarget: some enabled, non-death weapon can_target() it, ignoring range (Moho has no range test here)
    if (!cmd.engaged) {
        const bool in_weapon_range = /* in_firing_range of a weapon that can hit it */;
        if (in_weapon_range || horiz_dist < air_rules_.engage_distance) cmd.engaged = true;   // CUnitAttackTargetTask.cpp:1283-1287
        else { navigate to target->position() as today (no abort at range); return Hold; }
    }
    const AirSteer s = air_combat_step(*this, *target, ctx);   // §6.5; sets/clears MakingAttackRun
    navigator_.fly_air_step(*this, dt, ctx.terrain, s.dir, s.speed, s.turn_rate, s.altitude);
    return Hold;
}
```

Non-winged aircraft and ground units keep the current code.

**Resets.** Mirror Moho's no-target reset (CUnitMotion.cpp:4098-4101). When the head is not an Attack order, or the order pops, set `state = None` and `sustained = 0` and clear MakingAttackRun. Do this in Unit::update's head-change block (EN sim/unit.cpp:388-458), beside the ferry and refuel resets.

**Later.** Use the same entry point for attack-ground (`target_id == 0`, `target_pos`) once the engine supports that order. That gap is noted in memory `project-parity-triage-2026-10-05`.

### 6.5 New `src/sim/air_combat.{hpp,cpp}`

These are pure functions plus one stateful step, so they can be unit-tested:

- `AirSteer air_combat_step(Unit&, const Entity& target, SimContext&)`. This is the §1.3 pseudo-code verbatim. It reads `break_off_*_mult()`, `speed_mult()` (as moveSpeedMult) and `tick_count()`. It returns:
  - the desired horizontal direction: to the target, to its prediction, the current heading in BreakOff, or the map centre in ReturnToMap;
  - the speed, per the §1.3 table;
  - the turn rate: CombatTurnSpeed in state 3, TurnSpeed otherwise, times `turn_mult`;
  - the altitude, per CalcDesiredTargetElevation: absolute = terrain at the target + AttackElevation in Combat against ground, Elevation otherwise, and against air the target's Elevation clamped to at least 0.5·own;
  - `making_attack_run`.

  Moving-target and on-air tests:
  - "moved this tick" is `target.velocity() != 0` for units;
  - the target's forward is `quat_rotate(orientation, +Z)`;
  - "on air" is `layer() == "Air"`;
  - the map's IsWithin is `SimState` playable-rect containment.

  ReturnToMap is near-dead code in the engine because `fly_air_step` clamps to the playable area. Port it anyway; it is cheap.
- `std::optional<Vector3> calc_bomb_drop(Vector3 vel_per_s, Vector3 pos, Vector3 target, f32 g = Projectile::GRAVITY)` (§3.2).
- `Vector3 predict_ahead_bomb(const Unit& target, f32 precision)`: `pos.xz + velocity().xz·precision`, with no yaw term because the engine has no angular velocity (§3.4). Use precision = PredictAheadForBombDrop if > 0 and the target is not on Air, else 1.0. Apply it only when `target.is_unit() && is_mobile()`.
- `bool bomb_release_ok(…)`: the §3.3 inferred gate plus the §3.1 heading checks.

### 6.6 Weapon changes (EN sim/weapon.cpp)

- **`can_fire`** (:100-119) and **`try_fire`** (:417-438). Replace the "distance to target ≤ threshold" test with the Moho Winged gate:
  - owner is not Winged → no bomb gate (keep `can_fire` as it is);
  - `auto_initiate_attack_command && |owner.velocity()| < 0.25·max_airspeed·speed_mult` → false;
  - `need_compute_bomb_drop`:
    - require `has_unit_state("MakingAttackRun")`;
    - take the target position as `aim point` (collision centre), or `predict_ahead_bomb`;
    - require `calc_bomb_drop(owner.velocity(), owner.position(), at)` to be valid;
    - require `bomb_release_ok`.
- **`update_targeting`** (:291-307). For a `CanFly` owner, take the attack order's target **without** the reach check. Keep the layer, category and restriction checks. This matches CAcquireTargetTask.cpp:458-466 and stops the OnLostTarget/OnGotTarget churn during loops.
- **Auto-initiate.** In `update_targeting`, at the "new best target" point (:349-354), and only for `auto_initiate_attack_command`:
  - the owner must be mobile;
  - fire state must not be HoldFire, which already returns early at :162;
  - the owner's queue must be empty, or hold exactly one Attack order whose target is gone (CheckAutoInitiate).

  When all hold, set `auto_initiate_target = best_id`.
- **`launch`** (:504-630). After `apply_blueprint_physics`, if `physics.realistic_ordinance`:
  - `vel = owner.velocity()`, which is per second;
  - with a target or ground point, aim at it, or at `predict_ahead_bomb` for a mobile unit with PredictAheadForBombDrop > 0;
  - `vel = normalize((aim − owner.position()).xz) · |vel|` (3-D magnitude), with `vel.y = 0`;
  - skip FiringRandomness for these shots (§3.5);
  - re-face the projectile along `vel`, and keep gravity on (already the default, :618-620).

  `owner.velocity()` lags one tick, which is acceptable at constant speed.

### 6.7 Issuing the auto attack (EN sim/unit.cpp `tick_upkeep`, after the weapon loop at :~640)

If any weapon set `auto_initiate_target` and the unit is alive, build `UnitCommand{Attack, target_id}` and call `ctx.sim->route_command({id}, cmd, /*clear_existing=*/true)`.

Inside a tick `human_input_active_` is false, so `route_command` applies the command directly with `next_command_id()`. That keeps it deterministic and off the network (EN sim/sim_state.cpp:604-643). Then clear the field.

Do it once per unit per tick, taking the first weapon in index order.

### 6.8 Order of work (suggested PRs)

1. **Fields and defaults (no behaviour change).** Air, weapon and projectile reads; the threshold default of 1.5; save fields; kVersion bump; unit tests for the loaders.
2. **Bomb release and bomb velocity.**
   - Add `calc_bomb_drop`, `bomb_release_ok` and the RealisticOrdinance launch.
   - The gate needs MakingAttackRun, so this PR also raises the flag. Either ship it together with PR 3, or set the flag provisionally in order_attack.
3. **The tactics state machine and `fly_air_step`.** order_attack's winged branch, the resets and the integration tests. Include §6.3b, or show with the test that re-runs fail without it.
4. **Auto-initiate**, with its tests.

### 6.9 Tests

**Unit tests, new `tests/test_air_combat.cpp`, pure functions:**

- `calc_bomb_drop((10,0,0), (0,18,0), (40,0,0))` → `release.x = 40 − 10·√(36/4.9) = 12.895` (±1e-3). Add a vy ≠ 0 case (|vy| semantics) and a disc < 0 case → nullopt.
- `bomb_release_ok` with threshold 3:
  - dist 3.1 → false;
  - dist 1.4 → true;
  - dist 2.0 with the release point behind and the target 5 u ahead → true;
  - dist 2.0 with the release point ahead → false;
  - dist 2.0 with the target 0.5 u ahead → false.
- Tactics transitions with a seeded `SimRandom` and stubbed geometry:
  - None + aligned → Combat;
  - None + not aligned → state ∈ {3, 4, 5} and timeout − tick ∈ [30, 60);
  - Combat with 3-D distance < BreakOffTrigger → BreakOff with duration ∈ [ceil(BOD/MaxA·10), int(·1.5)), which is [18, 27) for UEA0103;
  - BreakOff holds until the timeout even when aligned;
  - sustained > 100 → BreakOff;
  - NormalTurn with hdot < 0 → BreakOff;
  - a moving air target facing the same way → NormalTurn;
  - BreakOffIfNearNewTarget from None within BOD → BreakOff.
- A StateIO round trip of `air_combat_`, `engaged`, MakingAttackRun and the new rules.

**Integration tests.** Register them in tests/integration/runner/test_modes.cpp (as at :194), integration_tests.hpp and data_tests.cmake:31.

**`--air-attack-run-test`:**

- Setup: UEA0103 (ARMY_1) 20 u over the ground at (300, 600), flying north. An ARMY_2 ueb1101 at (300.3, 680), which is 80 u ahead and so exercises the approach and engage phases. `IssueAttack({bomber}, target)`.
- Hook `TIFNapalmCarpetBomb01.OnImpact` to log impact positions, and the Bomb weapon's `OnFire` to count salvos and record the bomber's distance at release.
- Assertions:
  1. The first salvo comes within 120 ticks, released at 20 to 34 u horizontal from the target. The model gives 27.
  2. The first bomb lands within 3 u of the target centre. At least 2 of the 5 land within 6 u, all of them within 12 u, and they lie along the run, spaced about 2 u.
  3. The bomber's horizontal speed stays ≥ 0.9·MinAirspeed on every tick after engaging. It never hangs.
  4. The bomber passes the target and gets more than 12 u beyond it, the break-off, then turns more than 90° within 100 ticks.
  5. At creation each bomb has `|v.xz|` within 10% of the bomber's speed, a bearing within 3° of the target, and `v.y = 0`.
  6. The target's health drops.
  7. With §6.3b in: at least 2 salvos within 600 ticks for seeds 1-4. The model gives 2 to 5 in 900 ticks with its own RNG, so allow up to 900 ticks or use the `--seed` the harness supports.

**`--air-auto-engage-test`:**

- An idle UEA0102 (ARMY_1) at altitude, and an ARMY_2 UEA0103 crossing 28 u away on a move order.
- Within 10 ticks the fighter's queue head is Attack on the bomber (`IsUnitState('Attacking')` and `GetTargetEntity`). Its distance to the bomber shrinks, and its weapon fires within 100 ticks.
- Negatives:
  - a fighter with `SetFireState(1)` (hold fire) stays idle;
  - a fighter on a Move order keeps its Move order. Its weapon may still shoot at targets in reach.
- An idle UEA0103 with an enemy tank 45 u away gets an Attack order, because the search radius is 1.25 × 40 = 50.

**Determinism.** Add a bomber attack run to the determinism and cross-OS scenario (tests/integration/determinism.py and cross_os_pairs.py). Add a snapshot taken mid-run, then restore and compare, to save_load.py. Use `--entity-trace` and `--rng-trace` when it diverges (memory `desync-hunting`).

---

## 7. Risks

1. **Determinism.**
   - Each engaged winged aircraft adds RNG draws: 1 per break-off and 2 per turn roll. They are deterministic, because they come in unit order inside the tick, but they **shift the RNG stream** for everything drawn later. Old replays and golden traces with aircraft will diverge, and so will any test that asserts RNG-dependent values. Saves are covered by the kVersion bump.
   - Auto-initiate consumes `next_command_id()`, so command ids change after the first auto-attack. Check tests that assert ids.
   - Floating point: only dmath trig plus IEEE sqrt, ceil and floor; no `std::lround` or `nearbyint`.
   - The combat state must be saved. Otherwise a restore mid-run picks different states (lpersist rules, memory `lua-persistence`).
2. **AI and gameplay.**
   - Bombers now really kill things. Retail and FAF AI that `IssueAttack` with bombers get much stronger, and the balance of AI-vs-AI tests and benchmarks shifts.
   - Idle aircraft of all 39 flagged types now auto-attack inside their search radius and **clear their queue**, though only when the queue is already empty. AI scripts that leave air platoons idle between orders see them wander off chasing targets. Moho does the same.
   - Lua `SetBreakOffTriggerMult` and `SetBreakOffDistanceMult` start to matter.
   - Gunships (13 flagged, not Winged) auto-attack into the existing park-at-range code, which hovers instead of circling (§1.6).
3. **Interplay with the existing air code.**
   - **Turn model.** This is the big one: see §6.3b and Appendix A. Applying the PD lag globally would break `--air-turn-test` (integration_tests.cpp:12716-12726) and change every air move. Scope it to combat first.
   - **Separation** runs only while the navigator is moving (unit.cpp:564). Engaged aircraft fly with the navigator idle, so groups of bombers will stack on one line unless `fly_air_step` callers also separate.
   - **Altitude reference.** Moho samples the terrain at the target in Combat; the engine uses `air_floor` under the aircraft. Convert in `fly_air_step`.
   - **After the order ends** the engine's idle aircraft freezes mid-air (no Moho stop point, no circling). A bomber that kills its target will visibly stop. This is a follow-up, not a regression.
   - **Formation attack orders.** `expand_group_command` gives each unit a slot position. The winged branch must use the target entity, not `target_pos`.
   - **Fuel.** `tick_fuel` keeps running; no change.
4. **Correctness of the source.**
   - The bomb-gate inversion (§3.3) is my inference against faf-re's text.
   - faf-re's air physics and bomb code still carry TEMPORARY PROBE diagnostics: BOMBDIAG at UnitWeapon.cpp:376-397, and AIRSPIN and AIRFLEE at CUnitMotion.cpp:4243-4300.
   - Five of the eight state names are guesses.
   - faf-re decompiles FAF's patched exe, so retail may differ in details.
5. **Performance.** O(1) per engaged winged unit per tick, plus a 30-step prediction loop. That loop is closed form in the engine. Auto-initiate reuses the existing `update_targeting` scan, so there is no new spatial query. The extra cost is more aircraft moving, which means more separation radius queries, as at unit.cpp:567. This is negligible next to Lua GC (memory `sim-profiling`).

## 8. Open questions

- **The bomb gate.** Confirm the arm swap against the binary at 0x006D4C80 if a disassembly is available, or keep the inferred version guarded by the impact test.
- **§6.3b.** Port the PD lag, or tune a simpler "turn-rate lag" until FA-like loops appear? The latter only needs one first-order yaw lag.
- **ReturnToMap.** Do we want it, or should the clamp stand in for it?
- **`mAABox.Min.z` jitter** in the projectile constructor (§3.5): what is the real field?

---

## Appendix A: model used for the numbers above

This is a Python model I ran during the research. The script was deleted afterwards; the essentials are below.

It runs the §1.3 tactics exactly, with UEA0103 parameters and a target at the origin 1 u up on flat ground. The bomber starts 60 u south, flying north. There is one tick of weapon-velocity lag, as in the engine. A salvo is 5 bombs 2 ticks apart, re-armed after 20 ticks. Bombs inherit velocity, are steered at the target, and fall under g = 4.9.

**Airframe A: the engine today.** Heading moves toward the desired direction by at most `rate·0.1` per tick, and speed ramps at 5 u/s².

| TurnSpeed (rad/s) | 0.7 | 0.6 | 0.5 | 0.45 | 0.4 | 0.35 |
|---|---|---|---|---|---|---|
| Salvos in 90 s (6 seeds) | 1 | 1 | 1 | 1-2 | 1-2 | 1-2 |

The bomber circles the target at 14 to 35 u and is never aligned beyond the 27 u release point again.

**Airframe B: Moho-like (§1.4).** Per tick:

```
err = clamp(wrap(atan2(d) - h), ±TurnSpeed)
w  += (KTurn_eff*err - KTurnDamping*w)*0.1
h  += w*0.1
v  += (KMove*s*f*nose - KMove*v)*0.1
```

with KTurn 0.7, KTurnDamping 1, `f = max(dot, 0.5)` in states ≤ 2 and 1 otherwise, and `KTurn_eff = KTurn + wingBlend` in states 1 and 2.

| | Seed 0 | Seed 1 | Seed 2 | Seed 3 | Seed 4 | Seed 5 |
|---|---|---|---|---|---|---|
| Salvos in 90 s | 2 | 3 | 3 | 4 | 2 | 5 |

Salvos come at ticks 33, then about 180, 330, 480, 620 and 775, each released 24.8 to 27 u out. About half of all bombs land within 6 u.

**The two gate readings** (airframe A, first pass):

| Reading | Releases at | Bombs land |
|---|---|---|
| Inferred | tick 33, 27.0 u | 1, 3, 5, 7, 9 u from the target |
| faf-re literal | tick 21, 39 u | 11, 9, 7, 5, 3 u (short), then further salvos at 19 u and 1 u |

---

## 9. What landed (PR 3a)

- **The flight.** `sim/air_combat.{hpp,cpp}` has `air_tactics`, which is §1.3 rule for rule, plus `calc_bomb_drop`, `bomb_release_ok` (the §3.3 swapped reading), `predict_ahead` and `fly_attack_run`.
  - `fly_attack_run` gives the airframe the §6.3b lag in a run only: the yaw rate is under KTurn and KTurnDamping, raised by wingBlend = 1 − cos(off the nose), and the velocity eases toward the nose under KMove.
  - Height is held over the ground at the target (CalcDesiredTargetElevation), climbing at the unit's climb rate.
  - Plain flight is untouched.
- **The order.** `Unit::order_attack` has a winged branch: it engages within a weapon's reach or EngageDistance (`UnitCommand::engaged`), then flies the run.
  - `Unit::tick_orders` ends the run when the order goes.
- **The weapon.**
  - `Weapon::bomb_ready` gates a winged aircraft's bombs (§3.1 and §3.3).
  - RealisticOrdinance bombs leave as §3.5 says.
  - An Air-layer owner's weapons keep the ordered target out of reach.
  - BombDropThreshold defaults to 1.5.
- **State.** `AirCombatRules` comes from the blueprint, `AirCombatState` is the run, and `engaged` is on the order. All of it is saved (snapshot version 8), and the run is in the units checksum while one is under way.
- **Tests.**
  - `tests/test_air_combat.cpp`: the release point, the gate and the tactics transitions.
  - `--air-attack-run-test` (gate): a UEA0103 against a power generator 100 u out releases 27.5 u short, puts 4 of its first 5 bombs within 6 u of the target, takes it from 600 to 120 health, never stops, and makes 4 runs in 900 ticks.

**Left:**
- PR 3b: idle auto-engage (§4) and the AutoInitiateAttackCommand speed gate (§3.1 step 3).
- Gunship circling (§1.6).
- Separation during runs.
- Moho's stop point when the order ends.
- The attack-ground order.
