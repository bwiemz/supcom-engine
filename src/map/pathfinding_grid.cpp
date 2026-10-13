#include "map/pathfinding_grid.hpp"

#include <cstdint>
#include "map/heightmap.hpp"

#include <algorithm>
#include <cmath>

namespace osc::map {

PathfindingGrid::PathfindingGrid(const Heightmap& heightmap, f32 water_elevation, bool has_water,
                                 u32 cell_size, f32 slope_threshold)
    : cell_size_(cell_size), map_width_(heightmap.map_width()), map_height_(heightmap.map_height()),
      max_diff_(slope_threshold * static_cast<f32>(cell_size)), has_water_(has_water) {
    grid_width_ = map_width_ / cell_size_;
    grid_height_ = map_height_ / cell_size_;
    if (grid_width_ == 0) grid_width_ = 1;
    if (grid_height_ == 0) grid_height_ = 1;

    water_elevation_ = water_elevation;
    cells_.resize(grid_width_ * grid_height_);
    water_depth_.resize(grid_width_ * grid_height_, 0.0f);
    for (u32 gz = 0; gz < grid_height_; ++gz) {
        for (u32 gx = 0; gx < grid_width_; ++gx) {
            const u32 i = gz * grid_width_ + gx;
            classify_cell(heightmap, gx, gz, cells_[i], water_depth_[i]);
        }
    }
    base_cells_ = cells_;
}

void PathfindingGrid::classify_cell(const Heightmap& heightmap, u32 gx, u32 gz,
                                    CellPassability& pass, f32& depth) const {
    // Map cell to heightmap corners
    u32 hx0 = gx * cell_size_;
    u32 hz0 = gz * cell_size_;
    u32 hx1 = std::min(hx0 + cell_size_, heightmap.map_width());
    u32 hz1 = std::min(hz0 + cell_size_, heightmap.map_height());

    // Sample 4 corners of the cell from heightmap
    f32 h00 = heightmap.get_height_at_grid(hx0, hz0);
    f32 h10 = heightmap.get_height_at_grid(hx1, hz0);
    f32 h01 = heightmap.get_height_at_grid(hx0, hz1);
    f32 h11 = heightmap.get_height_at_grid(hx1, hz1);

    // Check max slope: height difference between adjacent corners
    f32 d1 = std::fabs(h10 - h00); // top edge
    f32 d2 = std::fabs(h01 - h00); // left edge
    f32 d3 = std::fabs(h11 - h10); // right edge
    f32 d4 = std::fabs(h11 - h01); // bottom edge
    f32 max_slope = std::max({d1, d2, d3, d4});

    // Underwater: the corners' average below the water (draft-aware naval
    // passability reads the depth)
    const f32 avg_h = (h00 + h10 + h01 + h11) * 0.25f;
    const bool under = has_water_ && avg_h < water_elevation_;
    depth = under ? water_elevation_ - avg_h : 0.0f;

    pass = CellPassability::Passable;
    if (max_slope > max_diff_) {
        pass = CellPassability::Impassable;
    } else if (under) {
        pass = CellPassability::Water;
    }
}

void PathfindingGrid::refresh_terrain(const Heightmap& heightmap, i32 x0, i32 z0, i32 x1, i32 z1) {
    if (x0 >= x1 || z0 >= z1) return;
    // The cells with a corner on the changed points (cell gx's corners are
    // points gx * cell_size and (gx + 1) * cell_size).
    const auto cs = static_cast<i32>(cell_size_);
    const auto lo = [cs](i32 p) { return std::max(p / cs - 1, 0); };
    const i32 gx0 = lo(x0);
    const i32 gz0 = lo(z0);
    const i32 gx1 = std::min((x1 - 1) / cs, static_cast<i32>(grid_width_) - 1);
    const i32 gz1 = std::min((z1 - 1) / cs, static_cast<i32>(grid_height_) - 1);
    bool changed = false;
    for (i32 gz = gz0; gz <= gz1; ++gz) {
        for (i32 gx = gx0; gx <= gx1; ++gx) {
            const auto i = static_cast<size_t>(gz) * grid_width_ + static_cast<size_t>(gx);
            CellPassability pass = CellPassability::Passable;
            f32 depth = 0.0f;
            classify_cell(heightmap, static_cast<u32>(gx), static_cast<u32>(gz), pass, depth);
            const bool covered = i < obstacle_refs_.size() && obstacle_refs_[i] > 0;
            const CellPassability now = covered ? CellPassability::Obstacle : pass;
            changed |= base_cells_[i] != pass || cells_[i] != now || water_depth_[i] != depth;
            base_cells_[i] = pass;
            cells_[i] = now;
            water_depth_[i] = depth;
        }
    }
    if (changed) ++version_;
}

CellPassability PathfindingGrid::get(u32 gx, u32 gz) const {
    if (gx >= grid_width_ || gz >= grid_height_)
        return CellPassability::Impassable;
    return cells_[gz * grid_width_ + gx];
}

bool PathfindingGrid::is_passable_for(u32 gx, u32 gz,
                                       const std::string& layer) const {
    return is_passable_for(gx, gz, layer, 0, false);
}

PathfindingGrid::MoveClass PathfindingGrid::classify(const std::string& layer, f32 draft,
                                                     bool amphibious) {
    MoveClass m;
    if (layer == "Air") m.kind = MoveClass::Kind::Air;
    else if (amphibious) m.kind = MoveClass::Kind::Amphibious;
    else if (layer == "Water" || layer == "Seabed" || layer == "Sub") {
        m.kind = MoveClass::Kind::Water;
        m.draft = draft;
    }
    return m;
}

bool PathfindingGrid::is_passable_for(u32 gx, u32 gz, const std::string& layer, f32 draft,
                                      bool amphibious) const {
    if (gx >= grid_width_ || gz >= grid_height_) return false;
    return passable_at(gz * grid_width_ + gx, classify(layer, draft, amphibious));
}

bool PathfindingGrid::terrain_passable_for(u32 gx, u32 gz, const std::string& layer, f32 draft,
                                           bool amphibious) const {
    if (gx >= grid_width_ || gz >= grid_height_) return false;
    return terrain_passable_at(gz * grid_width_ + gx, classify(layer, draft, amphibious));
}

f32 PathfindingGrid::water_depth(u32 gx, u32 gz) const {
    if (gx >= grid_width_ || gz >= grid_height_) return 0;
    return water_depth_[gz * grid_width_ + gx];
}

void PathfindingGrid::world_to_grid(f32 wx, f32 wz, u32& gx, u32& gz) const {
    f32 fx = wx / static_cast<f32>(cell_size_);
    f32 fz = wz / static_cast<f32>(cell_size_);
    gx = static_cast<u32>(std::max(0.0f, std::min(fx, static_cast<f32>(grid_width_ - 1))));
    gz = static_cast<u32>(std::max(0.0f, std::min(fz, static_cast<f32>(grid_height_ - 1))));
}

void PathfindingGrid::grid_to_world(u32 gx, u32 gz, f32& wx, f32& wz) const {
    // Return cell center
    wx = (static_cast<f32>(gx) + 0.5f) * static_cast<f32>(cell_size_);
    wz = (static_cast<f32>(gz) + 0.5f) * static_cast<f32>(cell_size_);
}

void PathfindingGrid::covered_cells(f32 wx, f32 wz, f32 sizeX, f32 sizeZ, u32& gx0, u32& gz0,
                                    u32& gx1, u32& gz1) const {
    const f32 cs = static_cast<f32>(cell_size_);
    const auto span = [&](f32 centre, f32 size, u32 cells, u32& lo, u32& hi) {
        const f32 first = std::ceil((centre - size * 0.5f) / cs - 0.5f);
        const f32 last = std::floor((centre + size * 0.5f) / cs - 0.5f);
        const f32 top = static_cast<f32>(cells - 1);
        if (first > last) {
            lo = hi = static_cast<u32>(std::clamp(std::floor(centre / cs), 0.0f, top));
            return;
        }
        lo = static_cast<u32>(std::clamp(first, 0.0f, top));
        hi = static_cast<u32>(std::clamp(last, 0.0f, top));
    };
    span(wx, sizeX, grid_width_, gx0, gx1);
    span(wz, sizeZ, grid_height_, gz0, gz1);
}

void PathfindingGrid::mark_obstacle(f32 wx, f32 wz, f32 sizeX, f32 sizeZ) {
    u32 gx0, gz0, gx1, gz1;
    covered_cells(wx, wz, sizeX, sizeZ, gx0, gz0, gx1, gz1);

    if (obstacle_refs_.size() != cells_.size()) obstacle_refs_.assign(cells_.size(), 0);
    bool changed = false;
    for (u32 z = gz0; z <= gz1; ++z) {
        for (u32 x = gx0; x <= gx1; ++x) {
            const size_t i = static_cast<size_t>(z) * grid_width_ + x;
            if (obstacle_refs_[i] < UINT16_MAX) ++obstacle_refs_[i];
            changed |= cells_[i] != CellPassability::Obstacle;
            cells_[i] = CellPassability::Obstacle;
        }
    }
    if (changed) ++version_;
}

void PathfindingGrid::clear_obstacle(f32 wx, f32 wz, f32 sizeX, f32 sizeZ) {
    if (obstacle_refs_.size() != cells_.size()) return; // nothing was marked
    u32 gx0, gz0, gx1, gz1;
    covered_cells(wx, wz, sizeX, sizeZ, gx0, gz0, gx1, gz1);

    bool changed = false;
    for (u32 z = gz0; z <= gz1; ++z) {
        for (u32 x = gx0; x <= gx1; ++x) {
            const size_t i = static_cast<size_t>(z) * grid_width_ + x;
            if (obstacle_refs_[i] == 0) continue;
            if (--obstacle_refs_[i] == 0) {
                cells_[i] = base_cells_[i]; // restore terrain passability
                changed = true;
            }
        }
    }
    if (changed) ++version_;
}

} // namespace osc::map
