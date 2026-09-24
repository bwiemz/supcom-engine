#pragma once

#include "core/types.hpp"

struct lua_State;

namespace osc::sim {

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
/// `before_pass` is for a check made earlier in the tick than the beam
/// pass, which then counts down once more.
void check_collision_beam(SimState& sim, lua_State* L, u32 beam_id, bool before_pass);

/// The beam pass, each tick after the entities have moved:
/// - every beam is posed on its launcher's muzzle;
/// - an enabled one is checked when its countdown runs out;
/// - between checks its end follows the muzzle at the last check's reach.
void update_collision_beams(SimState& sim, lua_State* L);

} // namespace osc::sim
