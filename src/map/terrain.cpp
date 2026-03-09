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

void Terrain::set_strata(std::vector<StratumInfo> strata,
                         std::vector<char> blend0, std::vector<char> blend1) {
    strata_ = std::move(strata);
    blend_dds_0_ = std::move(blend0);
    blend_dds_1_ = std::move(blend1);
}

void Terrain::set_decals(std::vector<DecalInfo> decals) {
    decals_ = std::move(decals);
}

void Terrain::set_normal_decals(std::vector<NormalDecalInfo> decals) {
    normal_decals_ = std::move(decals);
}

} // namespace osc::map
