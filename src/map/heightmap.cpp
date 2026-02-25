#include "map/heightmap.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace osc::map {

Heightmap::Heightmap(u32 map_width, u32 map_height, f32 scale,
                     std::vector<i16> raw_data)
    : grid_width_(map_width + 1),
      grid_height_(map_height + 1),
      scale_(scale),
      data_(std::move(raw_data)) {
    assert(map_width >= 1 && map_height >= 1);
    assert(data_.size() == static_cast<size_t>(grid_width_) * grid_height_);
}

f32 Heightmap::get_height_at_grid(u32 gx, u32 gz) const {
    return static_cast<f32>(data_[gz * grid_width_ + gx]) * scale_;
}

f32 Heightmap::get_height(f32 x, f32 z) const {
    // Clamp to valid world range
    f32 max_x = static_cast<f32>(grid_width_ - 1);
    f32 max_z = static_cast<f32>(grid_height_ - 1);
    x = std::clamp(x, 0.0f, max_x);
    z = std::clamp(z, 0.0f, max_z);

    // Grid integer coordinates
    u32 gx = static_cast<u32>(x);
    u32 gz = static_cast<u32>(z);

    // Clamp to avoid reading past the last grid cell
    if (gx >= grid_width_ - 1) gx = grid_width_ - 2;
    if (gz >= grid_height_ - 1) gz = grid_height_ - 2;

    // Fractional parts for interpolation
    f32 fx = x - static_cast<f32>(gx);
    f32 fz = z - static_cast<f32>(gz);

    // Four corner heights
    f32 h00 = get_height_at_grid(gx, gz);
    f32 h10 = get_height_at_grid(gx + 1, gz);
    f32 h01 = get_height_at_grid(gx, gz + 1);
    f32 h11 = get_height_at_grid(gx + 1, gz + 1);

    // Bilinear interpolation
    f32 h0 = h00 + (h10 - h00) * fx;
    f32 h1 = h01 + (h11 - h01) * fx;
    return h0 + (h1 - h0) * fz;
}

} // namespace osc::map
