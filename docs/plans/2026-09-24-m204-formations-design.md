# M204 — Formations

Status: design, 2026-09-24. The fourth milestone of Phase E (gameplay fidelity). It follows M203: units now drive and keep apart, so how a group is given its destination decides how it arrives and fights.

## Why

A group ordered somewhere goes to one point. The whole platoon aims at that spot, and M203b's separation spreads the pile that forms there. Moho instead gives each unit its own slot in a formation laid out around the destination, facing the way the group is travelling, and holds the group to its slowest member's speed. AI platoons also lose their route: retail's AI queues a platoon move for each waypoint it chose, and the engine kept only the last.

## What the survey found

These findings come from reading retail's `formations.lua` and AI scripts, FAF's annotated copy of `formations.lua`, and the engine.

**Retail's formation scripts:**
- **The functions:** `formations.lua`'s formation functions (`AttackFormation`, `GrowthFormation`, `BlockFormation`, `CircleFormation`, `GuardFormation`) take the group's units and return a list of slots. They run unmodified on this engine.
- **The slots:** each is `{x, y, category, moveDelay, rotate}`:
  - `x` runs across the formation, and `y` forward, with rows behind at negative `y`;
  - `category` is the kind of unit that takes the slot;
  - `moveDelay` staggers when rows start pathing, lower first (per FAF);
  - `rotate` is undocumented.
- **The scale:** FAF's copy states that the engine turns 1 in formation coordinates into (the group's largest `Footprint.SizeMax` + 2) world units.
- **The lists:** the engine reads `SurfaceFormations`, `AirFormations` and `ComboFormations` for the names it offers.
- **The facing:** `IssueFormMove(units, position, formation, degrees)` (and its aggressive, attack and patrol siblings) face the formation at `degrees`, with south 0 and east 90. That matches this engine's heading, where 0 is +Z.

**Retail's AI:**
- **Platoon moves queue:** `AttackForceAI` and friends call `platoon:MoveToLocation(waypoint)` for each waypoint of the path they chose, after a `Stop()`. They expect the moves to queue.
- **Where a platoon's formation comes from:**
  - the platoon's formation override;
  - the formation `AssignUnitsToPlatoon` was given (its fifth argument);
  - a template's fifth field.

**The engine:**
- **The form orders:** `IssueFormMove`, `IssueFormAggressiveMove` and `IssueFormAttack` are aliases of the plain orders.
- **Platoon moves:** each unit gets the same point, replacing its queue.
- **Formation storage:** the override is stored, and the per-unit formation is dropped.

**The player's orders:** in FA a plain right-click sends every selected unit to the point. A formation needs a right-click drag, which belongs with the world-view input work (M217).

## Slices

### M204a: formation moves

- **Expanding the order:**
  - An order carrying a formation name is expanded when the sim applies it: the direct path for AI orders, and the scheduler for player and network orders. So every peer and every replay computes the same slots.
  - The order records the name and an optional facing; the command codec carries them (replay format 5).
- **Laying out the slots:**
  - The formation function runs on the group. It faces the given `degrees`, or else points from the group's centre to the destination, and is scaled by the largest footprint + 2.
  - Each slot, in order, takes the nearest unassigned unit of its category (ties to the lower id). Units left over go to the destination itself.
- **Moving together:** every unit in the formation is held to the slowest member's top speed.
- **Bindings:**
  - `IssueFormMove` and `IssueFormAggressiveMove` take a formation and a facing.
  - Platoon moves use each unit's formation (the override first) and queue, as retail's AI expects.
  - `AssignUnitsToPlatoon` keeps the formation it is given.
- **Proof:** a `--formation-test` covers:
  - a group ordered in `AttackFormation` ending in its slots: a front row across the facing, spacing of the footprint + 2, and none overlapping;
  - the facing following `degrees`;
  - the group keeping the slowest unit's pace;
  - a platoon following each queued waypoint.

**What building M204a established:**

- **Retail's formation scripts run as they are.** `AttackFormation` and
  `GrowthFormation` return one slot per unit, category by category: eight
  Strikers make a front row, and two Lobos a row behind it. The engine
  only places the slots and gives them out.
- **The scale is the largest footprint + 2**, as FAF's notes say. A group
  of Strikers (footprint 1) stands 3 apart.
- **Platoons had been cutting their routes.** Retail's AI issues a platoon
  move for each waypoint of the path it chose, after a `Stop()`. The
  engine replaced a unit's orders with each move, so platoons made straight
  for the last waypoint. They now queue, and follow the route.
- **`MoveToTarget` had been sending platoons to the map's corner.** It
  read coordinates from the target unit's table, found none, and moved to
  (0, 0).
- **Pace is measured through pushes.** A formation's units drive at the
  slowest unit's speed, but neighbours' pushes (M203b) add a little to
  their movement over a tick.
- **Left for M204b:**
  - rows starting in `moveDelay` order;
  - holding the shape along the route;
  - `IssueFormAttack` and `IssueFormPatrol`;
  - the player's formation moves.

### M204b: travelling in formation

- **Row staggering:** rows start in `moveDelay` order.
- **Formation on the move:** the group holds its shape along the path, turning at corners, and regroups after obstacles.
- **Attack and patrol in formation:** `IssueFormAttack` and `IssueFormPatrol`.
- **Player formation moves** once the world view has right-click drag (M217).

## Risks

- **AI routes change.** Platoons now follow the waypoints their AI chose rather than cutting straight to the last one. Long AI games show whether fights, and the time it takes to reach them, change for the worse.
- **Lua cost.** Laying out a formation runs its script and matches slot categories through `EntityCategoryContains`. Orders are rare next to ticks, but AI platoons re-issue moves often, so this is measured.
