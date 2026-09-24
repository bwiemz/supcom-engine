#pragma once

#include "sim/entity.hpp"

#include <optional>
#include <string>
#include <vector>

struct lua_State;

namespace osc::map {
class Terrain;
}

namespace osc::sim {

class EntityRegistry;

/// Where one unit of a formation order goes.
struct FormationSlot {
    u32 unit_id = 0;
    Vector3 position;
};

/// Lay a group order out in `formation`, as Moho does (M204). The function
/// of that name in /lua/formations.lua gives the slots, {x, y, category,
/// moveDelay, rotate}, with x across the formation and y forward (rows behind
/// at negative y). They are placed about `target`:
/// - facing `facing` (the engine's heading: 0 south, pi/2 east), else from
///   the group's centre toward the target;
/// - scaled so that 1 is the group's largest footprint + 2 world units
///   (FAF's notes on the engine).
/// Each slot in turn takes the nearest unassigned unit of its category (ties
/// to the lower id); units left over go to the target. Returns nothing (the
/// order stays plain) for fewer than two units, a name with no function, or
/// a script error.
std::vector<FormationSlot> plan_formation(lua_State* L, const EntityRegistry& registry,
                                          const map::Terrain* terrain, std::vector<u32> unit_ids,
                                          const std::string& formation, const Vector3& target,
                                          std::optional<f32> facing);

} // namespace osc::sim
