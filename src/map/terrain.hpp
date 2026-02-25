#pragma once

#include "map/heightmap.hpp"

namespace osc::map {

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

private:
    Heightmap heightmap_;
    f32 water_elevation_;
    bool has_water_;
};

} // namespace osc::map
