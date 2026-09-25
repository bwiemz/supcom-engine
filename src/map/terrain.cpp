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

f32 Terrain::water_ratio() const {
    // Moho's CalculateMapWaterRatio: with W heightfield vertices across, the
    // columns 8, 16, ... up to 8 * (((W - 1) >> 3) - 2), likewise the rows.
    const auto buckets = [](u32 vertices) { return static_cast<i32>((vertices - 1) >> 3) - 1; };
    const i32 columns = buckets(heightmap_.grid_width()) - 1;
    const i32 rows = buckets(heightmap_.grid_height()) - 1;
    if (columns <= 0 || rows <= 0) return 0.0f; // too small to sample (Moho: 0/0)
    const f32 water = has_water_ ? water_elevation_ : -10000.0f;
    u32 under = 0;
    for (i32 i = 1; i <= columns; ++i)
        for (i32 k = 1; k <= rows; ++k)
            if (water >
                heightmap_.get_height_at_grid(static_cast<u32>(8 * i), static_cast<u32>(8 * k)))
                ++under;
    return static_cast<f32>(under) / static_cast<f32>(columns * rows);
}

void Terrain::set_strata(std::vector<StratumInfo> strata,
                         std::vector<char> blend0, std::vector<char> blend1) {
    strata_ = std::move(strata);
    blend_dds_0_ = std::move(blend0);
    blend_dds_1_ = std::move(blend1);
}

void Terrain::set_terrain_types(std::vector<u8> types) {
    const size_t cells = static_cast<size_t>(map_width()) * map_height();
    terrain_types_ = types.size() == cells ? std::move(types) : std::vector<u8>{};
}

u8 Terrain::terrain_type(f32 x, f32 z) const {
    constexpr u8 kDefault = 1;
    if (terrain_types_.empty() || x < 0 || z < 0) return kDefault;
    const auto cx = static_cast<u32>(x);
    const auto cz = static_cast<u32>(z);
    if (cx >= map_width() || cz >= map_height()) return kDefault;
    return terrain_types_[static_cast<size_t>(cz) * map_width() + cx];
}

void Terrain::set_decals(std::vector<DecalInfo> decals) {
    decals_ = std::move(decals);
}

void Terrain::set_normal_decals(std::vector<NormalDecalInfo> decals) {
    normal_decals_ = std::move(decals);
}

} // namespace osc::map
