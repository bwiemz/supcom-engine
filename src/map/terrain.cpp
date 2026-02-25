#include "map/terrain.hpp"

#include <algorithm>

namespace osc::map {

Terrain::Terrain(Heightmap heightmap, f32 water_elevation, bool has_water)
    : heightmap_(std::move(heightmap)),
      water_elevation_(water_elevation),
      has_water_(has_water) {}

f32 Terrain::get_terrain_height(f32 x, f32 z) const {
    return heightmap_.get_height(x, z);
}

f32 Terrain::get_surface_height(f32 x, f32 z) const {
    return std::max(heightmap_.get_height(x, z), water_elevation_);
}

} // namespace osc::map
