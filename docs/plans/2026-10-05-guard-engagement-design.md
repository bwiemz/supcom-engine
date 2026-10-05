# Guard engagement and the GuardReturnRadius leash (parity PR 4b)

Design, 2026-10-05. This is parity group 2 of `docs/plans/2026-10-05-parity-ratchet-triage.md`, after NeedUnpack (PR 4a). #335 already ported Moho's patrol task:
- FindTarget in the leg box;
- the PATROLHELPER reclaim and repair;
- per-leg claims (`src/sim/unit_patrol.cpp`).

This PR adds what guard needs, the leash both use, and point guard. Attack-move (a real `AggressiveMove` command) is the next PR.

FR = faf-re `src/sdk/moho/` (the decompiled FAF build of Moho).

## What Moho does

**Guard** (`CUnitGuardTask`, FR `unit/tasks/CUnitGuardTask.cpp`):
- **Scan.** In its Processing state, every 6 ticks (Execute returns 7), the guard task calls `FindBestEnemy(primary weapon, mBlipsInRange, AI.GuardScanRadius)` (:1674-1698). The distance is measured from **the guarding unit**, not from what it guards (FR `ai/CAiAttackerImpl.cpp:965-975`).
- **Order of checks.** The enemy scan comes after refuel, ferry and factory assist, and before build, reclaim and repair help (:1819-1885).
- **On a find** it aborts its move and pushes a `CAttackTargetTask` on top of itself (`SetEnemy`, :1616-1626). Its state becomes Starting.
- **Starting** (:1811-1817): while the unit is farther than `max(guarded footprint SizeX, SizeZ) + GuardScanRadius/2` (3-D) from its guard reference, it walks back and does not scan. Once inside, it is Processing again.
- **Who reacts** (:596-628, 1677). A guard reacts unless it is:
  - an immobile factory (factory assist comes first);
  - an engineer guarding an engineer (`mDisableReactionState`) or a factory (`mDisableBestEnemySearch`);
  - without a primary weapon.
- **Point guard.** A Guard at a position first moves there, then runs the same task with no guarded unit (FR `ai/IAiCommandDispatchImpl.cpp:424-434`). Its reference is the guard point, and the footprint term is 1.

**The leash** (`CheckTargetGuardExempt`, FR `unit/tasks/CAcquireTargetTask.cpp:232-271`):
- **When it applies.** The weapon's acquire task checks it while the unit is Attacking and its attacker has a desired target.
- **The anchor** is the guarded unit's live position if there is one, else `GuardedPos`:
  - for a point guard, the guard point;
  - for a patrol, the unit's own position when it broke off (FR `CUnitPatrolTask.cpp:859-867`).
- **When it fires:** the unit is farther than `AI.GuardReturnRadius` (default 50) from the anchor, in 3-D. The target is then exempt, and the attack task ends.
- **It is armed only once the unit has reached weapon range.** The attack task hands the attacker its desired target only inside the firing solution, or within `Air.EngageDistance` (FR `CUnitAttackTargetTask.cpp:1283-1339`). A chase that never closes to range is not leashed.

**FindBestEnemy's exemption** (FR `CAiAttackerImpl.cpp:1355-1412`). A candidate is dropped when either holds:
- it is the target of a Reclaim or Capture in the unit's own queue;
- it is being captured, and an engineer of the unit's army has it as its focus.

## The engine today

- `order_guard` assists, helps or follows within 10 u. It never engages.
- A Guard with a position is dropped:
  - `IssueGuard` returns early;
  - `order_guard` pops a Guard with no target id.

  Retail's AI guards positions: `platoon.lua:566` (bases) and `:815` (markers).
- A patrol's break-off Attack (`from_patrol`) has no leash.
- The patrol scan (`Unit::find_patrol_target`) has no exemption.

## Design

1. **Blueprint.** `AI.GuardReturnRadius` (default 50) becomes `Unit::guard_return_radius_`, read beside GuardScanRadius and saved.
2. **One pick for patrol and guard.** `find_patrol_target`'s candidate filter and scoring become `Unit::best_enemy(candidates, range, ctx)`, with Moho's exemption added.
   - The patrol passes the units in its leg box; the guard passes `units_in_radius(own position, GuardScanRadius)`.
   - Both are in id order, so ties break alike on every peer.
3. **Guard engagement** in `order_guard`, after the factory branch and before the helpers. When the guard reacts (the rules above):
   - **Returning** (`cmd.guard_returning`): farther than `max(footprint) + GSR/2` from the reference, it goes there and holds. Inside, returning ends, and the next scan comes 6 ticks on.
   - **Scanning** on the 6-tick clock (the existing `patrol_scan` field, which the patrol already uses the same way). On a find:
     - abort the move;
     - push an Attack sub-order with `from_guard`, the guard's `command_id`, and the leash anchor;
     - set `guard_returning`.
4. **The leash** in `order_attack`, for a `from_guard` or `from_patrol` sub-order:
   - `leash_armed` is set once the unit is within its best weapon range of the target, or a winged aircraft is `engaged`.
   - Once armed, a 3-D distance from the anchor beyond `guard_return_radius` ends the sub-order. The parent order resumes in the same tick.
   - The anchor is the guarded unit (`leash_anchor_id`, its live position while it lives), else `leash_anchor_pos`.
   - A patrol's break-off Attack anchors at the unit's own position when it broke off.
5. **Point guard.** `IssueGuard(units, position)` issues a Guard with `target_id` 0 and the position. `takes_command` already lets only mobile units take it.
   - `order_guard` goes to the point, scanning and returning as above, with the point as the reference and footprint 1. Moho's dispatch only joins the command's formation (`CUnitCommand::Move`, FR `unit/CUnitCommand.cpp:1590`) and starts the guard task at once, so it looks about on its way too.
   - The follow step goes back to the point once more than 2 u away.
   - Helpers need a guarded unit, so a point guard has none. The REPAIR/REBUILDER helper box around a point, FR :1419-1561, is a follow-up.
6. **What scripts see.** `IsUnitState('Guarding')`, `GetGuardedUnit`, and the UI's `UserUnit:GetGuardedEntity` look past a guard's `from_guard` sub-order to the Guard beneath it: Moho keeps the Guarding state, and the guarded unit, while the attack task runs. `IsUnitState('Attacking')` is true meanwhile, as in Moho.

**State.**
- New fields:
  - `UnitCommand::from_guard`, `guard_returning`, `leash_armed`, `leash_anchor_id` and `leash_anchor_pos`;
  - `Unit::guard_return_radius_`.
- All are saved; snapshot version 12.
- The orders checksum mixes the new fields only when set ("GARD", "LESH"), so games without guards and patrols hash as before.
- No RNG draws.

## Attack-move (PR 4c)

**What Moho does:**
- `UNITCOMMAND_AggressiveMove` is the patrol task for one leg, made in formation (FR `ai/IAiCommandDispatchImpl.cpp:736-742`). When the leg ends, the dispatch removes the command; it does not rotate it to the back (:1005-1013).
- Its search box runs from where the unit stands to the goal. The exception is a Patrol queued behind it: Moho takes the box's far end from the queue's tail whenever that is a distinct Patrol, whatever the current leg (FR `CUnitPatrolTask.cpp:759-776`), and the engine does the same.
- Being in formation, COMMAND and SACU_BEHAVIOR units skip the helpers' sweep.
- `IsUnitState('Patrolling')` holds throughout.
- **Who issues it:**
  - `CPlatoon::AggressiveMoveToLocation` and `IssueFormAggressiveMove`, laid out in formation;
  - retail's UI, for an Attack on bare ground. Every mobile unit on ReturnFire is sent an AggressiveMove, and only the others get a ground Attack (`SplitSelectionForAggressiveMove`, FR `sim/CWldSession.cpp:7448-7465`).

**The engine** had made all of these plain moves.

**Change:**
- `CommandType::AggressiveMove` (81, the engine's own numbering), run by `order_patrol`. On arrival the order is popped.
- The Issue, Form and platoon bindings issue it, and the formation layout covers it.
- The input handler splits a ground Attack as Moho's UI does.
- The order line uses retail's `UNITCOMMAND_AggressiveMove` style.

## Deviations, accepted

- The leash is checked every tick, where Moho checks once per TargetCheckInterval.
- The guard reference is the guarded unit's position, not its GuardFormation slot. The existing within-10 follow stands in for the formation.
- Moho probably starts the attack sub-task 6 ticks after pushing it. The engine starts it in the same tick.
- Candidates come from the unit grid with a recon check, not from Moho's per-unit blip list.

## Tests

- **Unit tests** (`tests/test_guard.cpp`, a flat-map sim without Lua):
  - a guard engages an enemy within GSR of itself, not one beyond;
  - after the fight it walks home before it scans again;
  - the leash ends a chase once the unit has been in range and is GRR from what it guards;
  - an engineer guarding an engineer does not react;
  - a point guard goes to its point and engages there;
  - the exemption: a capture target in the queue is not picked.
- **Unit test** (`tests/test_patrol.cpp`): a patrol's break-off chase is leashed at GRR from where it broke off.
- **Integration**, `--guard-engage-test` on Seton's Clutch, with real blueprints: a T1 tank guarding a T2 tank engages an enemy engineer within 25 u, kills it and returns; and a point guard from `IssueGuard(units, position)`.
