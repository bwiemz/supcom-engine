#pragma once

// Moho's orientation math for things that fly (faf-re QuaternionMath.cpp,
// Sim.cpp, Entity.cpp): a facing from a direction, the shortest turn from one
// direction to another, and that turn capped.

#include "sim/entity.hpp"

namespace osc::sim {

/// Which way `q` faces: its forward axis, R(q)·(0, 0, 1).
Vector3 forward_of(const Quaternion& q);

/// COORDS_Orient: the orientation whose forward axis is `dir`, its right axis
/// level. A zero vector gives the identity; straight up or down, a quarter
/// turn about X.
Quaternion coords_orient(const Vector3& dir);

/// QuatCrossAdd: the shortest rotation from `from` to `to` (neither need be
/// unit length), by their half vector. Opposite directions give the half
/// turn about `from`.
Quaternion shortest_arc(Vector3 from, Vector3 to);

/// RotateQuatByAngle: `q` (a rotation) cut to `angle` radians if it turns
/// further, its axis kept; unchanged if it turns less, or for an angle of a
/// half turn or more.
Quaternion cap_turn(Quaternion q, f32 angle);

/// QuatFromVecRot: `q` turned toward facing along `toward`, by at most
/// `angle` radians.
Quaternion turned_toward(const Quaternion& q, const Vector3& toward, f32 angle);

/// CalcWingedLift: how much of the climb `want` a winged aircraft's wings
/// give, its up axis `up_y` high and `height` over its floor.
f32 winged_lift(f32 want, f32 up_y, f32 lift_factor, f32 half_elevation, f32 height);

/// One step of ComputeAirControl's vertical axis, integrated as
/// SPhysBody::IntegrateFreefallStep: `velocity` and the rise it makes.
struct LiftStep {
    f32 velocity = 0.0f;
    f32 rise = 0.0f;
};
LiftStep lift_step(f32 velocity, f32 steer, f32 k_lift, f32 k_lift_damping, f32 load, f32 dt);

/// CalcMoveAir's ground to fly over: toward `look_ahead`, by at most
/// LiftFactor a second up and half that down.
f32 next_lift_ground(f32 ground, f32 look_ahead, f32 lift_factor, f32 dt);

/// CalcMoveAir's slowing for ground rising `clearance` over it within half
/// its look-ahead, `half_speed`: its speed's factor, 0.04 at least.
f32 rising_ground_slowdown(f32 clearance, f32 half_speed);

} // namespace osc::sim
