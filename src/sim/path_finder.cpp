#include "sim/path_finder.hpp"

#include "map/terrain.hpp"
#include "sim/path_clusters.hpp" // octile_distance

#include <algorithm>
#include <cmath>

namespace osc::sim::path {

namespace {

bool strict(const OccupancyRect& r) {
    return r.x0 < r.x1 && r.z0 < r.z1;
}

bool contains(const OccupancyRect& r, Cell c) {
    return r.x0 <= c.x && c.x < r.x1 && r.z0 <= c.z && c.z < r.z1;
}

bool overlap(const OccupancyRect& a, const OccupancyRect& b) {
    return a.x0 < b.x1 && b.x0 < a.x1 && a.z0 < b.z1 && b.z0 < a.z1;
}

bool inside(const OccupancyRect& inner, const OccupancyRect& outer) {
    return outer.x0 <= inner.x0 && inner.x1 <= outer.x1 && outer.z0 <= inner.z0 &&
           inner.z1 <= outer.z1;
}

} // namespace

void PathFinder::set_unit(const blueprints::Footprint& fp, i32 footprint_class, bool on_water) {
    footprint_ = fp;
    class_ = footprint_class;
    on_water_ = on_water;
    max_span_ = std::max<i32>(std::max(fp.size_x, fp.size_z), 1);
}

void PathFinder::set_goal(const NavGoal& goal) {
    goal_ = goal;
    goal_boundary_blocked_ = false;
    const OccupancyRect& r = goal.outer;
    if (!strict(r)) return;
    // Its edge as Moho's SetGoal walks it: x0 to x1 and z0 to z1 both
    // inclusive, so the far line is the one just past the rect (whose
    // x1 and z1 are exclusive everywhere else). Kept as Moho has it.
    for (i32 x = r.x0; x <= r.x1; ++x) {
        for (i32 z = r.z0; z <= r.z1; ++z) {
            if (x != r.x0 && x != r.x1 && z != r.z0 && z != r.z1) continue;
            f32 cost = 0;
            const Cell c{static_cast<i16>(x), static_cast<i16>(z)};
            if (!in_bounds(c, c, cost)) {
                goal_boundary_blocked_ = true;
                return;
            }
        }
    }
}

void PathFinder::prepare(const PathWorld& world, SearchType type, Cell anchor, f32 x, f32 z) {
    world_ = world;
    type_ = type;
    anchor_ = anchor;
    has_result_ = false;
    // Moho's UpdatePlayableRectGate: is the unit inside, by its span?
    const auto span = static_cast<f32>(max_span_);
    const OccupancyRect& p = world.playable;
    inside_playable_ = static_cast<f32>(p.x0) <= x - span && static_cast<f32>(p.z0) <= z - span &&
                       x + span <= static_cast<f32>(p.x1) && z + span <= static_cast<f32>(p.z1);
    const OccupancyRect at = footprint_rect(footprint_, x, z);
    has_occupancy_mask_ =
        world.terrain && world.grid &&
        footprint_fits(footprint_, *world.terrain, *world.grid, at.x0, at.z0) != 0;
    if (type == SearchType::None) {
        history_.clear();
        return;
    }
    // Moho's window round the unit's cell, [x - 8, x + 8) each way, its far
    // side clipped to the map's last cell (QueueSearch's heightfield
    // width - 2). Kept as Moho has it: 16 wide, and short of the last
    // cell at the map's edge.
    const i32 last_x = world.terrain ? static_cast<i32>(world.terrain->map_width()) - 1 : 0;
    const i32 last_z = world.terrain ? static_cast<i32>(world.terrain->map_height()) - 1 : 0;
    const OccupancyRect window{std::max(0, at.x0 - 8), std::max(0, at.z0 - 8),
                               std::min(last_x, at.x0 + 8), std::min(last_z, at.z0 + 8)};
    if (!strict(window)) return;
    while (history_.size() > 2) history_.pop_back();
    history_.push_front(window);
}

f32 PathFinder::heuristic(Cell c) const {
    const OccupancyRect& o = goal_.outer;
    const i32 dx = std::max({0, o.x0 - c.x, c.x - o.x1 + 1});
    const i32 dz = std::max({0, o.z0 - c.z, c.z - o.z1 + 1});
    return octile_distance(dx, dz) * 1.01f;
}

bool PathFinder::is_goal(Cell c) const {
    if (!contains(goal_.outer, c)) return false;
    const OccupancyRect& in = goal_.inner;
    return c.x < in.x0 || in.x1 <= c.x || c.z < in.z0 || in.z1 <= c.z;
}

bool PathFinder::should_search_rect(const OccupancyRect& r) const {
    if (!strict(r)) return false;
    // A first search looks closely round its start; a repath round the
    // windows it remembers. Either way, round the goal too.
    if (type_ == SearchType::None) {
        if (contains(r, anchor_)) return true;
    } else if (std::any_of(history_.begin(), history_.end(),
                           [&r](const OccupancyRect& h) { return overlap(r, h); })) {
        return true;
    }
    if (!overlap(r, goal_.outer) || !strict(goal_.outer)) return false;
    return !inside(r, goal_.inner);
}

bool PathFinder::can_traverse(Cell c) const {
    if (has_occupancy_mask_ && world_.terrain && world_.grid) {
        u8 caps = map_caps(footprint_, *world_.terrain, c.x, c.z);
        if (on_water_) caps = static_cast<u8>(caps & ~blueprints::occupancy::kSub);
        if (footprint_fits(footprint_, *world_.terrain, *world_.grid, c.x, c.z, caps) == 0)
            return false;
    }
    // A first search never minds mobile units; a leader's minds them all.
    if (type_ == SearchType::None || !world_.blockers) return true;
    return !world_.blockers->unit_blocked(world_.owner, c, type_ == SearchType::Leader ? 2 : 1);
}

bool PathFinder::in_bounds(Cell /*from*/, Cell to, f32& /*cost*/) const {
    if (world_.use_whole_map || !inside_playable_) return true;
    // By where it lands, the footprint's span inside the playable area; the
    // cell read as 16 bits unsigned, as Moho reads it.
    const auto margin = static_cast<f32>(max_span_);
    const auto x = static_cast<f32>(static_cast<u16>(to.x));
    const auto z = static_cast<f32>(static_cast<u16>(to.z));
    const OccupancyRect& p = world_.playable;
    return static_cast<f32>(p.x0) <= x - margin && static_cast<f32>(p.z0) <= z - margin &&
           x + margin <= static_cast<f32>(p.x1) && z + margin <= static_cast<f32>(p.z1);
}

void PathFinder::on_path(bool reached, std::vector<Cell> cells) {
    has_result_ = true;
    reached_ = reached;
    path_ = std::move(cells);
    if (PathListener* l = listener_) {
        listener_ = nullptr;
        l->on_path_event(reached, path_);
    }
}

} // namespace osc::sim::path
