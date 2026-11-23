#pragma once

#include "core/types.hpp"
#include "sim/entity.hpp"

#include <string>

struct lua_State;

namespace osc::sim {

class EntityRegistry;
class SimState;
class Unit;

struct BuildSiteProp {
    EntityId reclaim_id{};
    bool rebuild = false;
    EntityId wreck_id{};
    f32 bonus = 0.0f;
};

/// Moho's CUnitMobileBuildTask::FindObstructingPropToReclaim (faf-re's
/// CUnitMobileBuildTask.cpp).
BuildSiteProp find_build_site_prop(lua_State* L, const EntityRegistry& registry,
                                   const Unit& builder, const std::string& bp_id,
                                   const Vector3& site);

/// Moho's CUnitMobileBuildTask::FindExistingSeedUnitOnSite: a unit of any
/// army being built from `bp_id` exactly at `site`
const Unit* find_build_site_frame(const EntityRegistry& registry, const std::string& bp_id,
                                  const Vector3& site);

/// Whether `bp_id` may no longer go up at `site`, as Moho's build task asks
/// before it makes the structure (func_LocationIsFree)
bool build_site_taken(lua_State* L, const SimState& sim, i32 army, const std::string& bp_id,
                      const Vector3& site);

} // namespace osc::sim
