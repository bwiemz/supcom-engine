#pragma once

#include "core/result.hpp"
#include "core/types.hpp"

#include <string>
#include <vector>

namespace osc::map {

/// A terrain stratum (texture layer) from the .scmap binary.
struct ScmapStratum {
    std::string albedo_path;   // VFS path e.g. "/env/evergreen/terrain/..."
    f32 albedo_scale = 10.0f;  // world units per texture tile
    std::string normal_path;
    f32 normal_scale = 10.0f;
};

/// A prop embedded in the .scmap binary (trees, rocks, debris, deposits).
struct ScmapProp {
    std::string blueprint_path; // e.g. "/env/evergreen/props/trees/pine06_prop.bp"
    f32 px, py, pz;            // world position
    f32 rot[9];                // 3x3 rotation matrix (row-major: rotX, rotY, rotZ)
    f32 sx, sy, sz;            // scale
};

/// A terrain decal from the .scmap binary (roads, craters, dirt patches).
struct ScmapDecal {
    u32 decal_id = 0;
    u32 decal_type = 0;
    std::string texture1_path;
    std::string texture2_path;
    f32 scale_x = 1, scale_y = 1, scale_z = 1;
    f32 position_x = 0, position_y = 0, position_z = 0;
    f32 rotation_x = 0, rotation_y = 0, rotation_z = 0;
    f32 cut_off_lod = 1000.0f;
    f32 near_cut_off_lod = 0.0f;
    u32 remove_tick = 0;
};

/// Data extracted from a .scmap file.
struct ScmapData {
    u32 map_width = 0;
    u32 map_height = 0;
    f32 height_scale = 0.0f;
    std::vector<u16> heightmap; // (map_width+1)*(map_height+1) samples
    bool has_water = false;
    f32 water_elevation = 0.0f;
    f32 water_deep_elevation = 0.0f;
    f32 water_abyss_elevation = 0.0f;
    i32 version_minor = 0;
    std::vector<ScmapProp> props;   // map props from .scmap binary
    std::vector<ScmapDecal> decals; // terrain decals from .scmap binary
    std::vector<ScmapStratum> strata;   // up to 10 strata (0-9)
    std::vector<char> blend_dds_0;      // strata 1-4 blend weights (raw DDS)
    std::vector<char> blend_dds_1;      // strata 5-8 blend weights (raw DDS)
};

/// Parse a .scmap file and extract heightmap, water data, and props.
Result<ScmapData> parse_scmap(const std::vector<u8>& file_data);

} // namespace osc::map
