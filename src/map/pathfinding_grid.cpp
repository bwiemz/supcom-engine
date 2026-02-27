#include "map/pathfinding_grid.hpp"
#include "map/heightmap.hpp"

#include <algorithm>
#include <cmath>

namespace osc::map {

PathfindingGrid::PathfindingGrid(const Heightmap& heightmap,
                                 f32 water_elevation, bool has_water,
                                 u32 cell_size, f32 slope_threshold)
    : cell_size_(cell_size),
      map_width_(heightmap.map_width()),
      map_height_(heightmap.map_height()) {
    grid_width_ = map_width_ / cell_size_;
    grid_height_ = map_height_ / cell_size_;
    if (grid_width_ == 0) grid_width_ = 1;
    if (grid_height_ == 0) grid_height_ = 1;

    const f32 max_diff = slope_threshold * static_cast<f32>(cell_size_);
    cells_.resize(grid_width_ * grid_height_);

    for (u32 gz = 0; gz < grid_height_; ++gz) {
        for (u32 gx = 0; gx < grid_width_; ++gx) {
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

            CellPassability pass = CellPassability::Passable;

            if (max_slope > max_diff) {
                pass = CellPassability::Impassable;
            } else if (has_water) {
                // Check if cell is underwater (average height below water)
                f32 avg_h = (h00 + h10 + h01 + h11) * 0.25f;
                if (avg_h < water_elevation) {
                    pass = CellPassability::Water;
                }
            }

            cells_[gz * grid_width_ + gx] = pass;
        }
    }

    base_cells_ = cells_;
}

CellPassability PathfindingGrid::get(u32 gx, u32 gz) const {
    if (gx >= grid_width_ || gz >= grid_height_)
        return CellPassability::Impassable;
    return cells_[gz * grid_width_ + gx];
}

bool PathfindingGrid::is_passable_for(u32 gx, u32 gz,
                                       const std::string& layer) const {
    if (gx >= grid_width_ || gz >= grid_height_) return false;
    auto cell = cells_[gz * grid_width_ + gx];

    if (layer == "Air") return true;

    if (layer == "Water" || layer == "Seabed" || layer == "Sub") {
        return cell == CellPassability::Water;
    }

    // Land (default): only passable terrain
    return cell == CellPassability::Passable;
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

void PathfindingGrid::mark_obstacle(f32 wx, f32 wz, f32 sizeX, f32 sizeZ) {
    // Convert world-space rectangle to grid cells
    f32 half_x = sizeX * 0.5f;
    f32 half_z = sizeZ * 0.5f;
    u32 gx0, gz0, gx1, gz1;
    world_to_grid(wx - half_x, wz - half_z, gx0, gz0);
    world_to_grid(wx + half_x, wz + half_z, gx1, gz1);

    for (u32 z = gz0; z <= gz1; ++z) {
        for (u32 x = gx0; x <= gx1; ++x) {
            cells_[z * grid_width_ + x] = CellPassability::Obstacle;
        }
    }
}

void PathfindingGrid::clear_obstacle(f32 wx, f32 wz, f32 sizeX, f32 sizeZ) {
    f32 half_x = sizeX * 0.5f;
    f32 half_z = sizeZ * 0.5f;
    u32 gx0, gz0, gx1, gz1;
    world_to_grid(wx - half_x, wz - half_z, gx0, gz0);
    world_to_grid(wx + half_x, wz + half_z, gx1, gz1);

    for (u32 z = gz0; z <= gz1; ++z) {
        for (u32 x = gx0; x <= gx1; ++x) {
            // Restore to original terrain passability
            cells_[z * grid_width_ + x] = base_cells_[z * grid_width_ + x];
        }
    }
}

} // namespace osc::map
