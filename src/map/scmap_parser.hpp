#pragma once

#include "core/result.hpp"
#include "core/types.hpp"

#include <vector>

namespace osc::map {

/// Data extracted from a .scmap file.
struct ScmapData {
    u32 map_width = 0;
    u32 map_height = 0;
    f32 height_scale = 0.0f;
    std::vector<i16> heightmap; // (map_width+1)*(map_height+1) samples
    bool has_water = false;
    f32 water_elevation = 0.0f;
    f32 water_deep_elevation = 0.0f;
    f32 water_abyss_elevation = 0.0f;
    i32 version_minor = 0;
};

/// Parse a .scmap file and extract heightmap + water data.
/// Reads only the header, heightmap, and water sections; skips textures/decals/props.
Result<ScmapData> parse_scmap(const std::vector<u8>& file_data);

} // namespace osc::map
