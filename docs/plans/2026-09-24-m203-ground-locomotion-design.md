# M203 — Ground locomotion

Status: design, 2026-09-24. The third milestone of Phase E (gameplay fidelity). It follows M201: shots now meet what is in their way, so where a unit stands and which way it faces decide fights.

## Why

Ground units don't drive: they slide. The navigator moves a unit straight at its next waypoint at full speed from the first tick, and stops it dead on arrival. It never turns the unit.
- **Every ground unit faces +Z forever.** It faces elsewhere only if a script sets its orientation. `CreateUnitHPR` ignores its heading.
- **Weapons and scripts see the wrong facing.** Heading arcs, weapons fired with no target, `GetHeading` and the 46 retail weapons `SlavedToBody` to a mobile unit all read it.
- **Nothing takes time.** Starting, stopping and turning are instant, so dodging, kiting and chasing play nothing like FA.
- **Motion events are invented.** A start is always `Cruise` then `TopSpeed` a tick later, whatever the unit's acceleration.

## What the survey found

These findings come from reading the engine code, retail's Lua and blueprints, and FAF's blueprint annotations.

**The engine** (`navigator.cpp`, `unit.cpp`):
- **Movement:** position steps toward the waypoint at `MaxSpeed × SpeedMult`. Arrival snaps onto it, within 0.5 at the end and 1.5 along the way.
- **Orientation:** set only by air flight and air crashes.
- **Fields read from `Physics`:** only `MaxSpeed`, `MotionType`, `Elevation` and the air subtable.
- **Multipliers:** `SetAccMult` and `SetTurnMult` are stored, and only air uses them.
- **`SetImmobile`:** stored but never enforced. Retail uses it to lock a unit while a weapon unpacks and during teleports.
- **`GetCurrentMoveLocation`:** returns the unit's position.
- **No ground avoidance:** ground units stack freely, and mobile units occupy no footprint.

**Retail's blueprints** (134 mobile surface units):
- **Always present:** `MaxAcceleration` (equal to `MaxSpeed` in 124), `TurnRate` (degrees per second, median 90) and `TurnRadius` (0 in 50; median 0.5 on land and 15 on water).
- **`MaxBrake`:** present in 123 units.
- **`RotateOnSpot`:** present in 45, true in 34.
- **Reversing:** `MaxSpeedReverse` and `BackUpDistance` (typically 4–5) govern backing up.
- **`StandUpright`:** in 24 units, always true.
- **FAF's annotations say:**
  - `TurnRadius` applies when the waypoint is farther than it, and only if its turn rate (speed / radius) beats `TurnRate`.
  - `RotateOnSpotThreshold` is a speed below which a unit may turn in place (default 0.5).
  - `MaxSpeedReverse` defaults to `MaxSpeed`.

**Retail's scripts:**
- **`OnMotionHorzEventChange(new, old)`** compares `Stopped`, `Cruise`, `TopSpeed` and `Stopping`. It starts and stops move sounds and effects, walk animations (`WalkingLandUnit`), and every weapon's `PackAndMove` and `FiringRandomnessWhileMoving`.
- **`OnMotionTurnEventChange`** (`Straight`, `Turn`, `SharpTurn`) plays turn sounds.
- **`IsUnitState('Moving')`** gates the weapon unpack lock.

## Slices

### M203a: units drive

- **Heading:** a ground unit has a heading, set from `CreateUnitHPR` and `SetOrientation`/`Warp`. Its orientation is that heading, tilted to the terrain unless `StandUpright`. Hover and naval units stay level.
- **Steering:**
  - Each tick the unit turns toward its waypoint at `TurnRate × TurnMult`, faster at speed when `TurnRadius` allows.
  - It drives along its heading, not straight at the waypoint.
  - A unit facing well away from its waypoint slows. If it has `RotateOnSpot`, it turns in place once below its threshold.
- **Speed:**
  - It accelerates at `MaxAcceleration × AccMult` toward `MaxSpeed × SpeedMult`, less when turning hard.
  - It brakes at `MaxBrake` so that it stops on its goal.
  - Arrival no longer snaps.
- **Reversing:** a unit with `MaxSpeedReverse` backs up to a goal behind it within `BackUpDistance`, rather than turning round.
- **Motion events come from the speed:**
  - `Cruise` while accelerating, `TopSpeed` at full speed, `Stopping` while braking to a halt, `Stopped` at rest.
  - `OnMotionTurnEventChange` follows the turn rate being used.
- **`SetImmobile`** holds a unit still. `GetCurrentMoveLocation` returns the navigator's goal.
- **Air is unchanged:** it keeps its flight model.
- **Proof:** a `--drive-test` covers:
  - a Striker turning to face a goal behind it, then driving there, with speed rising at its acceleration and falling to a stop on the goal;
  - the motion events in order;
  - a unit without `RotateOnSpot` turning while it moves;
  - backing up;
  - `SetImmobile`.

**What building M203a established:**

- **`CreateUnitHPR` takes angles about X, Y and Z.** Scenario files store a
  unit's orientation that way, with the heading second (17,025 of retail's
  map units are `(0, y, 0)`), and retail's `ScenarioUtilities` passes the
  three in order. FAF's annotation names them heading, pitch and roll, but
  Moho can't read them in that order, or map units would stand on their
  noses.
- **Braking needs a tick of look-ahead.** A curve of `sqrt(2 × brake ×
  distance)` evaluated where the unit is lags a tick behind, and the unit
  entered the arrival radius at 2.3 u/s and stopped dead. Taken a tick
  ahead, it arrives slow enough to stop in one.
- **A unit that can't pivot can circle its goal for ever.** When the goal
  lies inside its turning circle, it misses it every lap. Its speed is now
  capped so that it can turn onto the waypoint: the circle along its
  heading through the waypoint has radius `distance / (2 sin error)`.
- **Motion events follow the speed.** A Striker driving to a goal behind
  it pivots, then runs `Stopped>Cruise`, `Cruise>TopSpeed`,
  `TopSpeed>Stopping` and `Stopping>Stopped`, the order the weapon test
  already expected.
- **Left for later:**
  - terrain tilt (`StandUpright`);
  - hover units' `TurnFacingRate`;
  - aircraft on orders that don't fly them (they still slide);
  - avoidance (M203b).

### M203b: units keep apart

- **Separation:** nearby ground units steer apart, within `MaxSteerForce`, rather than stacking.
- **Pushing:** a moving unit pushes an idle friend out of its way.
- **Footprints:** mobile units hold their footprint on the pathing grid while stationary, so paths go around parked groups.
- **Proof:** a group ordered to one point spreads around it, and two columns crossing pass each other.

**What building M203b established:**

- **Separation is a pass, not a force in steering.** After every unit has
  moved, each overlapping pair of ground units is pushed apart by half its
  overlap. An idle unit makes way for a moving one; otherwise the smaller
  gives more. Nothing is pushed where its layer can't go. The pairs are
  found through a tick-local bucket grid, walked in id order, so the sums
  come out the same on every platform.
- **A crowd needs its own arrival.** Six tanks sent to one point can't all
  stand on it, and without help five would jostle for it for ever. A unit
  jostled for five ticks near its goal, without getting nearer, counts
  itself there.
- **Left:**
  - Steering around a unit ahead, within `MaxSteerForce`, rather than
    bumping into it.
  - Parked groups holding their footprint on the pathing grid.
  - Formations (M204), which give each unit of a group its own spot.

## Risks

- **Everything takes longer.** Units that turn and accelerate arrive later, so tests that expect arrival within some number of ticks, AI timings and the weapon test's motion sequence change. Each is checked, not loosened blindly.
- **Chasing.** A target that turns inside a pursuer's radius can be circled forever. Pursuit re-paths as it does now, and the long AI games watch for stuck units.
- **Cost.** Steering adds trigonometry per moving unit per tick, through `osc::dmath` for determinism. It is profiled against the benchmark.
