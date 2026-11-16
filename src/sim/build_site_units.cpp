#include "sim/build_site_units.hpp"

#include "blueprints/footprint.hpp"
#include "sim/category_set.hpp"
#include "sim/collision.hpp"
#include "sim/collision_cells.hpp"
#include "sim/entity_registry.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/unit_command.hpp"
#include "sim/work_range.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace osc::sim {

namespace {

/// The box Moho's site queries hand the occupation grid: over the cells,
/// centred at height 0, 100 up and down (Steam 0x0063453B..0x006345D8 and
/// 0x0063486F..0x00634942).
OrientedBox site_box(const OccupancyRect& r) {
    const Vector3 centre{static_cast<f32>(r.x0 + r.x1) * 0.5f, 0.0f,
                         static_cast<f32>(r.z0 + r.z1) * 0.5f};
    const Vector3 half{static_cast<f32>(r.x1 - r.x0) * 0.5f, 100.0f,
                       static_cast<f32>(r.z1 - r.z0) * 0.5f};
    return oriented_box(centre, Quaternion{}, half);
}

/// Moho's COGrid::CollectEntitiesInBox for units: those whose collision
/// shape meets `box` (an unturned one), each once, in the collision cells'
/// order.
std::vector<Unit*> units_meeting(EntityRegistry& registry, const OrientedBox& box) {
    const f32 x0 = box.centre.x - box.extent[0];
    const f32 z0 = box.centre.z - box.extent[2];
    const f32 x1 = box.centre.x + box.extent[0];
    const f32 z1 = box.centre.z + box.extent[2];
    CollisionCells& cells = registry.collision_cells();
    std::vector<Entity*> gathered;
    if (cells.initialized()) {
        gathered = cells.gather(CollisionCells::box_rect(x0, z0, x1, z1), true, false);
    } else {
        constexpr f32 kReach = 32.0f; // past any unit's shape from its centre
        gathered = registry.units_in_rect(x0 - kReach, z0 - kReach, x1 + kReach, z1 + kReach);
    }
    std::vector<Unit*> out;
    for (Entity* e : gathered) {
        if (e->is_unit() && shape_meets_box(*e, box)) {
            out.push_back(static_cast<Unit*>(e));
        }
    }
    return out;
}

/// Moho's Unit::IsDead.
bool dead(const Unit& u) {
    return u.destroyed() || u.is_dying();
}

/// Whether a structure's skirt (RUnitBlueprint::GetSkirtRect) overlaps the
/// cells `r`, edges not counting (Steam 0x00634723..0x00634775).
bool skirt_overlaps(const Unit& u, const OccupancyRect& r) {
    const Vector3& at = u.position();
    const f32 sx0 =
        static_cast<f32>(std::lrint(at.x - u.footprint_size_x() * 0.5f)) + u.skirt_offset_x();
    const f32 sz0 =
        static_cast<f32>(std::lrint(at.z - u.footprint_size_z() * 0.5f)) + u.skirt_offset_z();
    const f32 sx1 = sx0 + u.skirt_size_x();
    const f32 sz1 = sz0 + u.skirt_size_z();
    const auto x0 = static_cast<f32>(r.x0);
    const auto z0 = static_cast<f32>(r.z0);
    const auto x1 = static_cast<f32>(r.x1);
    const auto z1 = static_cast<f32>(r.z1);
    return x1 > sx0 && sx1 > x0 && z1 > sz0 && sz1 > z0 && sx1 > sx0 && sz0 < sz1 && x1 > x0 &&
           z0 < z1;
}

/// What SIM_TryToBuild needs to test a place for one unit.
struct Search {
    SimState& sim;
    const map::Terrain& map;
    const Unit& unit;
    const blueprints::Footprint& fp;
    const OccupancyRect& site;
};

/// SIM_TryToBuild's test of one place (Steam 0x00634CD5..0x00634EFE): the
/// unit's footprint centred at (cx, cz), its corner cell rounded to nearest
/// as Moho's fistp does. A place touching the site, or on it, is none.
bool place_fits(const Search& s, f32 cx, f32 cz, OccupancyRect& out) {
    const i32 x0 = static_cast<i16>(std::lrint(cx - static_cast<f32>(s.fp.size_x) * 0.5f));
    const i32 z0 = static_cast<i16>(std::lrint(cz - static_cast<f32>(s.fp.size_z) * 0.5f));
    const OccupancyRect r{x0, z0, x0 + s.fp.size_x, z0 + s.fp.size_z};
    const OccupancyRect& b = s.site;
    if (r.x1 >= b.x0 && b.x1 >= r.x0 && r.z1 >= b.z0 && b.z1 >= r.z0 && b.x0 < b.x1 &&
        b.z0 < b.z1 && r.x0 < r.x1 && r.z0 < r.z1) {
        return false;
    }
    u8 caps = map_caps(s.fp, s.map, x0, z0);
    if (s.unit.layer() == "Water") {
        caps = static_cast<u8>(caps & ~blueprints::occupancy::kSub);
    }
    if (footprint_fits(s.fp, s.map, s.sim.occupancy(), x0, z0, caps) == 0 ||
        s.sim.occupancy().reserved_any(r, s.unit.reserved_ground()) ||
        !location_is_free(s.sim, &s.unit, r, false)) {
        return false;
    }
    out = r;
    return true;
}

/// SIM_TryToBuild's first search: the cells along the line from the unit to
/// (far_x, far_z), walked as FUN_0040D860's line steps (each axis mirrored
/// where the far end lies below the near one; Steam 0x00634C48..0x00634F7B).
bool place_along_line(const Search& s, f32 far_x, f32 far_z, OccupancyRect& out) {
    const Vector3& at = s.unit.position();
    const f32 near_x = at.x + 0.5f;
    const f32 near_z = at.z + 0.5f;
    const f32 end_x = far_x + 0.5f;
    const f32 end_z = far_z + 0.5f;
    const i32 mask_x = end_x < near_x ? -1 : 0;
    const i32 mask_z = end_z < near_z ? -1 : 0;
    const f32 from_x = mask_x != 0 ? -near_x : near_x;
    const f32 to_x = mask_x != 0 ? -end_x : end_x;
    const f32 from_z = mask_z != 0 ? -near_z : near_z;
    const f32 to_z = mask_z != 0 ? -end_z : end_z;
    const f32 span_x = to_x - from_x;
    const f32 span_z = to_z - from_z;
    auto cell_x = static_cast<i32>(std::floor(from_x));
    auto cell_z = static_cast<i32>(std::floor(from_z));
    for (;;) {
        if (place_fits(s, static_cast<f32>(mask_x ^ cell_x), static_cast<f32>(mask_z ^ cell_z),
                       out)) {
            return true;
        }
        const i32 next_x = cell_x + 1;
        const i32 next_z = cell_z + 1;
        const f32 err_x = (static_cast<f32>(next_x) - to_x) * span_z;
        const f32 err_z = (static_cast<f32>(next_z) - to_z) * span_x;
        if (err_z > err_x) {
            cell_x = next_x;
        } else {
            cell_z = next_z;
        }
        if (static_cast<f32>(cell_x) > to_x || static_cast<f32>(cell_z) > to_z) {
            return false;
        }
    }
}

/// SIM_TryToBuild's second search: square rings round the unit, four times
/// its footprint's larger side apart, rows along X, 900 places at most (the
/// count is looked at once a ring; Steam 0x00634F81..0x006353DB).
bool place_in_rings(const Search& s, OccupancyRect& out) {
    const i32 step = 4 * std::max<i32>(s.fp.size_x, s.fp.size_z);
    const Vector3& at = s.unit.position();
    i32 tested = 0;
    for (i32 radius = 1; tested < 900; ++radius) {
        for (i32 row = -radius; row <= radius; ++row) {
            const i32 col_step = row == -radius || row == radius ? 1 : 2 * radius;
            for (i32 col = -radius; col <= radius; col += col_step) {
                ++tested;
                const f32 x = at.x + static_cast<f32>(row * step);
                const f32 z = static_cast<f32>(col * step) + at.z;
                if (place_fits(s, x, z, out)) {
                    return true;
                }
            }
        }
    }
    return false;
}

} // namespace

bool location_is_free(SimState& sim, const Unit* ignore, const OccupancyRect& rect,
                      bool require_idle) {
    for (const Unit* u : units_meeting(sim.entity_registry(), site_box(rect))) {
        if (dead(*u) || u == ignore) {
            continue;
        }
        // Moho's idle here: no order at the head of its queue.
        if (require_idle && !u->command_queue().empty()) {
            continue;
        }
        if (u->layer() == "Air") {
            continue;
        }
        if (!u->is_mobile() && !skirt_overlaps(*u, rect)) {
            continue;
        }
        // What `ignore` carries doesn't count (TransportIsStoredUnit).
        if (!ignore || !u->transport_id() || u->transport_id() != ignore->entity_id()) {
            return false;
        }
    }
    return true;
}

void try_to_build(SimContext& ctx, i32 army, const OccupancyRect& rect, bool require_idle) {
    if (!ctx.sim || !ctx.terrain) {
        return;
    }
    SimState& sim = *ctx.sim;
    static const CategoryName kAir{"AIR"};
    const f32 centre_x = static_cast<f32>(rect.x0 + rect.x1) * 0.5f;
    const f32 centre_z = static_cast<f32>(rect.z0 + rect.z1) * 0.5f;
    const f32 half_x = static_cast<f32>(rect.x1 - rect.x0) * 0.5f;
    const f32 half_z = static_cast<f32>(rect.z1 - rect.z0) * 0.5f;
    const f32 push = half_z > half_x ? half_z : half_x;

    // Its own units at rest (a moving one clears the site by itself), or
    // aircraft, but none in the air (Steam 0x006349D8..0x00634A76).
    std::vector<Unit*> blockers;
    for (Unit* u : units_meeting(sim.entity_registry(), site_box(rect))) {
        if (!u->is_mobile()) {
            continue;
        }
        const bool rests = u->has_category(kAir) || !u->moved_last_tick();
        if (!rests || u->layer() == "Air" || dead(*u) || u->army() != army) {
            continue;
        }
        if (require_idle && !u->is_idle_state()) {
            continue;
        }
        blockers.push_back(u);
    }

    std::vector<OccupancyRect> held;
    for (Unit* u : blockers) {
        const Search s{sim, *ctx.terrain, *u, u->footprint(), rect};
        // Out along the line from the site's centre through the unit, three
        // times the site's larger half-extent (straight up the Z axis from
        // the very centre).
        const Vector3 at = u->position();
        const f32 dx = at.x - centre_x;
        const f32 dz = at.z - centre_z;
        f32 dir_x = 0.0f;
        f32 dir_z = 0.0f;
        if (dx == 0.0f && dz == 0.0f) {
            dir_z = 1.0f;
        } else if (const f32 len = std::sqrt(dx * dx + dz * dz); len > 0.000001f) {
            const f32 inv = 1.0f / len;
            dir_x = dx * inv;
            dir_z = dz * inv;
        }
        const f32 far_x = at.x + dir_x * push * 3.0f;
        const f32 far_z = at.z + dir_z * push * 3.0f;
        OccupancyRect placed;
        if (!place_along_line(s, far_x, far_z, placed) && !place_in_rings(s, placed)) {
            continue;
        }
        // Held while the next unit looks (Moho reserves it as the unit's).
        sim.reserve_ground(placed, true);
        held.push_back(placed);
        // Its navigator's goal is the place's corner cell. An aircraft's
        // would fly it there (Moho's air navigator): not ported, it stays.
        if (!u->can_fly()) {
            const blueprints::Footprint& fp = s.fp;
            u->step_aside({static_cast<f32>(placed.x0) + static_cast<f32>(fp.size_x) * 0.5f, at.y,
                           static_cast<f32>(placed.z0) + static_cast<f32>(fp.size_z) * 0.5f},
                          ctx);
        }
    }
    // Every place goes back once all have looked: only the goals stay
    // (Steam 0x00635501..0x0063551F).
    for (const OccupancyRect& r : held) {
        sim.reserve_ground(r, false);
    }
    // What the units had reserved before (a guard's anchor) stays theirs,
    // where Moho frees it.
    for (const Unit* u : blockers) {
        if (const OccupancyRect& own = u->reserved_ground(); own.x1 > own.x0 && own.z1 > own.z0) {
            sim.reserve_ground(own, true);
        }
    }
}

bool Unit::units_on_site(UnitCommand& cmd, SimContext& ctx) {
    if (!ctx.sim) {
        return false;
    }
    // The site's footprint cells (the task's mBuildRect).
    const auto [size_x, size_z] = blueprint_footprint_cells(ctx.L, cmd.blueprint_id);
    const auto x0 = static_cast<i32>(std::lrint(cmd.target_pos.x - size_x * 0.5f));
    const auto z0 = static_cast<i32>(std::lrint(cmd.target_pos.z - size_z * 0.5f));
    const OccupancyRect site{x0, z0, x0 + static_cast<i32>(size_x), z0 + static_cast<i32>(size_z)};
    if (location_is_free(*ctx.sim, this, site, false)) {
        return false;
    }
    // Its own units are sent off the footprint grown a cell at -Z and +X
    // alone: the shipped task grows z0 and x1, then x0 by nothing (it
    // subtracts and adds 1, Steam 0x005FE61C..0x005FE628), and z1 not at all.
    OccupancyRect clear = site;
    clear.z0 -= 1;
    clear.x1 += 1;
    try_to_build(ctx, army(), clear, false);
    ++cmd.site_tries;
    cmd.task_wait = kSiteRetryTicks;
    return true;
}

void Unit::step_aside(const Vector3& goal, const SimContext& ctx) {
    set_path_goal(goal, ctx);
    stepping_aside_ = true;
    aside_goal_ = navigator_.goal();
}

void Unit::keep_stepping_aside() {
    if (!stepping_aside_) {
        return;
    }
    // Moho's navigator walks on by itself; the engine's moves only when asked
    // each tick. Another goal, or none, ends the walk.
    const Vector3& goal = navigator_.goal();
    if (!navigator_.busy() || goal.x != aside_goal_.x || goal.z != aside_goal_.z) {
        stepping_aside_ = false;
        return;
    }
    if (!nav_requested_) {
        nav_update();
    }
}

} // namespace osc::sim
