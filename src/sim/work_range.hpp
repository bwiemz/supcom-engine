#pragma once

// How near a unit must be to build, repair, reclaim or capture something
// (M206e), as Moho's unit tasks measure it (faf-re's decompiled
// CUnitMobileBuildTask, CUnitRepairTask, CUnitReclaimTask and
// CUnitCaptureTask). Every range is a gap: the ground distance between the
// two centres, less the worker's largest footprint side and the target's
// largest side, in whole cells. Builds and repairs measure the target's
// skirt, reclaim and capture its footprint.

#include "core/types.hpp"
#include "sim/entity.hpp"

#include <string>
#include <utility>

struct lua_State;

namespace osc::sim {

class Unit;

/// Capture's fixed reach, not MaxBuildDistance: a unit moves in when the gap
/// is over 5, and gives up beyond 10.
inline constexpr f32 kCaptureReach = 5.0f;
inline constexpr f32 kCaptureHold = 10.0f;

/// An entity's largest footprint side, in whole cells.
f32 footprint_extent(const Entity& e);

/// A unit's largest skirt side (a skirt is at least its footprint).
f32 skirt_extent(const Unit& u);

/// The skirt of the blueprint `bp_id` (at least its footprint), X and Z:
/// what a structure not yet begun will span. 1 by 1 when unknown.
std::pair<f32, f32> blueprint_skirt(lua_State* L, const std::string& bp_id);

/// The gap from `worker` to a target centred at `at` whose largest side is
/// `target_extent`.
f32 work_gap(const Unit& worker, const Vector3& at, f32 target_extent);

/// Where `worker` goes to reach something centred at `at` that it must stay
/// clear of, `half_x` by `half_z` about its centre: just outside that, on
/// the worker's side, with room for the worker's footprint. Moho's
/// Unit::PrepareMove picks such a cell; where exactly is a reading.
Vector3 approach_point(const Unit& worker, const Vector3& at, f32 half_x, f32 half_z);

} // namespace osc::sim
