#pragma once

#include "core/result.hpp"
#include "core/types.hpp"

#include <string>
#include <utility>
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

/// A map's lighting (M210a): the 23 floats after its environment cubemaps,
/// as FA's shaders read them (terrain.fx CalculateLighting, mesh.fx
/// ComputeLight). The defaults are SCMP_009's, for a scene without a map.
struct ScmapLighting {
    f32 multiplier = 1.54f;                          ///< LightingMultiplier
    f32 sun_direction[3] = {0.616f, 0.559f, 0.555f}; ///< toward the sun, unit length
    f32 sun_ambience[3] = {0.0f, 0.0f, 0.0f};
    f32 sun_color[3] = {1.38f, 1.29f, 1.14f};
    f32 shadow_fill[3] = {0.54f, 0.54f, 0.70f};  ///< ShadowFillColor
    f32 specular[4] = {0.31f, 0.0f, 0.0f, 0.0f}; ///< SpecularColor
    f32 bloom = 0.036f;
    f32 fog_color[3] = {0.37f, 0.49f, 0.45f}; ///< unused in game (no FA shader fogs)
    f32 fog_start = 0.0f;
    f32 fog_end = 740.0f;
};

/// What a map draws with besides its terrain textures (M210a).
struct ScmapEnvironment {
    std::string terrain_shader; ///< "TTerrain" (the original maps) or "TTerrainXP"
    std::string background;     ///< background texture
    std::string sky_cubemap;
    /// Environment cubemaps by name ("<default>", "<aeon>", "<seraphim>").
    std::vector<std::pair<std::string, std::string>> cubemaps;
};

/// A map's water (for M213): the 20 floats and two textures after its
/// elevations.
struct ScmapWater {
    f32 surface_color[3] = {0.0f, 0.7f, 1.5f};
    f32 color_lerp[2] = {0.064f, 0.119f};
    f32 refraction_scale = 0.375f;
    f32 fresnel_bias = 0.15f;
    f32 fresnel_power = 1.5f;
    f32 unit_reflection = 0.5f;
    f32 sky_reflection = 1.5f;
    f32 sun_shininess = 50.0f;
    f32 sun_strength = 10.0f;
    f32 sun_direction[3] = {0.09954818f, -0.9626309f, 0.2518569f};
    f32 sun_color[3] = {0.8f, 0.7f, 0.5f};
    f32 sun_reflection = 5.0f;
    f32 sun_glow = 0.1f;
    std::string cubemap;
    std::string ramp;
    /// The four wave normal layers (M213a): how often each repeats a unit,
    /// how far it moves a tick, and its texture.
    f32 normal_repeat[4] = {0.0009f, 0.009f, 0.05f, 0.5f};
    f32 normal_movement[4][2] = {
        {0.5f, -0.95f}, {0.05f, -0.095f}, {0.01f, 0.03f}, {0.0005f, 0.0009f}};
    std::string normal_texture[4];
};

/// A map's water masks (M213a), half its size: foam (0 where unset),
/// flatness (255) and depth bias (127), one byte a texel.
struct ScmapWaterMasks {
    u32 width = 0, height = 0;
    std::vector<u8> foam, flatness, depth_bias;
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
    ScmapWater water; ///< meaningful when has_water
    ScmapWaterMasks water_masks;
    ScmapLighting lighting;
    ScmapEnvironment environment;
    i32 version_minor = 0;
    std::vector<ScmapProp> props;   // map props from .scmap binary
    std::vector<ScmapDecal> decals; // terrain decals from .scmap binary
    std::vector<ScmapStratum> strata;   // up to 10 strata (0-9)
    std::vector<char> blend_dds_0;      // strata 1-4 blend weights (raw DDS)
    std::vector<char> blend_dds_1;      // strata 5-8 blend weights (raw DDS)
    std::vector<char> preview_dds;      // lobby preview image (raw DDS, may be empty)
    /// Each map cell's terrain type: a TypeCode of /lua/TerrainTypes.lua,
    /// map_width x map_height, row by row.
    std::vector<u8> terrain_types;
};

/// Parse a .scmap file and extract heightmap, water data, and props.
Result<ScmapData> parse_scmap(const std::vector<u8>& file_data);

} // namespace osc::map
