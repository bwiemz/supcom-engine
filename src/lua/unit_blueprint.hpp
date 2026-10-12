#pragma once

#include "core/types.hpp"

struct lua_State;

namespace osc::blueprints {
class BlueprintStore;
struct BlueprintEntry;
} // namespace osc::blueprints

namespace osc::sim {
class SimState;
class Unit;
} // namespace osc::sim

namespace osc::lua {

/// Give a unit being made its blueprint's rules, as Moho's RUnitBlueprint
/// gives them: health, drive, weapons, economy, categories, shape, transport,
/// intel, lifebar, layer and footprints, an aircraft's flight and fuel, and
/// the orders and toggles it starts with. A unit `being_built` starts at 1
/// health. (x, z) is where it is made: a structure's or surfacing sub's
/// starting layer depends on the ground there.
void read_unit_blueprint(lua_State* L, const sim::SimState& sim,
                         const blueprints::BlueprintStore& store,
                         const blueprints::BlueprintEntry& entry, sim::Unit& unit, bool being_built,
                         f32 x, f32 z);

} // namespace osc::lua
