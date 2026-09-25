#include "sim/build_placement.hpp"
#include "core/dmath.hpp"

#include "map/pathfinding_grid.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"

#include <algorithm>
#include <cmath>

namespace osc::sim {

namespace {

/// Largest structure half-extent searched around a site (experimentals and
/// big factories stay well under this).
constexpr f32 MAX_STRUCTURE_HALF_EXTENT = 16.0f;

/// How far a structure's center may sit from a deposit and still use it.
constexpr f32 DEPOSIT_TOLERANCE = 1.0f;

} // namespace

bool StructureSite::overlaps(const StructureSite& o) const {
    // Touching edges is fine: FA packs structures edge to edge.
    return std::abs(x - o.x) * 2.0f < size_x + o.size_x &&
           std::abs(z - o.z) * 2.0f < size_z + o.size_z;
}

StructurePlacement::StructurePlacement(const SimState& sim, i32 army,
                                       PlacementRulesLookup rules)
    : sim_(sim), lookup_(std::move(rules)) {
    sim_.entity_registry().for_each_unit([&](const Entity& e) {
        if (e.destroyed() || !e.is_unit() || e.army() != army) return;
        const auto& unit = static_cast<const Unit&>(e);
        for (const auto& cmd : unit.command_queue()) {
            if (cmd.type != CommandType::BuildMobile || cmd.blueprint_id.empty()) continue;
            const auto& r = this->rules(cmd.blueprint_id);
            reserved_.push_back({cmd.target_pos.x, cmd.target_pos.z, r.size_x, r.size_z});
        }
    });
}

const PlacementRules& StructurePlacement::rules(const std::string& bp_id) const {
    auto it = rules_cache_.find(bp_id);
    if (it == rules_cache_.end())
        it = rules_cache_.emplace(bp_id, lookup_ ? lookup_(bp_id) : PlacementRules{}).first;
    return it->second;
}

bool StructurePlacement::can_build(const std::string& bp_id, f32 x, f32 z) const {
    const auto& r = rules(bp_id);
    const StructureSite site{x, z, r.size_x, r.size_z};
    if (!terrain_allows(r, site)) return false;
    if (r.deposit != PlacementRules::Deposit::None && !on_deposit(r, x, z)) return false;
    for (const auto& reserved : reserved_)
        if (reserved.overlaps(site)) return false;
    return !structure_overlaps(site);
}

bool StructurePlacement::terrain_allows(const PlacementRules& r,
                                        const StructureSite& site) const {
    const f32 x0 = site.x - site.size_x * 0.5f, x1 = site.x + site.size_x * 0.5f;
    const f32 z0 = site.z - site.size_z * 0.5f, z1 = site.z + site.size_z * 0.5f;
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
        grid->world_to_grid(site.x, site.z, cx, cz);
        gx0 = gx1 = cx;
        gz0 = gz1 = cz;
    }
    for (i64 gz = gz0; gz <= gz1; ++gz) {
        for (i64 gx = gx0; gx <= gx1; ++gx) {
            switch (grid->get(static_cast<u32>(gx), static_cast<u32>(gz))) {
            case map::CellPassability::Passable:
                if (!r.on_land) return false;
                break;
            case map::CellPassability::Water:
                if (!r.on_water) return false;
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
        0.5f * osc::dmath::hypot(site.size_x, site.size_z) + MAX_STRUCTURE_HALF_EXTENT;
    const auto& registry = sim_.entity_registry();
    for (const auto* e : registry.units_in_radius(site.x, site.z, reach)) {
        const auto& unit = static_cast<const Unit&>(*e);
        if (!unit.has_category("STRUCTURE") || unit.footprint_size_x() <= 0 ||
            unit.footprint_size_z() <= 0)
            continue;
        const StructureSite other{unit.position().x, unit.position().z,
                                  unit.footprint_size_x(), unit.footprint_size_z()};
        if (other.overlaps(site)) return true;
    }
    return false;
}

bool StructurePlacement::on_deposit(const PlacementRules& r, f32 x, f32 z) const {
    const auto want = r.deposit == PlacementRules::Deposit::Mass
                          ? ResourceDeposit::Mass : ResourceDeposit::Hydrocarbon;
    for (const auto& d : sim_.resource_deposits()) {
        if (d.type == want && std::abs(d.x - x) <= DEPOSIT_TOLERANCE &&
            std::abs(d.z - z) <= DEPOSIT_TOLERANCE)
            return true;
    }
    return false;
}

} // namespace osc::sim
