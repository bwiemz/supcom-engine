#pragma once

#include "core/types.hpp"

struct lua_State;

namespace osc::sim {

class Entity;
class EntityRegistry;
class SimState;

/// One collision check of the beam `beam_id` (M206c).
/// - Where it reaches: posed on its launcher's muzzle bone, along the
///   muzzle's facing, to its length.
/// - What stops it: the first thing in the way whose OnCollisionCheckWeapon
///   (given the firing weapon) lets it: a unit, a shield met from outside, a
///   prop, a projectile, the ground or the water's surface.
/// - What follows: its end goes there, and its script hears
///   OnImpact(type, target), which deals the damage. With nothing in the
///   way the type is 'Air', or 'Underwater' below the surface.
/// The next check comes CollisionCheckInterval + 1 ticks later.
/// `before_step` is for a check made earlier in the tick than the beam's
/// step, which then counts down once more.
void check_collision_beam(SimState& sim, lua_State* L, u32 beam_id, bool before_step);

/// Moho's CollisionBeamEntity::MotionTick: false once its launcher or its weapon is gone.
bool step_collision_beam(SimState& sim, lua_State* L, Entity& beam);
void repose_collision_beam(EntityRegistry& registry, Entity& beam);

} // namespace osc::sim
