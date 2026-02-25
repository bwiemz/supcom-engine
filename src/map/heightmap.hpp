#pragma once

#include "core/types.hpp"

#include <vector>

namespace osc::map {

/// Stores a heightmap grid and provides bilinear-interpolated height queries.
/// Grid dimensions are (map_width + 1) x (map_height + 1).
/// World coordinates (0..map_width, 0..map_height) map directly to grid coordinates.
class Heightmap {
public:
    Heightmap(u32 map_width, u32 map_height, f32 scale,
              std::vector<i16> raw_data);

    /// Bilinear-interpolated height at world position (x, z).
    /// Coordinates are clamped to the valid range.
    f32 get_height(f32 x, f32 z) const;

    /// Raw height at grid position (no interpolation, no bounds check).
    f32 get_height_at_grid(u32 gx, u32 gz) const;

    u32 grid_width() const { return grid_width_; }
    u32 grid_height() const { return grid_height_; }
    u32 map_width() const { return grid_width_ - 1; }
    u32 map_height() const { return grid_height_ - 1; }
    f32 scale() const { return scale_; }

private:
    u32 grid_width_;   // map_width + 1
    u32 grid_height_;  // map_height + 1
    f32 scale_;        // raw_value * scale = world height
    std::vector<i16> data_; // row-major [gz * grid_width + gx]
};

} // namespace osc::map
