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

/// The forward axis of an orientation as Moho's Projectile and MotionTick take it.
Vector3 forward_moho(const Quaternion& q);

/// Wm3's Vector3::Normalize: zero and its length 0 at 1e-6 or under, else times the
/// inverse length.
f32 wm3_normalize(Vector3& v);

/// COORDS_Orient (0x0050B480) bit for bit: Wm3's Normalize (times the inverse length)
/// and func_MatrixToQuat (0x004EB3F0) on the rows right, up, forward.
Quaternion coords_orient_moho(const Vector3& dir);

/// func_MatrixToQuat (0x004EB3F0): the rotation of a matrix given by its rows, as axes.
Quaternion matrix_to_quat_moho(const f32 m[3][3]);

/// A map prop's orientation from its .scmap rows (CWldMap's prop load): matrix_to_quat_moho,
/// made unit length with the squares summed z, y, x, w.
Quaternion map_prop_orientation(const f32 rows[3][3]);

/// MatrixRowsToQuatCanonical (0x004F0CB0): the rotation of a matrix given by its rows.
Quaternion matrix_rows_to_quat_canonical(const f32 m[3][3]);

/// CreateUnitHPR's and CreatePropHPR's orientation: BuildRotationMatrixFromEulerHPR
/// (0x006D1E30, heading about Y times pitch about X times roll about Z, in radians) through
/// matrix_rows_to_quat_canonical.
Quaternion hpr_orientation(f32 heading, f32 pitch, f32 roll);

/// QuatFromAxisAngleVector (0x00697360): the turn of `axis_angle`'s length about it, the axis
/// by Wm3's Normalize.
Quaternion quat_from_axis_angle_moho(Vector3 axis_angle);

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

/// CalcMoveAir's ground to fly over: toward `look_ahead`, by at most
/// LiftFactor a second up and half that down, or the whole of it landing.
f32 next_lift_ground(f32 ground, f32 look_ahead, f32 lift_factor, f32 dt, bool landing = false);

/// CalcMoveAir's slowing for ground rising `clearance` over it within half
/// its look-ahead, `half_speed`: its speed's factor, 0.04 at least.
f32 rising_ground_slowdown(f32 clearance, f32 half_speed);

/// CalcAirMovementDampingFactor.
f32 air_move_damping(f32 control, f32 top, f32 k_move, f32 k_move_damping);

/// The axes an aircraft's controller steers toward: its up and its nose.
struct AirAxes {
    Vector3 up{};
    Vector3 nose{};
    f32 wing_blend = 0.0f;
};

/// CalcWingedOrientation: the nose `nose` turned toward `selected` by at most
/// `turn_speed` x 0.1, ten times over, banked into the turn; in a NormalTurn
/// ten times harder, its alignment to the eighth.
AirAxes winged_axes(const Vector3& nose, const Vector3& selected, f32 limited, f32 start_turn,
                    f32 turn_speed, f32 bank_factor, f32 elevation_scale, bool normal_turn = false);

/// CalcHoverOrientation: up leaning into the last tick's change of velocity
/// `dv`, its forward part dropped unless BankForward; the nose along `facing`.
AirAxes hover_axes(const Quaternion& body, Vector3 dv, f32 bank_factor, bool bank_forward,
                   f32 elevation_ratio, const Vector3& facing);

/// VAxes3::OrthoNormalize of `axes` and MatrixColumnsToQuatCanonical.
Quaternion attitude_of(const AirAxes& axes);

/// ComputeAirControl's rotation error from `body` to `wanted`, through QuatToAxisAndAngle:
/// axis x angle.
Vector3 attitude_error(const Quaternion& body, const Quaternion& wanted);

/// `v` in the axes of `body`.
Vector3 to_body(const Quaternion& body, const Vector3& v);

/// REntityBlueprint's inertia tensor for a box of these sizes, per unit mass.
Vector3 box_inertia(f32 size_x, f32 size_y, f32 size_z);

/// Entity::GetPhysBody's mass and inverse inertia, 1 / (`inertia` x mass); a box's inertia when
/// the blueprint's has a zero.
struct BodyMass {
    f32 mass = 1.0f;
    Vector3 inv_inertia{1.0f, 1.0f, 1.0f};
};
BodyMass body_mass(f32 density, f32 size_x, f32 size_y, f32 size_z, const Vector3& inertia);

/// The angular velocity, in body axes, of a body turning with angular momentum `impulse`
/// (world axes).
Vector3 body_spin(const Quaternion& body, const Vector3& impulse, const Vector3& inv_inertia);

/// The angular momentum, in world axes, of a body turning at `spin` (world axes).
Vector3 momentum_of(const Quaternion& body, const Vector3& spin, const Vector3& inv_inertia);

/// Moho's SPhysBody for a flier: its centre (its collision box's), orientation, velocity and
/// angular momentum in world axes.
struct PhysBody {
    BodyMass mass;
    Vector3 centre{};
    Quaternion orientation{};
    Vector3 velocity{};
    Vector3 impulse{};
};

/// ComputeAirControl's gains, the lift, turn and roll ones over the transport load; `damping` is
/// CalcAirMovementDampingFactor's.
struct AirGains {
    f32 k_move = 1.0f;
    f32 k_lift_damping = 1.0f;
    f32 lift = 1.0f;
    f32 turn = 1.0f;
    f32 roll = 1.0f;
    f32 k_turn_damping = 1.0f;
    f32 k_roll_damping = 1.0f;
    f32 damping = 1.0f;
};

/// ComputeAirControl's force and world-axes torque on `body`, steered by `steer` toward
/// `wanted`.
struct AirControl {
    Vector3 force{};
    Vector3 torque{};
};
AirControl air_control_of(const PhysBody& body, const Quaternion& wanted, const Vector3& steer,
                          const AirGains& gains);

/// SPhysBody::IntegrateFreefallStep: velocity and centre under `force` and gravity, then
/// IntegrateAngularImpulse under `torque`.
void integrate_freefall(PhysBody& body, const Vector3& force, const Vector3& torque, f32 dt);

/// SPhysBody::IntegrateAngularImpulse: angular momentum `impulse` (world axes) under `torque`,
/// and the orientation turned by its mean over the step.
struct SpinStep {
    Quaternion orientation{};
    Vector3 momentum{};
};
SpinStep integrate_angular_impulse(const Quaternion& body, const Vector3& impulse,
                                   const Vector3& inv_inertia, const Vector3& torque, f32 dt);

/// CalcMoveAir's tumble for an aircraft killed in flight: each axis' draw is in +-h, h the
/// axis' inverse inertia clamped to [0.25, 4] (a NaN to 0.25).
Vector3 tumble_range(const Vector3& inv_inertia);

/// The tumble's torque in world axes for the draws `drawn`: each over its axis' inverse
/// inertia, turned by `body`.
Vector3 tumble_torque(const Quaternion& body, const Vector3& inv_inertia, const Vector3& drawn);

} // namespace osc::sim
