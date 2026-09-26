# M203c: units steer around each other

Ground units drive their paths (M203a) and push apart when they overlap
(M203b), but they don't see each other coming. Two tanks on the same road
meet head-on and shove past each other, and a column crossing another
drives through it. Moho's steering predicts those meetings about two
seconds ahead and acts on them:
- it steps aside to overtake a unit ahead going its way;
- it stops for a unit coming at it or crossing its way, and lets it pass.

The reference is faf-re's decompiled Moho:
- `CAiSteeringImpl`: `CheckCollisions`, `PredictCollisionForSteerings`,
  `ResolvePossibleCollision`, `ProcessSplineMovement`;
- `CAiPathSpline`: `Generate` and `Update`;
- `func_UnitsWillCollide` and `func_IsSourceUnit`;
- `Unit::IsHigherPriorityThan`.

## Moho's rules

**The path ahead.** A moving unit's steering keeps a spline: its
simulated motion, one node per tick, at most 20 nodes (two seconds). When
the unit reaches the spline's end it makes a new one, and checks for
collisions.

**Checking** (`CheckCollisions`). Nothing is checked for a unit that is:
- dead, being built, being destroyed, or submerged.

Otherwise its collision state is cleared, and the units within a query
radius are gathered. The radius is:
- the spline's node count × `MaxSpeed` × 0.1,
- plus its braking distance (`MaxSpeed² / 2 MaxAcceleration`),
- plus its larger size.

A candidate is ignored (`func_IsSourceUnit`) when it is any of these:
- dead or being destroyed, the unit itself, or immobile;
- attached (on a transport);
- on another layer, or a land unit when the unit is naval;
- an aircraft in the air, or a transport;
- a structure-bound unit, when the unit ignores structures;
- waiting for the same transport as the unit;
- a unit the unit is upgrading into.

It is also ignored when it has no steering, or when its spline is a
`PT_2` path (a sidestep under way).

The rest fall into two lists:
- **Deferred:** a unit of the same army that does not outrank the unit.
  It yields, so the collision is predicted for it, against the unit.
- **Preferred:** any other unit that is on a path (or can fly). The unit
  yields, so the collision is predicted for the unit.

**Outranking** (`Unit::IsHigherPriorityThan`) is decided by the first rule
that separates the two:
1. An immobile or upgrading unit outranks.
2. A naval unit outranks a land one.
3. One that ignores structures (a hover or amphibious footprint) outranks
   one that doesn't.
4. A flier on the ground outranks.
5. One waiting for a transport outranks.
6. A guarding unit yields to the unit it guards.
7. In a formation, the lead outranks.
8. A still unit outranks a moving one.
9. In a formation, the lower priority order outranks, then the one nearer
   its spot.
10. The larger footprint outranks.
11. The one more in line with the other outranks.
12. The lower entity id outranks.

**Predicting** (`PredictCollisionForSteerings`). The two splines are
walked together from their current nodes, three nodes (ticks) at a time.
At each step it asks whether the units will collide (`UnitsWillCollide`):
- Each unit is a box in plan view. It is (SizeX + SizeZ) / 4 wide either
  side, and its length is SizeZ plus a braking lead of `speed² / 2
  MaxAcceleration` (speed per second).
- The box sits ahead of the unit by the lead, along its facing.
- They collide if the boxes overlap: a separating-axis test over the four
  axes.

The lead is left out for two units of the same formation, unless either
is attacking.

The first collision closer than the one recorded becomes the unit's
possible collision. That records the other unit, the spot, and the tick
it would happen. The walk stops at the end of either spline.

**Resolving** (`ResolvePossibleCollision`) is done at that tick:
- **Against a flier** (it can fly), the flier is stopped if the two are
  closing, and nothing else happens.
- **Against a unit that isn't moving, or isn't closing** (the dot product
  of their offset and relative velocity is not negative), nothing
  happens.
- **Against one that is not heading away within 45°** of the line from
  the unit to it (it comes head-on, or crosses), the unit **stops**:
  - its spline becomes a stop, at twice its acceleration and brake;
  - it holds still for at least five nodes (half a second);
  - then it carries on to its destination.

  The other drives on. If it meets the stopped unit, separation (M203b)
  makes way, as an idle unit makes way for a moving one.
- **Against one within that cone** (it is ahead, going the same way), the
  unit **steps aside** to overtake:
  - It aims for a point off its path. The distance is the two units' larger
    sizes plus 0.5.
  - The direction is the other's heading (blended with its own when they
    run within 45°) plus a turn of 90° to the side away from the other.
    The side is flipped within a formation. So the point is ahead of the
    unit and to one side.
  - It drives to that point on a `PT_2` spline, then back to its
    destination.
  - The other unit brakes too (a `COLLISIONTYPE_5` repath), unless the two
    are in the same formation, or the other is naval and the unit isn't.

**A pushed unit** (being shoved by another) drops its spline, and makes a
new one once it has nearly stopped.

## The engine

The engine has no splines. Its navigator follows A* waypoints and drives
them one tick at a time (M203a). The steering pass stands in for the
spline with the path ahead, and keeps Moho's decisions.

- **The path ahead** is up to 20 points, one per tick. They are the
  waypoint polyline sampled from the unit's position at its speed,
  rising toward its top speed at its acceleration. This gives Moho's node
  spacing without simulating its turns.
- **Checking** happens in a steering pass, each tick after orders and
  before movement:
  - when a unit gets a new path;
  - every 10 ticks while it drives (see below);
  - after it finishes a sidestep or a stop.

  Units are walked in id order and candidates in id order, so every
  platform decides the same.
- **Candidates** come from the unit grid (M224b) within Moho's query
  radius, filtered as `func_IsSourceUnit` does. "Transport" and "upgrade"
  are read from the unit's state, and "same formation" is the same
  formation move. The deferred and preferred lists are Moho's, with
  `IsHigherPriorityThan`'s order. Its formation rules (the lead, then the
  priority order) are left out, since M204 keeps neither, and footprints
  carry no `IgnoreStructures` flag here.
- **Predicting** and **resolving** follow Moho:
  - A possible collision holds the other unit's id, its tick and its spot.
  - A **sidestep** puts a temporary waypoint in front of the path: the
    point beside it. The unit drives there as it drives any waypoint,
    then goes on.
  - A **stop** brakes the unit at twice its brake and holds it for five
    ticks once it has stopped. Then it carries on and checks again.
- **Determinism:** the trigonometry goes through `osc::dmath`, and
  everything is in id order.

**What building it established:**

- **Checks every 10 ticks, not 20.** Moho re-checks as its spline runs
  out, and also at each of its navigator's waypoints, which lie close
  together. The engine's A* waypoints are few. At a 20-tick check, two
  Strikers crossing were 9.6 apart, just beyond the owner's query radius
  (9.4: its own 20 ticks of travel, braking distance and size). The next
  check came when they touched. Checking every half horizon sees any
  meeting within it in time to stop.
- **Two units that yield to each other need a tiebreak.** Units of
  different armies both take the other as preferred, and a tie in rank
  does the same: head-on, `IsHigherPriorityThan`'s "more in line" rule says
  each outranks the other. Both stop, both go on, both stop again, for
  ever. In Moho the spline timing tells them apart. Here the one that wins
  goes first: the one that outranks without being outranked back, else the
  lower id. The other stops.
- **A sidestep point is reached, not passed near.** The point lies about
  1.2 off the path, inside the navigator's 1.5 waypoint tolerance. It
  counted as passed at once, and the unit never moved aside. It now has a
  0.25 tolerance of its own.
- **Exactly head-on, separation pushes straight back.** Two units on one
  line push along it, so a stopped unit is shoved backwards the whole way.
  The test offsets the lines by half a unit, as a real meeting is.

**Left:**
- Moho's spline curvature (turns simulated per node).
- The formation lead and priority order in ranking.
- Stopping a flier on the ground: a flier is only ever a candidate there.
- A pushed unit dropping its path (separation pushes it instead).

## Tests

A `--steer-test` (gate) on flat ground:
1. **Overtaking:** a fast unit comes up behind a slow one going the same
   way. It steps aside and passes, rather than running into it. The two
   never overlap, and both reach their goals.
2. **Crossing:** a tank crosses another's path at right angles. The one
   that yields stops, then goes on after the other has passed. They never
   overlap.
3. **Priority and head-on:** a Pillar (footprint 2) and an engineer (1) of
   one army meet head-on. The engineer is made first, so its footprint,
   not its id, decides. It stops, and the Pillar keeps its line. Both
   reach their goals.
4. **Parked:** a unit parked on the path isn't a candidate (it isn't on a
   path). Separation (M203b) handles it.
5. **Overtaking one that outranks it:** a Striker overtakes a slower Pillar
   of its own army (footprint 2, so the Pillar never yields on its own).
   The Striker steps aside, and the Pillar is made to stop while it passes.
5. **Determinism:** the pass runs in the two-process determinism test.

The existing movement tests (drive, crowd, formation, move) must stay green.
Their arrival times may shift, and each shift is checked, not loosened.

**Mutation check:** 9 mutants. Eight are killed:
- no pass;
- no sidestep;
- never stopping;
- no tiebreak;
- no footprint rank;
- a loose sidestep tolerance;
- checking every 20 ticks;
- the overtaken unit not stopping.

One survives: no deferral, where the lower-ranked of one army is recorded
as the owner's instead. In each test both units check, and the tiebreak
reaches the same decision. Deferral only changes *when* the yielder learns
of the meeting, when its own, smaller query radius hasn't reached the other
yet.
