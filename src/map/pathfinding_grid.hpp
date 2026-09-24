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
    /// How a mover passes cells, resolved once from its layer, draft and
    /// amphibiousness (a search asks per cell, and a layer name per cell was
    /// the costliest part of it).
    struct MoveClass {
        enum class Kind : u8 { Air, Land, Water, Amphibious };
        Kind kind = Kind::Land;
        f32 draft = 0; ///< Water only: the depth it needs (0: any water)
    };
    static MoveClass classify(const std::string& layer, f32 draft, bool amphibious);

    /// is_passable_for on a cell index (z * grid_width + x), for a resolved
    /// mover.
    bool passable_at(u32 index, const MoveClass& m) const {
        return passable_cell(cells_[index], index, m);
    }
    /// As passable_at, on the terrain alone.
    bool terrain_passable_at(u32 index, const MoveClass& m) const {
        return passable_cell(base_cells_[index], index, m);
    }

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

    /// Draft-aware passability check for naval units.
    bool is_passable_for(u32 gx, u32 gz, const std::string& layer,
                         f32 draft, bool amphibious) const;

    /// is_passable_for on the terrain alone, as if no structure stood there.
    bool terrain_passable_for(u32 gx, u32 gz, const std::string& layer, f32 draft,
                              bool amphibious) const;

    /// Get the water depth at a grid cell (0 if land).
    f32 water_depth(u32 gx, u32 gz) const;

    /// Convert world position to grid coordinates (clamped).
    void world_to_grid(f32 wx, f32 wz, u32& gx, u32& gz) const;

    /// Convert grid coordinates to world position (cell center).
    void grid_to_world(u32 gx, u32 gz, f32& wx, f32& wz) const;

    /// Mark a rectangular footprint as Obstacle.
    /// (wx, wz) = center in world coords, sizeX/sizeZ in world units.
    /// Obstacles are reference-counted per cell: adjacent footprints can
    /// share a border cell after rounding, and it must stay blocked until
    /// every footprint covering it is cleared.
    void mark_obstacle(f32 wx, f32 wz, f32 sizeX, f32 sizeZ);

    /// Undo one mark_obstacle() with the same rectangle. A cell returns to
    /// its terrain passability once no footprint covers it.
    void clear_obstacle(f32 wx, f32 wz, f32 sizeX, f32 sizeZ);

    /// Changes whenever a cell's passability does (obstacles placed or
    /// cleared), so data derived from the grid can tell it is stale.
    u64 version() const { return version_; }

private:
    bool passable_cell(CellPassability cell, u32 index, const MoveClass& m) const {
        switch (m.kind) {
        case MoveClass::Kind::Air: return true;
        case MoveClass::Kind::Amphibious:
            return cell == CellPassability::Passable || cell == CellPassability::Water;
        case MoveClass::Kind::Water:
            return cell == CellPassability::Water &&
                   (m.draft <= 0 || water_depth_[index] >= m.draft);
        case MoveClass::Kind::Land: break;
        }
        return cell == CellPassability::Passable;
    }

    u32 grid_width_;
    u32 grid_height_;
    u32 cell_size_;
    u32 map_width_;
    u32 map_height_;
    std::vector<CellPassability> cells_;
    std::vector<CellPassability> base_cells_; // terrain-only (for restore)
    std::vector<u16> obstacle_refs_;           // footprints covering each cell
    std::vector<f32> water_depth_;
    f32 water_elevation_ = 0;
    u64 version_ = 0;
};

} // namespace osc::map
