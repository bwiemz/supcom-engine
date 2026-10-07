#include "sim/path_tables.hpp"

#include "map/terrain.hpp"

#include <algorithm>

namespace osc::sim {

FootprintOccupation::FootprintOccupation(const blueprints::Footprint& fp,
                                         const map::Terrain& terrain, const OccupancyGrid& grid)
    : fp_(fp), cell_(fp), terrain_(terrain), grid_(grid) {
    fp_.size_x = std::max<u8>(fp_.size_x, 1);
    fp_.size_z = std::max<u8>(fp_.size_z, 1);
    cell_.size_x = 1;
    cell_.size_z = 1;
}

path::OccupationWindow FootprintOccupation::window(i32 x, i32 z) const {
    constexpr i32 kSpan = path::OccupationWindow::kSpan;
    const i32 size_x = fp_.size_x;
    const i32 size_z = fp_.size_z;
    // Row r, bit b: the class may stand with its corner on (x + b, z + r)
    // as far as row r's cells go. A closed cell closes every corner whose
    // footprint covers it, b from c - size_x + 1 to c.
    std::vector<u32> rows(static_cast<size_t>(size_z + kSpan - 1), (1U << kSpan) - 1);
    for (i32 r = 0; r < size_z + kSpan - 1; ++r) {
        for (i32 c = 0; c < size_x + kSpan - 1; ++c) {
            if (footprint_fits(cell_, terrain_, grid_, x + c, z + r) != 0) continue;
            const i32 lo = std::max(c - size_x + 1, 0);
            const i32 hi = std::min(c, kSpan - 1);
            if (lo <= hi) rows[static_cast<size_t>(r)] &= ~(((1U << (hi - lo + 1)) - 1) << lo);
        }
    }
    // Then down the rows its footprint covers.
    path::OccupationWindow w;
    for (i32 r = 0; r < kSpan; ++r) {
        u32 open = rows[static_cast<size_t>(r)];
        for (i32 dz = 1; dz < size_z; ++dz)
            open &= rows[static_cast<size_t>(r) + static_cast<size_t>(dz)];
        w.rows[static_cast<size_t>(r)] = static_cast<u16>(open);
    }
    return w;
}

PathTables::PathTables(const std::vector<blueprints::NamedFootprint>& classes,
                       const map::Terrain& terrain, const OccupancyGrid& grid) {
    sources_.reserve(classes.size());
    maps_.reserve(classes.size());
    for (const blueprints::NamedFootprint& fp : classes) {
        sources_.push_back(std::make_unique<FootprintOccupation>(fp, terrain, grid));
        maps_.push_back(std::make_unique<path::ClusterMap>(
            *sources_.back(), cache_, terrain.map_width(), terrain.map_height(),
            std::max<i32>(fp.size_x, 1), std::max<i32>(fp.size_z, 1)));
    }
}

void PathTables::dirty(const OccupancyRect& r) {
    for (const auto& m : maps_) m->dirty_rect(r);
}

void PathTables::update_background(i32 budget) {
    for (const auto& m : maps_) m->background_work(budget);
}

bool PathTables::done() const {
    return std::all_of(maps_.begin(), maps_.end(), [](const auto& m) { return m->done(); });
}

} // namespace osc::sim
