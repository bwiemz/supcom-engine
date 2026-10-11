#pragma once

#include "core/types.hpp"

struct lua_State;

namespace osc::sim {

class EntityRegistry;
class Unit;

/// Moho's CUnitMotion::ProcessSurfaceCollisionFromLastMove and
/// Sim::DoCollisionsFor, for props: a surface unit that moved, whose SizeX
/// by SizeZ is over 0.2, on the ticks its id matches mod 5, calls
/// OnCollision(unit, nx, ny, nz, depth) of each prop its box meets.
void collide_with_props(Unit& unit, EntityRegistry& registry, lua_State* L, u32 tick);

} // namespace osc::sim
