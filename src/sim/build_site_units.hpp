#pragma once

// Units standing on a build site, as Moho's build tasks ask about them (faf-re
// SimFromLua.inl): Sim::LocationIsFree, whether a site's cells are clear of
// units, and SIM_TryToBuild, which sends an army's own resting units off them.
// The mobile build task asks the first before it makes its frame and, told
// no, the second, then waits and asks again (CUnitMobileBuildTask::Execute,
// TASKSTATE_Processing).

#include "core/types.hpp"
#include "sim/occupancy.hpp"

namespace osc::sim {

/// A mobile build gives up once it has found units on its site more than
/// this many times: 11 looks, failing at the 12th (CUnitMobileBuildTask::
/// Execute's `mPlacementRetryCount > 10`, Steam 0x005FE4DB).
inline constexpr i32 kSiteTries = 10;
/// Ticks from one look to the next: the task returns 50, which Moho's task
/// thread runs 49 ticks on (Steam 0x005FE64F).
inline constexpr i32 kSiteRetryTicks = 49;

class SimState;
class Unit;
struct SimContext;

/// Moho's Sim::LocationIsFree (faf-re 0x0062DA50, Steam 0x00634520): whether
/// no unit stands on the cells `rect`. A unit stands there when its collision
/// shape meets the box over them (100 up and down from height 0), unless it
/// is dead, `ignore`, carried by `ignore`, on the air layer, or (with
/// `require_idle`) has an order. A mobile unit counts wherever it meets the
/// box; a structure only where its skirt overlaps the cells.
bool location_is_free(SimState& sim, const Unit* ignore, const OccupancyRect& rect,
                      bool require_idle);

/// Moho's SIM_TryToBuild (faf-re 0x0062DD40, Steam 0x00634810): each mobile
/// unit of `army` whose shape meets the box over `rect` and that is at rest
/// (or of the AIR category), off the air layer and alive (with
/// `require_idle`, idle as well) is sent to the nearest place clear of `rect`
/// that it fits, no other unit stands on and nobody has reserved: first
/// along the line away from the rect's centre, three times the rect's larger
/// half-extent long, then in square rings round it. It walks there with no
/// order (Unit::step_aside). Each place is held while the next unit looks,
/// and given up after.
void try_to_build(SimContext& ctx, i32 army, const OccupancyRect& rect, bool require_idle);

} // namespace osc::sim
