# The sim tick in Moho's order: research and step plan

Design, 2026-10-18. The engine updates each entity whole, one after another, and writes positions at once.
Moho runs a tick as phases over all entities: army work (economy, navigators, weapons), then every unit's
orders, then every Lua thread, then every entity's motion and pose, then recon, then all moves are committed
at once. Small timing differences between the two (a shot one tick late, aim taken from the pose before the
body turned, an order run a tick early) appear all over the game and are hard to chase one at a time. This
plan reproduces Moho's sequence step by step.

Path prefixes used below:

- **FR:** faf-re's `src/sdk/moho/` (Draiget/faf-re, the decompiled FAF build of Moho)
- **EN:** this repository's `src/`, as of d98ad203 (line numbers drift as the port lands)

Certainty tags:

- **[read]** — read directly from faf-re code.
- **[inferred]** — reasoned from faf-re code.
- **[probe]** — measured in the retail game with a Lua probe.

---

## 0. Summary

1. **Moho's tick is phased.** `Sim::AdvanceBeat` runs the armies (economy, then the AI brain's navigators,
   steering and attackers), then the units' order threads, then the Lua threads, then every entity's
   `MotionTick`, then recon, kill cleanup and `AdvanceCoords`, then the deletion queue (§1).
2. **Moho double-buffers transforms.** Motion writes a pending transform; every read in the tick sees the
   transform committed at the end of the last tick; `AdvanceCoords` commits all of them at once (§1.6).
3. **The engine interleaves.** `SimState::tick` runs every Lua thread first, then the economy, then each
   entity in id order; a unit's `Unit::update` poses its manipulators, runs its orders (which move it), then
   fires its weapons. Positions are written at once (§2).
4. **The visible effects** come from five roots (§3): aim and launch from different poses (D1), Lua running
   before weapons and orders (D2, D7–D9), orders and weapons inside one unit's update (D3, D12), no
   committed transforms (D5, D6) and immediate destruction (D11). Measured on retail's Corsair attacking a
   structure: 318 damage a salvo here, 756 with the pose moved after the motion, 763 in retail (§4).
5. **The plan** is eleven steps (§5), each a PR with a test that fails without it. Step 1 is small and
   gives the largest measured effect.

---

## 1. Moho

### 1.1 Before the tick, and beats

- The command stream for beat N is applied as it is decoded (`CDecoder::DecodeMessage`, FR
  misc/CDecoder.cpp:151-252), before `++mCurTick` (FR sim/Sim.cpp:10505): code run by a command (a
  `SimCallback`, `AbandonedByPlayer`) sees `GetGameTick()` as N−1. [read]
- `Sim::AdvanceBeat` runs every beat. The tick body (§1.2 steps 2-12) runs only when the game is neither
  paused nor over; the deletion queue, effect purge, checksum and Lua GC (steps 13-14) run every beat, paused
  or not. A pause requested by a command of beat N stops the tick of beat N. [read]
- `Sim::Sync` (FR sim/Sim.cpp:5357) runs after every beat: dirty entities, purging deleted ones, the `Sync`
  table. [read]

### 1.2 `Sim::AdvanceBeat` (FR sim/Sim.cpp:10460-10657)

1. `RulesUpdateLuaState`.
2. `++mCurTick`.
3. `UpdatePaths`: background rebuild of the path clusters.
4. Each live unit: `ClearBeatResourceAccumulators`.
5. **Army phase**, each army in index order, `CArmyImpl::OnTick` (FR sim/CArmyImpl.cpp:1707-1790):
   - stats pulse, no-rush timer, `SetCanSee`, `CleanUpPlatoons` (platoon `OnDestroy`);
   - `ProcessArmyEconomy` (FR sim/CEconomy.cpp:372);
   - stat triggers, only when the tick is past 10;
   - the influence map when `tick % 30` is the army's index; unit-cap stats;
   - the AI brain's three task stages in order: **navigators**, **steering**, **attackers** (each attacker
     runs its own stage: for each weapon its fire task, then its target-acquisition task);
   - the army's path queue budget (2500).
6. **Stage B**: every unit's order thread (`IAiCommandDispatchImpl` and the task stack on it).
7. **Lua stage**: every sim Lua thread.
8. **Stage A**: every entity's `Entity::TaskTick` → `MotionTick`, in creation order.
9. `RefreshBlips`.
10. Recon: army `tick % N` runs a full `ReconTick`, the others `ReconRefresh` (FR sim/Sim.cpp:10565-10585).
11. Effect manager tick; `UpdateFormationDb`.
12. `KillCleanup` for units killed this tick, then `AdvanceCoords` for every entity on the coordinate list
    (pending → current, collision cells, intel), then the sync filter's extra data.
13. The deletion queue: `OnDestroy` of everything `Destroy`ed this beat.
14. Effect purge, decal cleanup, checksum, debug overlays, a full Lua collection when `tick % 70 == 0`.

### 1.3 Task stages (FR task/CTaskThread.cpp)

- A thread is appended to its stage's list when created (:491-500); a stage runs in creation order.
- `CTaskStage::UserFrame` (:638-680) runs until its list is empty, so a thread created or woken into the
  stage during its frame runs in the same frame; one created into a stage still to run this tick runs this
  tick.
- A step's result (:362-447): `1` next tick; `n > 1` after n−1 ticks; `0` again at once; `-1` task done, the
  next task on the stack runs at once; `-2` parked until resumed; `-4` requeued to the end of this frame
  (an attached entity waiting for its parent).
- Waking (`TaskResume`, an event signalled) moves a thread to the end of its stage's list (:573-581).

### 1.4 What runs where [read]

| Stage | Threads | Created at |
|---|---|---|
| brain, first | navigators of the army's units | FR ai/CAiNavigatorImpl.cpp:1301; ticks: ai/CAiNavigatorLand.cpp:560, ai/CAiNavigatorAir.cpp:416 |
| brain, second | steering: waypoint → `MotionSetTarget`, collision prediction | FR ai/CAiSteeringImpl.cpp:982; tick :1627 |
| brain, third | attackers (`LAiAttackerImpl` runs the attacker's own stage) | FR ai/CAiAttackerImpl.cpp:516; ai/LAiAttackerImpl.cpp:31-35 |
| attacker stage | per weapon: `CFireWeaponTask` (made in `UnitWeapon`'s constructor), then `CAcquireTargetTask` | FR unit/core/UnitWeapon.cpp:2937-2939; ai/CAiAttackerImpl.cpp:594-614 |
| sim stage B | `IAiCommandDispatchImpl` | FR ai/IAiCommandDispatchImpl.cpp:830 |
| sim Lua stage | every Lua thread: the root state's task is this stage (FR sim/Sim.cpp:6864); `ForkThread` puts a thread in its caller's stage (FR task/CLuaTask.cpp:418-437) | — |
| sim stage A | each entity | FR entity/Entity.cpp:2192 |

`UnitWeapon::Fire` only calls the weapon script's `OnFire` (FR unit/core/UnitWeapon.cpp:3794-3807).
Retail's `ChangeState` forks the new state's `Main` (`lua/system/class.lua`), so `RackSalvoFiringState.Main`
runs in the Lua stage of the same tick and creates the projectile there; stage A moves it the same tick.

### 1.5 An entity in stage A

- `Entity::TaskTick` (FR entity/Entity.cpp:2674-2692): texture scroller; if attached and the parent has not
  run this tick, `-4`; else the pending transform follows the parent; `MotionTick`.
- `Unit::MotionTick` (FR unit/core/Unit.cpp:12689-12746): `SiloTick` → guard formation → info cache →
  `--mStunTicks` → `CUnitMotion::MotionTick` (FR unit/CUnitMotion.cpp:4335-4440: surface collision from
  the last move, fuel, layer, one `CalcMove*`, `MoveTo`) → economy events → `UpdateManipulators` on the
  **pending** transform → regeneration or decay → `HandleResourceManagement` (production into the army,
  `ResourceConsumed`) → pod assist → factory queue check.
- `Projectile::MotionTick` (FR projectile/Projectile.cpp:797-1036): a pending impact runs `Impact()`
  (`OnImpact`, damage) and returns; otherwise the step, `CheckCollision` (which only records the hit and
  places the pending transform at the impact point), then the lifetime test.
- A prop without a motor parks itself; `FallDown`/`SinkAway` set a motor and wake it to the end of the
  stage, so it moves the same tick (FR entity/Entity.cpp:3047-3080).
- Shields, collision beams and script entities are attached to their unit and run right after it.

### 1.6 Transforms and poses

- `GetPosition`/`GetTransform` return the committed transform (FR entity/Entity.cpp:3952-3960); motion sets
  the pending one (`SetPendingTransform`, :3006-3017); `AdvanceCoords` (:4001-4040) commits it, updates the
  collision cells and moves the intel handles.
- A bone's world transform comes from the pose built on the pending transform of the last `MotionTick`
  (FR unit/core/Unit.cpp:13565-13598), which is committed by the time the weapons fire.
- `CAimManipulator::Aim` (FR ai/CAimManipulator.cpp:963-1023) aims from the muzzle in the prior pose plus
  the unit's velocity for a tick, at the target point plus its velocity for a tick, and turns the bones in
  the new pose. The aim (stage A, tick N) and the launch (tick N+1) use the same pose.
- Every engine spatial query (target acquisition, `DamageArea`, projectile collision,
  `GetUnitsAroundPoint`) goes through `COGrid`, which holds committed positions; an entity destroyed this
  tick stays in it until it is purged in `Sync`.

### 1.7 A shot, tick by tick

- Tick N−1, stage A: the body moves, the pose is built, the aim is on target.
- Tick N: army phase — `CFireWeaponTask` → `OnFire` → `ChangeState` → `Main` forked; then this weapon's
  target acquisition. Lua stage — `Main` creates the projectile at the muzzle (pose of N−1). Stage A — the
  projectile moves once; the unit moves and builds pose N.
- Tick N+k: stage A finds the collision; the pending transform is the impact point.
- Tick N+k+1: stage A — `Impact` → `OnImpact`, damage.

---

## 2. The engine (EN, d98ad203)

`SimState::tick` (EN sim/sim_state.cpp):

1. `++tick`, a slice of the lazy Lua sweep.
2. `dispatch_due_commands` — after the tick count went up.
3. `note_tick_position` — each unit's position as the tick begins (a partial stand-in for committed
   transforms).
4. Path cluster background work; every army's path queue.
5. Platoon cleanup; influence maps.
6. **Every Lua thread** (`ThreadManager::resume_all`).
7. Economy of all armies, overflow sharing, stat triggers.
8. `update_entities`: `steer_ground_units`, then a snapshot of ids (entities made during the walk wait for
   the next tick); a unit — `collide_with_props`, `Unit::update`, velocity = (after − before)/dt; a projectile
   — `Projectile::update`; a prop — fall and sink. Then `separate_ground_units`.
9. Collision beams; ferry beacons; crashed aircraft landing (`OnImpact`).
10. No-rush; visibility and recon for every army; decal sight; influence feed; victory; attachments
    follow their parents; economy events.
11. Effects, Lua collection at `tick % 70`, collision cells advanced, garbage, the UI snapshot, checksums.

`Unit::update` (EN sim/unit.cpp): lifecycle → **`tick_manipulators`** (the pose from the transform before
this tick's motion) → wrong-layer damage, take-off → orders that ended are cleaned up → **`tick_orders`**
(the orders move the unit: `nav_update`, `fly_attack_run`, `fly_circling`, `hold_altitude`, `hover_low`, …)
→ **`tick_after_orders`** (more motion: facing the weapons' work, coasting, diving, circling, landing,
layer, cargo, fuel) → hover → **`tick_upkeep`** (stun, regeneration or decay, silo, motion events, then
**the weapons**: each acquires, aims and fires in one call).

A shot here: tick N — the pose (transform N−1), the order turns the body (transform N), `OnFire` forks
`Main`; tick N+1 — `resume_all` runs `Main`, which creates the projectile at the muzzle (transform N with
the pose of N−1); `update_entities` moves it; the tick of the collision deals the damage at once.

---

## 3. Differences

### 3.1 The roots

| | Difference | Visible effect | Evidence |
|---|---|---|---|
| D1 | Pose built before the motion, launch after it | the launch is off by the body's turn in a tick: aircraft on attack runs, turrets while the hull turns, ships | measured (§4) |
| D2 | Lua runs before weapons | `Main` forked by `OnFire` runs a tick later: every scripted shot leaves a tick late | [probe]: retail `OnFire` at tick 11 → first rocket at 11; engine 6 → 7 |
| D3 | A unit's orders and weapons in one update | a target handed over by an order is taken the same tick, a tick earlier than Moho | [probe]: retail `OnGotTarget` 86, engine 85 |
| D4 | Projectile damage at the collision | Moho finds the collision in tick N and damages in N+1; here damage, `OnImpact`, shield hits, veterancy are a tick earlier | [probe], not isolated: launched at tick 90, retail's first impact 106, the engine's 105 |
| D5 | A projectile made during the entity walk moves next tick | fragments, death-weapon shots, engine-fired shots a tick late; #701 compensates the lifetime by half a tick | code |
| D6 | No committed transforms | what an entity reads depends on id order: projectiles against targets that have or haven't moved this tick, formation and steering neighbours, area damage | code |
| D7 | Economy after Lua | Lua reads the economy a step older; Lua-requested consumption is granted a tick earlier | code |
| D8 | Orders from Lua run the same tick | AI and script orders start a tick early | code |
| D9 | Threads forked by order and weapon callbacks start next tick | build effects, bone attachment, roll-off, a finished generator's shield a tick late | code |
| D10 | The navigator runs inside the order | at each point of a move chain Moho pauses two ticks, the engine none (one with `--moho-pathing`); there are no double steps | code |
| D11 | Destruction is immediate | `OnDestroy`, freed cells and gone-from-queries a tick early; no `KillCleanup` at the end of the death tick | code |
| D12 | A weapon acquires and fires in one call | a new target can be shot the tick it is picked, along the old target's line | code |
| D13 | Attached entities follow at the end of the tick | k links of attachment lag k ticks; shields and cargo trail their carrier during the tick | code |
| D14 | Beams after all entities | beam damage after the tick's projectile hits; the muzzle from the stale pose | code |

### 3.2 Other timing differences

- **Pause.** Moho keeps beating while paused: commands and sim callbacks apply (an engineer told to stop
  stops, with `OnStopBuild`), the deletion queue, `Sync` and GC run; the tick of the pause request is not
  run. The engine stops everything and runs that tick.
- **Command tick number.** `GetGameTick()` in code run by a command: Moho N−1, the engine N.
- **Cancelling a task.** Clearing or removing the head of a queue destroys its task at once in Moho, and the
  task's destructor runs `OnStopReclaim`, `OnStopRepair`, `OnStopCapture`, the script task's `OnDestroy`
  (FR unit/CUnitCommandQueue.cpp:533-551). The engine runs those in the unit's next update, after Lua.
- **Economy.** Production and `ResourceConsumed` are banked in the unit's stage-A step and reach storage at
  the next economy; economy events take their first step with the next tick's grant; stat triggers only
  past tick 10.
- **Recon.** One army a tick runs a full `ReconTick` on the positions committed last tick; blips,
  `OnIntelChange`, `OnDetectedBy` and the UI's fog change only then. The engine does every army every tick
  on this tick's positions: detection up to N ticks early, without Moho's steps.
- **Death.** `Unit::Kill` (FR unit/core/Unit.cpp:16072-16199) runs adjacency loss and the transport's cargo
  before `OnKilled`, stats after. `KillCleanup` at the end of the death tick sends the weapons to their dead
  state (a started salvo stops) and removes the attacker, navigator and builder. The engine has no
  `KillCleanup`: a dying unit's salvo finishes, its beam keeps burning.
- **Entities in stage A.** A tree hit by a projectile or a unit falls the same tick in Moho (its motor wakes
  it to the end of the stage); a unit a factory creates in stage B moves and poses the same tick; here both
  wait a tick.
- **Projectile display.** Moho places a hitting projectile at the impact point with a fraction-of-a-step
  scale, so it reaches the point; its first frame starts at the muzzle. The engine drops it at the
  collision, a step short, and shows it a step past the muzzle on its first frame.

---

## 4. Evidence

The Corsair probe (retail and engine running the same Lua hooks; SCMP_009, seed 1): a Corsair attacks a
power generator on its own for 3000 ticks, and a moving scout for 1200.

| | structure: damage a salvo (salvos) | rockets within 2 of the target | scout: damage a salvo | scout: `OnGotTarget` / `OnFire` tick |
|---|---|---|---|---|
| retail [probe] | 763 (23) | 47% | 244 | 86 / 90 |
| d98ad203 | 0 (24): the racks don't follow the aim before #722 | 0% | 0 | 2 / 146 |
| d98ad203 + #722 + #723 | 318 (25) | 20% | 433 | 85 / 89 |
| the same + manipulators after the motion | 756 (24) | 48% | 356 | 85 / 89 |

The mean difference between the rack bone's and the aim bone's pitch at launch: retail 0.50°, the engine with
#722 and #723 0.57°, the same with the pose moved 0.57° (structure); the pose's frame, not the aim, is what
moves the hits.

---

## 5. Plan

Each step is one PR with a test that fails without it, checked on retail's Lua and FAF's, with the data tests
and goldens run in CI. The order of phases keeps determinism: phases walk entities in creation order and
armies by index. New state goes into the snapshot with a format version bump. Steps 9-11 were added to the
first eight after mapping every kind of state change in a tick.

Order of work: 1 → 2 → 3a → 3b → 3c → 4 → 6 → 5 → 7 → 9 → 10 → 8; step 11 at any time.

**Step 1. Pose after motion.** `tick_manipulators` moves after all of the unit's motion and before its
weapons, for every unit.
- Fixes D1 for weapons, beams' muzzles, builder arms, thrust and animation rate; the tick an order sees an
  arm on target is unchanged.
- Test: a turret on a hull turning at a constant rate fires along its aim; without the change the error is
  the hull's turn in a tick.
- Acceptance: the Corsair probe on top of #722 and #723 — about 756 damage a salvo and 48% within 2
  (retail 763, 47%).

**Step 2. Motion as the unit's own step.** Orders only set intent: the navigator's goal; an aircraft's mode
(move, attack run, circling, hover, hold height); a facing request; stop. `Unit::motion_tick()` runs one
step a tick in Moho's order: stun countdown → surface collision from the last move → fuel → layer by the
position at the start of the step → one `CalcMove*` (land, water and diving, hover, air with its combat
tactics, ballistic fall and crash) → motion events → wrong-layer damage. Then the pose (step 1),
regeneration or decay, resources. The air-step latch (`take_air_step`) goes; the navigator outlives the
order that set it, as Moho's does.
- Fixes: `OnLayerChange` and motion events no longer reach the weapons a tick early; the double air step
  while picking up cargo; stun counting down before the motion.
- Tests: an aircraft picking up cargo moves once a tick; `OnLayerChange` comes after the tick's fire;
  `SetStunned(1)` from Lua — the first moving tick is Moho's.
- Split into PRs: 2a land and water, 2b air (`fly_attack_run`, `fly_circling` run from the motion step, as
  `CalcMoveAir` runs the combat tactics), 2c transports, landing, ferries, staging.

**Step 3. `AdvanceBeat`'s phases.** Three PRs.
- **3a. Weapons in the army phase.** Armies in index order → each army's units in creation order → for each
  weapon its fire, then its target acquisition; before every unit's orders. Auto-attack orders and the
  attacker's status reach the order phase the same tick.
  - Fixes D3, D12; the tick an attack order's target is taken; a new target is never shot the tick it is
    picked; army 0's `OnFire` before army 1's.
  - Tests: an attack order → `OnGotTarget` on the next tick; no shot at a target in the tick it is picked;
    auto-attack moves the aircraft the same tick.
  - Acceptance: the scout probe — `OnGotTarget` 86, `OnFire` 90.
- **3b. Orders → Lua → entities.** All units' orders (creation order), then `resume_all`, then the entity
  walk.
  - Fixes D2, D8, D9; Lua sees an order begun, a build site created and its progress the same tick.
  - Tests: the projectile from `Main` forked by `OnFire` exists in the `OnFire` tick; an `IssueMove` from a
    Lua thread runs next tick; a thread forked by `OnStartBuild` runs the same tick.
  - Acceptance: the Corsair probe — first rocket in the `OnFire` tick (retail 11 → 11, 90 → 90).
- **3c. Economy in the army phase.** Each army's economy, stat triggers (past tick 10) and unit-cap stats in
  the army phase; production, `ResourceConsumed`, unit storage and economy events in the unit's step;
  reclaim banked in the order phase; overflow sharing inside each army's economy.
  - Fixes D7 and the economy items of §3.2.
  - Tests: a trigger true from the start fires at tick 11; consumption raised by a Lua thread works at
    N+2; a unit finished in N shows in `GetEconomyIncome` at N+1.

**Step 4. Projectiles as Moho's.** The entity walk includes entities made during it (a projectile always
moves on its creation tick); the impact runs at the start of the next step, with the pending position at the
impact point; the lifetime is an absolute tick (#701's half-tick compensation goes); the display reaches the
impact point.
- Fixes D4, D5, the projectile display, a shield's last tick after it breaks.
- Tests: damage on the tick after the collision, at the impact point; a fragment made in `OnImpact` moves
  that tick; `SetLifetime(0.5)` from `OnLostTarget` gives 6 steps; the snapshot of the collision tick holds
  the projectile at the impact point.

**Step 5. Committed transforms.** `position()`/`orientation()` return the committed transform; the motion
step writes the pending one; the end of the tick commits (`AdvanceCoords`): collision cells, intel, the
velocity from two commits, the last transform for display. Immediate writes (Warp, `SetPosition(p, true)`,
creation) write both and move the cells at once; `SetPosition(p)` writes the pending one. Every engine
spatial query reads the committed transform; a unit's own bones come from its pose.
- Fixes D6 and its spread to queries, `DamageArea`, collision, target acquisition, recon and the first
  frame of a projectile; replaces `note_tick_position` and the velocity from before/after.
- Tests: two units reading each other — same result whichever has the lower id; a projectile collides with
  its target where the target stood at the start of the tick; Warp, then `GetUnitsInRect` in the same thread
  finds the new place.

**Step 6. Navigators and steering in the army phase.** Path following → steering → (weapons) → the army's
path budget; arrival and failure events before the order phase; `SetGoal` leaves the steering waypoint;
`steer_ground_units` becomes the army phase's steering. For `--moho-pathing`.
- Fixes D10.
- Tests: a two-point move chain pauses two ticks at the first point; a path not found ends the order two
  ticks after it is set; an `IssueMove` from Lua at tick T first steps along the path at T+3 (to be pinned by
  probe P3 first).

**Step 7. Death and deletion.** A deletion queue: `OnDestroy` after the commit, first in first out; until
then the entity is found by `GetEntityById`, `BeenDestroyed()` is true and `IsDestroyed()` false, and it
stays in the spatial structures; cells, path obstacles and vision go when it is purged. An entity destroyed
during the tick still takes its step. `KillCleanup` at the end of the death tick; `Unit::Kill` in Moho's
order (dead flag → adjacency → cargo → queue → `OnKilled` → stats → kill credit).
- Fixes D11 and the death items of §3.2.
- Tests: an entity destroyed by a thread is found until the end of the tick and gets `OnDestroy` after every
  step; a salvo with a muzzle delay stops at the end of the death tick; a freed cell is free from the next
  tick.

**Step 9. Entities in the walk.** Attached entities wait for their parent and follow it the same tick;
shields, beams and script entities step after their owner; a prop parks without a motor and is woken to
the end of the walk by `FallDown`/`SinkAway`; a carrier's stored aircraft; cargo takes its step (pose, layer,
stun, regeneration).
- Fixes D13, D14, the tree and factory items of §3.2.
- Tests: a chain of two attached entities follows the same tick; a tree hit by a projectile leans the same
  tick; beam damage comes before the tick's projectile hits.

**Step 10. Recon by army.** A full `ReconTick` for army `tick % N`, `ReconRefresh` for the rest; blips,
flags, callbacks and jammer blips only on `ReconTick`; `RefreshBlips` every tick; recon reads committed
positions.
- Tests: army i's `OnIntelChange` only on ticks ≡ i (mod N); `IsSeenNow` reports the last `ReconTick`.

**Step 11. Commands and beats.** Commands before `++tick`; a cancelled task's callbacks when the command is
applied; pause as beats without a tick (commands, `OnDestroy`, `Sync`, GC run); the tick of a pause request
not run; single step; beats and commands after the game ends; a `Sync` table per tick when a frame runs
several.
- Tests: `GetGameTick()` in a `SimCallback` equals the previous tick's; Stop given to a building engineer
  while paused gives `OnStopBuild` before the game resumes; two ticks in a frame give two `Sync` tables.

**Step 8. Cleanup.** The air-step latch, `note_tick_position`, the duplicated cargo following and the stale
comments about order go.

---

## 6. Retail probes

Each runs the retail game with Lua hooks that log per tick, and the engine with the same hooks.

- **P1 turret-turn** (step 1). A tank drives a patrol round a stationary target and fires; per shot the muzzle
  and aim bone directions, the projectile's velocity, the hull heading this tick and last. Expected: the
  angle between the muzzle and the target does not grow with the hull's turn rate.
- **P2 shell-timing** (steps 3b, 4, 7). A stationary turret fires at a stationary target: the ticks of
  `OnFire`, the projectile's `OnCreate`, its position each tick, `OnImpact` and the target's `OnDamage`.
  Variants: a fragmenting shell; a salvo against a shield; a launcher killed during a salvo. Expected: created
  and first moved in the `OnFire` tick; `OnImpact` a tick after the step that crosses the target; fragments
  move on their creation tick; the salvo stops at the end of the death tick.
- **P3 order-timing** (steps 3b, 6). `IssueMove` from a Lua thread at tick T to a stationary tank: position
  and navigator target each tick; a chain of two moves; a replaced order; `IssueClearCommands` while moving;
  `CreateUnitHPR` and `IssueMove` in one thread; a factory's unit after `OnStopBeingBuilt`. Expected by
  faf-re's code: the first step along the path at T+3, a two-tick pause at each point.
- **P4 lua-sched** (step 11 and follow-ups). Event set by a later thread; `ResumeThread` on a thread in
  `WaitTicks`; `WaitFor` on an animation another thread destroys; `GetGameTick()` in a `SimCallback`.
- **P5 death-order** (step 7). `Destroy()` then `GetEntityById`, `IsDestroyed`, `GetUnitsAroundPoint` in the
  same thread; the tick of `OnDestroy`; cargo when a transport dies; a beam unit killed mid-beam.
- **P6 economy-timing** (step 3c). `GetEconomyIncome`/`Stored` each tick around a producer finishing; the
  first `OnStatsTrigger` of a trigger true from the start.
- **P7 recon-cadence** (step 10). Three armies; a unit entering vision: the ticks of `OnIntelChange` and
  `OnDetectedBy` against the tick its position crosses the radius.
- **P8 contact-pause** (steps 5, 9, 11). Two tanks meeting head on; a tree hit by a shell; Warp then
  `GetUnitsInRect`; an order given while paused.

---

## 7. Follow-ups (not about order)

Found while mapping the tick; kept out of the steps above. Among them: stun does not stop motion or aim here;
weapons pick targets the army has no blip for; a weapon's target change runs `OnLostTarget` and `OnGotTarget`
on every change instead of only between none and some; an unfinished unit's health is rewritten from its
progress each tick; a rotator waited on after `SetTargetSpeed` never signals; a cruise missile stops after
touching an anti-missile; order tasks wait n ticks where Moho's wait n−1; formations are laid out once
rather than kept; unit–unit contact is a positional push rather than Moho's impulse with its exemptions
(formation layer, same command, naval, layers, landed aircraft, stationary pairs); terrain type and lava
damage; a factory's skirt in the collision cells; production not scaled by the grant; overflow not shared
with allies by default; economy events not all-or-nothing.
