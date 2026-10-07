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

} // namespace osc::sim
