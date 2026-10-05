#include "sim/build_placement.hpp"
#include "core/dmath.hpp"

#include "map/pathfinding_grid.hpp"
#include "map/terrain.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"

#include <algorithm>
#include <cmath>

namespace osc::sim {

namespace {

/// Largest structure half-extent searched around a site (experimentals and
/// big factories stay well under this).
constexpr f32 MAX_STRUCTURE_HALF_EXTENT = 16.0f;

} // namespace

std::vector<std::pair<f32, f32>> structure_line_sites(f32 x0, f32 z0, f32 x1, f32 z1, f32 size_x,
                                                      f32 size_z, f32 spacing) {
    const f32 hx = size_x * 0.5f;
    const f32 hz = size_z * 0.5f;
    const f32 cx0 = std::nearbyint(x0 - hx);
    const f32 cz0 = std::nearbyint(z0 - hz);
    const f32 dx = std::nearbyint(x1 - hx) - cx0;
    const f32 dz = std::nearbyint(z1 - hz) - cz0;
    const f32 along = std::max(std::abs(dx), std::abs(dz));
    std::vector<std::pair<f32, f32>> sites;
    if (along == 0.0f || spacing <= 0.0f) {
        sites.emplace_back(cx0 + hx, cz0 + hz);
        return sites;
    }
    const int count = static_cast<int>(std::floor(along / spacing)) + 1;
    const f32 step_x = dx / along * spacing;
    const f32 step_z = dz / along * spacing;
    for (int i = 0; i < count; ++i) {
        const f32 k = static_cast<f32>(i);
        sites.emplace_back(std::nearbyint(cx0 + step_x * k) + hx,
                           std::nearbyint(cz0 + step_z * k) + hz);
    }
    return sites;
}

StructureSite StructureSite::of(f32 x, f32 z, f32 size_x, f32 size_z, f32 skirt_x, f32 skirt_z,
                                f32 off_x, f32 off_z) {
    const f32 x0 = x - size_x * 0.5f + std::min(off_x, 0.0f);
    const f32 z0 = z - size_z * 0.5f + std::min(off_z, 0.0f);
    return {x0, z0, x0 + std::max(skirt_x, size_x), z0 + std::max(skirt_z, size_z)};
}

StructureSite StructureSite::of(const PlacementRules& r, f32 x, f32 z) {
    return of(x, z, r.size_x, r.size_z, r.skirt_x, r.skirt_z, r.skirt_off_x, r.skirt_off_z);
}

std::optional<std::pair<f32, f32>> deposit_snap(const std::vector<ResourceDeposit>& deposits,
                                                PlacementRules::Deposit want, f32 x, f32 z,
                                                f32 size_x, f32 size_z, f32 radius) {
    if (want == PlacementRules::Deposit::None) {
        return std::nullopt;
    }
    const auto type = want == PlacementRules::Deposit::Mass ? ResourceDeposit::Mass
                                                            : ResourceDeposit::Hydrocarbon;
    const f32 cx = std::nearbyint(x - size_x * 0.5f);
    const f32 cz = std::nearbyint(z - size_z * 0.5f);
    std::optional<std::pair<f32, f32>> best;
    f32 best_dist = 0;
    for (const auto& d : deposits) {
        if (d.type != type) {
            continue;
        }
        const f32 half = std::floor(std::nearbyint(d.size) * 0.5f);
        const f32 dx = std::nearbyint(d.x - d.size * 0.5f) + half;
        const f32 dz = std::nearbyint(d.z - d.size * 0.5f) + half;
        const f32 dist = std::sqrt((dx - cx) * (dx - cx) + (dz - cz) * (dz - cz));
        if (dist <= radius && (!best || dist < best_dist)) {
            best = std::pair{dx + 0.5f, dz + 0.5f};
            best_dist = dist;
        }
    }
    return best;
}

f32 extract_snap_radius(f32 world_per_pixel) {
    if (world_per_pixel <= 0) {
        return 4.0f;
    }
    return std::clamp(4.0f / world_per_pixel, 20.0f, 90.0f) * world_per_pixel;
}

bool StructureSite::overlaps(const StructureSite& o) const {
    // Touching edges is fine: FA packs structures edge to edge.
    return x0 < o.x1 && o.x0 < x1 && z0 < o.z1 && o.z0 < z1;
}

bool StructureSite::touches(const StructureSite& o) const {
    const auto nested = [](f32 a0, f32 a1, f32 b0, f32 b1) {
        return (a0 >= b0 && b1 >= a1) || (b0 >= a0 && a1 >= b1);
    };
    if (std::abs(x0 - o.x1) < 1.0f || std::abs(x1 - o.x0) < 1.0f) {
        return nested(z0, z1, o.z0, o.z1);
    }
    if (std::abs(z0 - o.z1) < 1.0f || std::abs(z1 - o.z0) < 1.0f) {
        return nested(x0, x1, o.x0, o.x1);
    }
    return false;
}

StructurePlacement::StructurePlacement(const SimState& sim, i32 army, PlacementRulesLookup rules,
                                       bool scheduled)
    : sim_(sim), army_(army), lookup_(std::move(rules)), scheduled_(scheduled) {}

const std::vector<StructureSite>& StructurePlacement::reserved() const {
    if (reserved_) return *reserved_;
    auto& sites = reserved_.emplace();
    const auto reserve = [&](const UnitCommand& cmd) {
        if (cmd.type == CommandType::BuildMobile && !cmd.blueprint_id.empty()) {
            sites.push_back(
                StructureSite::of(rules(cmd.blueprint_id), cmd.target_pos.x, cmd.target_pos.z));
        }
    };
    const auto pending =
        scheduled_ ? sim_.queues_with_pending() : std::map<u32, SimState::QueueWithPending>();
    sim_.entity_registry().for_each_unit([&](const Entity& e) {
        if (e.destroyed() || !e.is_unit() || e.army() != army_) return;
        if (const auto it = pending.find(e.entity_id()); it != pending.end()) {
            for (const auto& cmd : it->second.orders) {
                reserve(cmd);
            }
            return;
        }
        for (const auto& cmd : static_cast<const Unit&>(e).command_queue()) {
            reserve(cmd);
        }
    });
    return sites;
}

const PlacementRules& StructurePlacement::rules(const std::string& bp_id) const {
    auto it = rules_cache_.find(bp_id);
    if (it == rules_cache_.end())
        it = rules_cache_.emplace(bp_id, lookup_ ? lookup_(bp_id) : PlacementRules{}).first;
    return it->second;
}

bool StructurePlacement::can_build(const std::string& bp_id, f32 x, f32 z) const {
    const auto& r = rules(bp_id);
    if (!terrain_allows(r, StructureSite::of(x, z, r.size_x, r.size_z))) return false;
    const StructureSite site = StructureSite::of(r, x, z);
    if (r.deposit != PlacementRules::Deposit::None && !on_deposit(r, x, z)) return false;
    for (const auto& pending : reserved())
        if (pending.overlaps(site)) return false;
    return !structure_overlaps(site);
}

bool StructurePlacement::terrain_allows(const PlacementRules& r,
                                        const StructureSite& site) const {
    const f32 x0 = site.x0, x1 = site.x1;
    const f32 z0 = site.z0, z1 = site.z1;
    if (sim_.has_playable_rect() &&
        (x0 < sim_.playable_x0() || x1 > sim_.playable_x1() ||
         z0 < sim_.playable_z0() || z1 > sim_.playable_z1()))
        return false;

    const auto* grid = sim_.pathfinding_grid();
    if (!grid) return true; // no terrain data: nothing to check against
    const f32 map_w = static_cast<f32>(grid->grid_width() * grid->cell_size());
    const f32 map_h = static_cast<f32>(grid->grid_height() * grid->cell_size());
    if (x0 < 0 || z0 < 0 || x1 > map_w || z1 > map_h) return false;

    // Cells whose centers lie inside the footprint (a 2x2 structure on a
    // 2-unit grid covers exactly its own cell, not its neighbours).
    const f32 cs = static_cast<f32>(grid->cell_size());
    const auto first = [cs](f32 lo) {
        return static_cast<u32>(std::max(0.0f, std::ceil(lo / cs - 0.5f)));
    };
    const auto last = [cs](f32 hi) {
        return static_cast<i64>(std::floor(hi / cs - 0.5f));
    };
    i64 gx0 = first(x0), gz0 = first(z0);
    i64 gx1 = std::min<i64>(last(x1), static_cast<i64>(grid->grid_width()) - 1);
    i64 gz1 = std::min<i64>(last(z1), static_cast<i64>(grid->grid_height()) - 1);
    if (gx1 < gx0 || gz1 < gz0) {
        // Smaller than a cell and between centers: judge by the cell it is in.
        u32 cx, cz;
        grid->world_to_grid((x0 + x1) * 0.5f, (z0 + z1) * 0.5f, cx, cz);
        gx0 = gx1 = cx;
        gz0 = gz1 = cz;
    }
    for (i64 gz = gz0; gz <= gz1; ++gz) {
        for (i64 gx = gx0; gx <= gx1; ++gx) {
            switch (grid->get(static_cast<u32>(gx), static_cast<u32>(gz))) {
            case map::CellPassability::Passable:
                if (!r.on_land) return false;
                break;
            case map::CellPassability::Water: // afloat, or on the ground under it
                if (!r.on_water && !r.on_seabed) return false;
                break;
            case map::CellPassability::Impassable:
            case map::CellPassability::Obstacle:
                return false;
            }
        }
    }
    return true;
}

bool StructurePlacement::structure_overlaps(const StructureSite& site) const {
    const f32 reach =
        0.5f * osc::dmath::hypot(site.x1 - site.x0, site.z1 - site.z0) + MAX_STRUCTURE_HALF_EXTENT;
    const auto& registry = sim_.entity_registry();
    for (const auto* e :
         registry.units_in_radius((site.x0 + site.x1) * 0.5f, (site.z0 + site.z1) * 0.5f, reach)) {
        const auto& unit = static_cast<const Unit&>(*e);
        if (!unit.has_category("STRUCTURE") || unit.footprint_size_x() <= 0 ||
            unit.footprint_size_z() <= 0) {
            continue;
        }
        const StructureSite other = StructureSite::of(
            unit.position().x, unit.position().z, unit.footprint_size_x(), unit.footprint_size_z(),
            unit.skirt_size_x(), unit.skirt_size_z(), unit.skirt_offset_x(), unit.skirt_offset_z());
        if (other.overlaps(site)) {
            return true;
        }
    }
    return false;
}

bool StructurePlacement::on_deposit(const PlacementRules& r, f32 x, f32 z) const {
    const auto want = r.deposit == PlacementRules::Deposit::Mass
                          ? ResourceDeposit::Mass : ResourceDeposit::Hydrocarbon;
    for (const auto& d : sim_.resource_deposits()) {
        if (d.type == want && std::abs(d.x - x) < 0.5f && std::abs(d.z - z) < 0.5f) {
            return true;
        }
    }
    return false;
}

// Moho: a footprint that can sit on the seabed occupies OC_SEABED, which
// BuildOnLayerCaps' LAYER_Seabed gives.
f32 structure_elevation(const SimState& sim, const PlacementRules& rules, f32 x, f32 z) {
    const auto* terrain = sim.terrain();
    if (!terrain) {
        return 0.0f;
    }
    return rules.on_seabed ? terrain->get_terrain_height(x, z) : terrain->get_surface_height(x, z);
}

} // namespace osc::sim
