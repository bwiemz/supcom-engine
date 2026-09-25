#pragma once

#include "core/types.hpp"

#include <optional>
#include <vector>

namespace osc::map {

/// Stores a heightmap grid and provides bilinear-interpolated height queries.
/// Grid dimensions are (map_width + 1) x (map_height + 1).
/// World coordinates (0..map_width, 0..map_height) map directly to grid coordinates.
class Heightmap {
public:
    Heightmap(u32 map_width, u32 map_height, f32 scale,
              std::vector<u16> raw_data);

    /// Bilinear-interpolated height at world position (x, z).
    /// Coordinates are clamped to the valid range.
    f32 get_height(f32 x, f32 z) const;

    /// Raw height at grid position (no interpolation, no bounds check).
    f32 get_height_at_grid(u32 gx, u32 gz) const;

    /// Where the line p + t * d first meets the terrain, for t in
    /// [start, end]: Moho's CHeightField::Intersection. Each grid cell is two
    /// triangles, split along its (x, z)-(x+1, z+1) diagonal. A line already
    /// under a triangle where it enters the cell meets the terrain where it
    /// entered. t is in d's units; d need not be unit length.
    std::optional<f32> intersect(f32 px, f32 py, f32 pz, f32 dx, f32 dy, f32 dz, f32 start,
                                 f32 end) const;

    /// The grid's highest point.
    f32 max_height() const { return max_height_; }

    u32 grid_width() const { return grid_width_; }
    u32 grid_height() const { return grid_height_; }
    u32 map_width() const { return grid_width_ - 1; }
    u32 map_height() const { return grid_height_ - 1; }
    f32 scale() const { return scale_; }

private:
    u32 grid_width_;   // map_width + 1
    u32 grid_height_;  // map_height + 1
    f32 scale_;        // raw_value * scale = world height
    std::vector<u16> data_; // row-major [gz * grid_width + gx]
    f32 max_height_ = 0.0f;
};

/// The path of a shot, for CAiBrain:CheckBlockingTerrain.
enum class ShotArc { Straight, Low, High };

/// Whether the terrain stands between a and b for a shot along `arc`:
/// Moho's CAiBrain:CheckBlockingTerrain. The start is lifted 1 and the end
/// 0.5. An arc is four chords of a half-sine over the span, whose peak is a
/// quarter of the span's length times 0.5 (Low) or 2 (High).
bool terrain_blocks_shot(const Heightmap& heightmap, f32 ax, f32 ay, f32 az, f32 bx, f32 by, f32 bz,
                         ShotArc arc);

} // namespace osc::map
