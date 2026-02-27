#pragma once

#include "core/types.hpp"
#include <string>
#include <vector>

namespace osc::map {

class Heightmap;

enum class CellPassability : u8 {
    Passable   = 0, // open terrain — land units can traverse
    Impassable = 1, // cliff / steep slope — blocks all ground units
    Water      = 2, // submerged — blocks land, allows naval
    Obstacle   = 3, // dynamic building footprint — blocks all ground
};

class PathfindingGrid {
public:
    /// Build passability grid from heightmap + water data.
    /// cell_size: world units per grid cell (default 2).
    /// slope_threshold: max height diff per world unit that is passable.
    PathfindingGrid(const Heightmap& heightmap, f32 water_elevation,
                    bool has_water, u32 cell_size = 2,
                    f32 slope_threshold = 0.75f);

    u32 grid_width() const { return grid_width_; }
    u32 grid_height() const { return grid_height_; }
    u32 cell_size() const { return cell_size_; }

    CellPassability get(u32 gx, u32 gz) const;

    /// Check if a cell is passable for a given movement layer.
    bool is_passable_for(u32 gx, u32 gz, const std::string& layer) const;

    /// Convert world position to grid coordinates (clamped).
    void world_to_grid(f32 wx, f32 wz, u32& gx, u32& gz) const;

    /// Convert grid coordinates to world position (cell center).
    void grid_to_world(u32 gx, u32 gz, f32& wx, f32& wz) const;

    /// Mark a rectangular footprint as Obstacle.
    /// (wx, wz) = center in world coords, sizeX/sizeZ in world units.
    void mark_obstacle(f32 wx, f32 wz, f32 sizeX, f32 sizeZ);

    /// Clear obstacle back to original terrain passability.
    void clear_obstacle(f32 wx, f32 wz, f32 sizeX, f32 sizeZ);

private:
    u32 grid_width_;
    u32 grid_height_;
    u32 cell_size_;
    u32 map_width_;
    u32 map_height_;
    std::vector<CellPassability> cells_;
    std::vector<CellPassability> base_cells_; // terrain-only (for restore)
};

} // namespace osc::map
