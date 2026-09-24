#pragma once

#include "sim/entity.hpp"

#include <optional>

struct lua_State;

namespace osc::sim {

/// The shape a unit or prop blueprint (the table at `bp_index`) gives an
/// entity until a script sets another: a box SizeX by SizeY by SizeZ whose
/// base sits at CollisionOffsetX/Y/Z, as retail's wrecks and GetRandomOffset
/// read those fields. None for a blueprint without sizes.
CollisionShape blueprint_collision_shape(lua_State* L, int bp_index);

/// How far from its entity's position any point of `shape` can lie.
f32 collision_reach(const CollisionShape& shape);

/// Where along the segment `from` to `to` (0 at `from`, 1 at `to`) it
/// enters `shape`, worn by an entity at `position` facing `orientation`.
/// A segment starting inside counts at 0 when `inside_counts`; a shield,
/// which stops only what comes in from outside, passes false.
std::optional<f32> segment_enters(const CollisionShape& shape, const Vector3& position,
                                  const Quaternion& orientation, const Vector3& from,
                                  const Vector3& to, bool inside_counts = true);

/// The centre of `e`'s collision shape in the world (its position without one).
Vector3 collision_centre(const Entity& e);

} // namespace osc::sim
