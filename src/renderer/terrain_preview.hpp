#pragma once

#include "core/types.hpp"

#include <vector>

namespace osc::renderer {

/// Generate RGBA terrain preview from raw heightmap data.
/// heightmap array is (map_width+1) * (map_height+1) u16 samples (grid vertices).
/// map_width/map_height are in game units.
/// Returns output_size * output_size * 4 bytes of RGBA pixel data.
std::vector<u8> generate_terrain_preview(
    const u16* heightmap, u32 map_width, u32 map_height,
    f32 height_scale, f32 water_elevation, bool has_water,
    u32 output_size = 512);

} // namespace osc::renderer
