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
/// LiftFactor a second up and half that down, or the whole of it landing.
f32 next_lift_ground(f32 ground, f32 look_ahead, f32 lift_factor, f32 dt, bool landing = false);

/// CalcMoveAir's slowing for ground rising `clearance` over it within half
/// its look-ahead, `half_speed`: its speed's factor, 0.04 at least.
f32 rising_ground_slowdown(f32 clearance, f32 half_speed);

/// CalcAirMovementDampingFactor.
f32 air_move_damping(f32 control, f32 top, f32 k_move, f32 k_move_damping);

/// ComputeAirControl's level axes, integrated as SPhysBody::IntegrateFreefallStep.
struct AirMoveStep {
    Vector3 velocity{};
    Vector3 move{};
};
AirMoveStep air_move_step(const Vector3& velocity, const Vector3& force, f32 k_move, f32 damping,
                          f32 dt);

/// The axes an aircraft's controller steers toward: its up and its nose.
struct AirAxes {
    Vector3 up{};
    Vector3 nose{};
};

/// CalcWingedOrientation out of combat: the nose `nose` turned toward
/// `selected` by at most TurnSpeed x 0.1, ten times over, banked into the turn.
AirAxes winged_axes(const Vector3& nose, const Vector3& selected, f32 limited, f32 start_turn,
                    f32 turn_speed, f32 bank_factor, f32 elevation_scale);

/// CalcHoverOrientation: up leaning into the last tick's change of velocity
/// `dv`, its forward part dropped unless BankForward; the nose along `facing`.
AirAxes hover_axes(const Quaternion& body, Vector3 dv, f32 bank_factor, bool bank_forward,
                   f32 elevation_ratio, const Vector3& facing);

/// VAxes3::OrthoNormalize of `axes`, as an orientation.
Quaternion attitude_of(const AirAxes& axes);

/// ComputeAirControl's rotation error: the turn from `body` to `wanted`, as
/// axis x angle in the body's axes.
Vector3 attitude_error(const Quaternion& body, const Quaternion& wanted);

/// `v` in the axes of `body`.
Vector3 to_body(const Quaternion& body, const Vector3& v);

/// REntityBlueprint's inertia tensor for a box of these sizes, per unit mass.
Vector3 box_inertia(f32 size_x, f32 size_y, f32 size_z);

/// The angular velocity, in body axes, of a body of `inertia` turning with
/// angular momentum `momentum` (world axes).
Vector3 body_spin(const Quaternion& body, const Vector3& momentum, const Vector3& inertia);

/// The angular momentum of a body of `inertia` turning at `spin` (world axes).
Vector3 momentum_of(const Quaternion& body, const Vector3& spin, const Vector3& inertia);

/// SPhysBody::IntegrateAngularImpulse for a body-axes angular acceleration
/// `accel`, its angular momentum per unit mass `momentum` in world axes.
struct SpinStep {
    Quaternion orientation{};
    Vector3 momentum{};
};
SpinStep air_spin_step(const Quaternion& body, const Vector3& momentum, const Vector3& inertia,
                       const Vector3& accel, f32 dt);

} // namespace osc::sim
