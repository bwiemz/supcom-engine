#pragma once

// Mobile units in a unit's way (roadmap item 4c-3), as Moho's path search
// and navigator ask: COGrid::UnitIsBlocked at a cell, SweptPathBlockedByUnit
// along a straight way, and func_IsSourceUnit, which says whom a unit
// ignores. Units are found by their collision shapes; a unit without one
// never blocks.

#include "core/types.hpp"
#include "sim/collision.hpp"
#include "sim/entity.hpp"
#include "sim/path_finder.hpp"

namespace osc::sim {

class SimState;
class Unit;

/// Whether two boxes overlap: the separating-axis test over their six axes
/// and nine cross products. Touching overlaps, as in Moho's CollideBox.
bool boxes_overlap(const OrientedBox& a, const OrientedBox& b);

/// Whether a sphere reaches a box: its centre's distance to the box at most
/// `radius` (Wm3's IntrBox3Sphere3).
bool box_sphere_overlap(const OrientedBox& box, const Vector3& centre, f32 radius);

/// Moho's Entity::Intersects(Box3f): `e`'s collision shape, where it stands,
/// against `box`. Without a shape, no.
bool shape_overlaps_box(const Entity& e, const OrientedBox& box);

/// Moho's func_IsSourceUnit: whether `owner` ignores `candidate` when it
/// asks what is in its way. `mode` 1 plans: units that moved last tick
/// don't count, nor ones `owner` outranks. 2 counts every unit not ignored
/// outright (a different layer, flying, being carried...).
bool is_source_unit(i32 mode, const Unit& owner, const Unit& candidate);

/// The sim's answers to the path code's questions about mobile units.
class UnitBlockers final : public path::MobileBlockers {
public:
    explicit UnitBlockers(const SimState& sim) : sim_(sim) {}

    bool unit_blocked(u32 unit, path::Cell cell, i32 mode) const override;
    bool unit_blocked_at(u32 unit, f32 x, f32 z, i32 mode) const override;
    bool swept_blocked(u32 unit, const path::WorldPoint& from, const path::WorldPoint& to,
                       i32 mode) const override;

private:
    const SimState& sim_;
};

} // namespace osc::sim
