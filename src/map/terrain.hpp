#pragma once

#include "map/heightmap.hpp"

#include <string>
#include <vector>

namespace osc::map {

/// Info about a single terrain stratum (texture layer) for rendering.
struct StratumInfo {
    std::string albedo_path;
    f32 albedo_scale = 10.0f;
    std::string normal_path;
    f32 normal_scale = 10.0f;  // parsed for completeness; renderer uses albedo_scale for normal UVs (push constant limit)
};

/// A map decal for rendering (static, not simulated).
struct DecalInfo {
    std::string texture_path;
    f32 position_x, position_y, position_z;
    f32 scale_x, scale_y, scale_z;
    f32 rotation_x, rotation_y, rotation_z;
    f32 cut_off_lod = 1000.0f;
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

private:
    Heightmap heightmap_;
    f32 water_elevation_;
    bool has_water_;
    std::vector<StratumInfo> strata_;
    std::vector<char> blend_dds_0_;
    std::vector<char> blend_dds_1_;
    std::vector<DecalInfo> decals_;
};

} // namespace osc::map
