# Projectile flight (roadmap item 3c)

Moho moves a projectile in `Projectile::MotionTick` and steers a homing one in
`Projectile::UpdateTracking` (faf-re `Projectile.cpp`, `QuaternionMath.cpp`
QuatFromVecRot/RotateQuatByAngle, `Sim.cpp` QuatCrossAdd). The engine moved
shots its own way: speed scaled up to MaxSpeed (only with both set), a
homing shot's velocity turned toward its target at TurnRate per second, and
the facing set from the velocity for drawing. This stacks on #405 (launch
along the muzzle).

## Moho, per tick (0.1 s)

- **Not tracking:** velocity += gravity·dt + facing·Acceleration·dt. With
  VelocityAlign, the facing turns toward the velocity by at most TurnRate
  degrees, as radians, *per tick* (no ×dt in this branch; TurnRate 0 never
  turns it).
- **Tracking** (UpdateTracking), in order:
  - **Aim point:** the target's aim spot while it has one.
  - **Target lost:** `OnLostTarget`, then tracking off. If the latch is set it
    still steers this tick at the last aim. The latch is set at launch for a
    target not in the Air or Sub layer.
  - **Lead:** LeadTarget shots aim ahead by the target's velocity × time to
    reach it at MaxSpeed, refined once.
  - **Underwater aim:** StayUnderwater shots aim at most 0.25 under the
    surface.
  - **Zig-zag:** a random offset in ±MaxZigZag per axis, redrawn from the sim
    RNG every ⌊ZigZagFrequency·10⌋ ticks, weighted by distance / MaxZigZag
    (capped at 1). It is added to a point MaxSpeed ahead along the steer, and
    kept 0.5 above the ground and, unless StayUnderwater, the water.
  - **Steer:** the facing turns toward the steer by at most TurnRate·0.1
    degrees.
  - **Under the water:** a rising facing is flattened at the surface.
  - **VelocityAlign:** the velocity is projected onto the facing.

  Then velocity += facing·Acceleration·dt.
- **Speed cap:** |velocity| clamped to MaxSpeed when it is non-zero.
- **Facing:** StayUpright rebuilds the facing level (COORDS_Orient). The local
  angular velocity spins it, reprojected onto its forward axis (align/track)
  or up axis (upright).
- **Position:** advances by the mean of the old and new velocity × dt.
  StayUnderwater shots under the water stay 0.01 under it.
- **QuatFromVecRot(q, v, a):** the shortest arc from q's forward to v
  (half-vector), capped at angle a, applied as d·q.

Shells (no acceleration, not tracking) integrate exactly as before.

## Not in this change

- **Bounce:** no retail projectile has Min/MaxBounceCount.
- **Tracking with no live target at launch is destroyed:** script-made
  homing projectiles get their target after creation in the engine.
- **The miss-retarget probe (ReTargetOnMiss):** dead in retail as Moho has it.
  The impact listener is attached only for ReTargetOnMiss weapons (7 in
  retail: the T1/T2 point defences and URL0202). CAcquireTargetTask::OnEvent
  returns unless the weapon also has AutoInitiateAttackCommand, and no retail
  weapon has both.

## Engine

- **`sim/flight_math`:** COORDS_Orient (shared with Lua's OrientFromDir), the
  shortest arc, the turn cap, QuatFromVecRot.
- **`Projectile::update`:** MotionTick and UpdateTracking as above. The
  blueprint gives LeadTarget, MaxZigZag and ZigZagFrequency (Lua's Change*
  still override them). New saved fields: lead_target, keep_last_aim and
  the zig-zag offset and its next tick.
- **Launch:** a shot faces the way it leaves (heading and pitch), not only
  its heading, since it now thrusts that way.
