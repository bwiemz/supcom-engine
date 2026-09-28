#pragma once

#include "map/heightmap.hpp"
#include "map/scmap_parser.hpp"

#include <string>
#include <utility>
#include <vector>

namespace osc::map {

/// Info about a single terrain stratum (texture layer) for rendering.
struct StratumInfo {
    std::string albedo_path;
    f32 albedo_scale = 10.0f;
    std::string normal_path;
    f32 normal_scale = 10.0f; // the normal map repeats every normal_scale world units (M212a)
};

/// A map decal's type, as Moho's CWldTerrainDecal names them (M212b).
enum class DecalType : u32 {
    Undefined = 0,
    Albedo = 1,
    Normals = 2,
    WaterMask = 3,
    WaterAlbedo = 4,
    WaterNormals = 5,
    Glow = 6,
    AlphaNormals = 7,
    GlowMask = 8,
    AlbedoXP = 9,
};

/// A type that draws into the terrain's normals (TDecalsNormals and its
/// Alpha kin), not its colour.
inline bool normal_decal(DecalType t) {
    return t == DecalType::Normals || t == DecalType::AlphaNormals;
}

/// A type drawn lit over the terrain (TDecals, TDecalsXP; M212b).
inline bool lit_decal(DecalType t) {
    return t == DecalType::Albedo || t == DecalType::AlbedoXP;
}

/// A map decal for rendering (static, not simulated). It is placed by its
/// corner: its footprint runs from its position along its x and z axes.
struct DecalInfo {
    DecalType type = DecalType::Albedo;
    std::string texture_path;  ///< its first texture: the albedo
    std::string texture2_path; ///< its second: the specular (empty: none)
    f32 position_x = 0, position_y = 0, position_z = 0;
    f32 scale_x = 1, scale_y = 1, scale_z = 1;
    f32 rotation_x = 0, rotation_y = 0, rotation_z = 0;
    f32 cut_off_lod = 1000.0f;
    f32 near_cut_off_lod = 0.0f;
};

/// A normal-map decal for terrain normal perturbation (decal_type == 2).
struct NormalDecalInfo {
    std::string texture_path;
    f32 position_x, position_z;  // World-space XZ center
    f32 scale_x, scale_z;        // World-space footprint
    f32 rotation_y;              // Y-axis rotation in radians
};

/// Terrain system combining heightmap and water data.
/// Provides the queries used by simulation code (GetTerrainHeight, GetSurfaceHeight).
class Terrain {
public:
    Terrain(Heightmap heightmap, f32 water_elevation, bool has_water = false);

    /// Raw terrain height at world position (can be below water).
    f32 get_terrain_height(f32 x, f32 z) const;

    /// Surface height: max(terrain_height, water_elevation).
    f32 get_surface_height(f32 x, f32 z) const;

    f32 water_elevation() const { return water_elevation_; }
    bool has_water() const { return has_water_; }

    /// The share of the map under water, as Moho's brain:GetMapWaterRatio
    /// measures it: heightfield vertices every 8 units, the border ring
    /// left out, below the water's surface (none if the map has no water).
    f32 water_ratio() const;

    const Heightmap& heightmap() const { return heightmap_; }
    u32 map_width() const { return heightmap_.map_width(); }
    u32 map_height() const { return heightmap_.map_height(); }

    /// Set terrain stratum data for rendering.
    void set_strata(std::vector<StratumInfo> strata,
                    std::vector<char> blend0, std::vector<char> blend1);

    const std::vector<StratumInfo>& strata() const { return strata_; }
    const std::vector<char>& blend_dds_0() const { return blend_dds_0_; }
    const std::vector<char>& blend_dds_1() const { return blend_dds_1_; }

    /// Set terrain decal data for rendering.
    void set_decals(std::vector<DecalInfo> decals);
    const std::vector<DecalInfo>& decals() const { return decals_; }

    void set_normal_decals(std::vector<NormalDecalInfo> decals);
    const std::vector<NormalDecalInfo>& normal_decals() const { return normal_decals_; }

    /// The map's lighting and environment (M210a): SCMP_009's until a map
    /// sets them.
    void set_lighting(const ScmapLighting& lighting, ScmapEnvironment environment) {
        lighting_ = lighting;
        environment_ = std::move(environment);
    }
    const ScmapLighting& lighting() const { return lighting_; }
    const ScmapEnvironment& environment() const { return environment_; }

    /// The map's water (M213a): its parameters, masks, and the elevation of
    /// its abyss (the depth the water map measures down to).
    void set_water(ScmapWater water, ScmapWaterMasks masks, f32 abyss_elevation) {
        water_ = std::move(water);
        water_masks_ = std::move(masks);
        water_abyss_elevation_ = abyss_elevation;
    }
    const ScmapWater& water() const { return water_; }
    const ScmapWaterMasks& water_masks() const { return water_masks_; }
    f32 water_abyss_elevation() const { return water_abyss_elevation_; }

    /// The map's terrain types, one TypeCode per map cell, row by row.
    void set_terrain_types(std::vector<u8> types);
    /// The terrain type at a world position: its TypeCode in
    /// /lua/TerrainTypes.lua. Off the map, or on a map without the layer,
    /// it is 1 ('Default').
    u8 terrain_type(f32 x, f32 z) const;

private:
    Heightmap heightmap_;
    f32 water_elevation_;
    bool has_water_;
    std::vector<StratumInfo> strata_;
    std::vector<char> blend_dds_0_;
    std::vector<char> blend_dds_1_;
    std::vector<DecalInfo> decals_;
    std::vector<NormalDecalInfo> normal_decals_;
    std::vector<u8> terrain_types_;
    ScmapLighting lighting_;
    ScmapEnvironment environment_;
    ScmapWater water_;
    ScmapWaterMasks water_masks_;
    f32 water_abyss_elevation_ = 0.0f;
};

} // namespace osc::map
