# Moho's spline states: reversing, braking to a stop, and a goal dead behind

Design, 2026-10-22. With Moho's speed cap and coast in (M203d), a ground unit's straight run and its
stop matched retail. Most units of the drive grid probe still differed in four ways:

- **Goal behind:** a unit that can't pivot backed up only within `BackUpDistance` of a final goal, where
  retail's turns about by reversing.
- **Goal dead behind:** the engine turned whichever way rounding gave, where retail's always turns the
  positive way.
- **Braking:** the engine braked earlier and more gently than retail toward its goal.
- **The `Moving` state:** the engine raised it two ticks after retail's.

This note ports the rules behind all four from faf-re.

Path prefixes:

- **FR:** faf-re's `src/sdk/moho/` (Draiget/faf-re, the decompiled FAF build of Moho)
- **EN:** this repository's `src/`, as of ede1e89a

Certainty tags: **[read]** read from faf-re code; **[probe]** measured in retail FA with a Lua probe.

---

## 1. Moho's rules

1. **The turn limiter** (FR `ai/CAiPathSpline.cpp` `RotateDirectionTowardTargetLimited`, 0x006992C0)
   **[read]** turns the heading by the sign of the cross product of heading and target, positive on a
   tie: a goal dead behind is turned to the positive way.
2. **The spline's states** (FR `ai/CAiPathSpline.cpp` `CAiPathSpline::Generate`, 0x005B2FF0) **[read]**.
   A spline is generated for each new target (`CAiSteeringImpl::DriveToNextWaypoint`), as path type
   `PT_0`, or `PT_1` when the steering drives through at top speed (`GetVal`).
   - **The start**, for a unit with `MaxSpeedReverse` above 0 and without `RotateOnSpot`, from the
     alignment (cosine of the heading error) and the speed ratio. With the goal behind (alignment
     below 0) and either slower than half its top speed or wide (`TurnRadius` above `TurnRate` in
     degrees), a unit moving forward faster than 0.01 of its top speed brakes (`PPS_4`), else it
     reverses (`PPS_5`). Moving backward, it brakes (`PPS_6`) with the goal ahead, else keeps
     reversing. Any other unit drives forward (`PPS_7`).
   - **Backing up** (`doBackup`): reversing to a goal nearer than `BackUpDistance` and within 60
     degrees of straight behind (alignment below -0.5), it goes tail first, through `PPS_5` and
     `PPS_6`, and stops on the goal.
   - **The changes**, after each node:
     - `PPS_7` brakes (`PPS_3`) once its braking distance, `v² / 2 max(MaxAcceleration, MaxBrake)`,
       is past the goal (`PT_0` only).
     - `PPS_3` ends the spline once stopped; the next one starts at `PPS_7`.
     - `PPS_4` reverses once stopped.
     - `PPS_5` brakes when its braking distance reaches the distance left, if backing up, else once
       its nose is within acos 0.15 of the goal; also when the ground behind it won't take it
       (`UnitWontFitAt` a length plus a braking distance back).
     - `PPS_6` goes forward once stopped; backing up, the move is then done.
   - **Speed:** braking states (`PPS_3`, `PPS_4`, `PPS_6`) aim at 0, and `PT_0` caps the others at the
     stopping speed `sqrt(2 d brake)`, `d` being the distance from the node to the goal (the whole of
     `d` in a tick once a tick's brake covers it). A wide unit's speed is held to
     `(max(alignment, -0.5) + 1) / 2` of its cap.
3. **The `Moving` state** (FR `unit/tasks/CUnitMoveTask.cpp`, constructor 0x006180E0 and destructor)
   **[read]**: the move task sets `UNITSTATE_Moving` (bit 2) as it starts, before its navigator has a
   path, and clears it as it ends.

## 2. What retail does

`tools/moho_probes/drivegrid` **[probe]**: each of the 85 ground units on SCMP_009, 24 at a time,
driven four legs from a standstill (40 back, 10 on, 13 back, 7 on, along one line), 337 legs in all.
`drive_diff.py` compares retail's log with the engine's, per leg:

- the first tick it turns, and which way;
- the tick it moves 0.3;
- its top speed;
- the tick the move ends, and how far it ends from its goal;
- its coast, and whether it backs up.

Legs differing, before this change, with the reverse states, and with braking and `Moving` as well:

| Measure | Before | Reverse | All |
|---|---|---|---|
| move ends | 39 | 13 | 10 |
| misses its goal | 19 | 17 | 13 |
| coast | 12 | 8 | 2 |
| backs up | 1 | 1 | 0 |
| turn side | 68 | 13 | 15 |
| starts moving | 25 | 25 | 25 |
| first turn | 25 | 17 | 15 |
| top speed | 47 | 48 | 20 |
| `Moving` from tick 1 | 0 of 337 | 0 of 337 | 330 of 337 |

A Striker's hop of 1.85, retail against the engine: 2.38, 2.48, 2.13 against 2.38, 2.47, 2.14.

A probe pitfall: retail's `GetPosition()` hands back one table, updated in place, so a probe must copy
its coordinates to compare two ticks.

## 3. The engine

- **Dead behind:** EN `sim/navigator.cpp` `Navigator::drive` turns a goal within 1e-4 of dead behind
  the positive way.
- **The spline's state:** `Navigator::Gear` holds it (`Forward`, `Stop`, `BrakeForward`, `Reverse`,
  `BrakeReverse`). It is chosen when the unit's waypoint changes or a move starts, and changed each
  tick by the rules above. The reverse drive, its speed cap read from the tail and a wide unit's cap
  follow from it.
- **Stopping speed:** the drive caps its speed at Moho's, from the distance along its path, not a tick
  ahead.
- **Grid pathing:** a move that ends within reach coasts what speed it has (`Unit::coast`), as one
  ended by Moho pathing does, rather than stopping dead.
- **`Moving`:** `IsUnitState('Moving')` reads the navigator's `busy()`, true from the order until the
  move ends.
- **Snapshot and checksum:** the gear, its backing up flag and its waypoint are in the snapshot
  (version 78) and the sync checksum.
- **Not ported:** shortcut: `PPS_7`'s change to `PPS_4` when the ground ahead won't take a sharp turn
  is left out.

## 4. Tests

`--drive-test` (gate):

- 5b: a Striker is `Moving` the tick after its order.
- 5c: on a hop of 2 a Striker peaks above 2.55 (2.67; a tick ahead gave 2.38).
- 7c: a tank spawned with its goal dead behind turns the positive way first.
- 7d: a tank that can't pivot gains ground toward a goal behind it while it turns its first 90 degrees.
- 7e: a unit that rotates on the spot never backs up.

`--patrol-test` clears the map's props about its route first. Which of them a patrolling engineer
reclaims, and so where it wanders, turned on where each stop left it.

## 5. Left

- **Turning near the target:** a unit turning about near its target goes up to 0.17 faster than retail
  (`dal0310`, `url0107` on the third leg). Moho's steering moves its destination up to 8 along the
  path (`TrySnapPosToWaypoint`), where the engine's waypoint is the target cell.
- **Accelerating while turning:** on the fourth leg some units speed up more slowly than the engine's
  (`url0101`).
- **The end of `Moving`:** retail's ends a tick or three after the engine's on some long legs.
- **Special drives:** units with no brake or a special drive (`url0401`, `url0402`, `uxl0021`,
  `xsl0402`).
- **Other layers:** naval units and hover on water, then aircraft, by the same probe.
