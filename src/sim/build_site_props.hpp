#pragma once

#include "core/types.hpp"
#include "sim/entity.hpp"

#include <string>

struct lua_State;

namespace osc::sim {

class EntityRegistry;
class Unit;

struct BuildSiteProp {
    u32 reclaim_id = 0;
    bool rebuild = false;
    u32 wreck_id = 0;
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

/// Whether another structure takes part of the skirt `bp_id` would have at
/// `site` (Moho's func_LocationIsFree)
bool build_site_taken(lua_State* L, const EntityRegistry& registry, const std::string& bp_id,
                      const Vector3& site);

} // namespace osc::sim
