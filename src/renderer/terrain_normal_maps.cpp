#include "renderer/terrain_normal_maps.hpp"

#include "map/terrain.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace osc::renderer {

namespace {

constexpr size_t kDdsHeader = 128; // "DDS " and the 124-byte header
constexpr size_t kDxt5Block = 16;  // bytes a 4x4 block

u32 read_u32(const std::vector<char>& d, size_t at) {
    u32 v = 0;
    std::memcpy(&v, d.data() + at, sizeof(v));
    return v;
}

void write_u32(std::vector<char>& d, size_t at, u32 v) {
    std::memcpy(d.data() + at, &v, sizeof(v));
}

/// A DXT5 DDS of `w` x `h`, with room for its mip 0.
bool is_dxt5(const std::vector<char>& d, u32 w, u32 h) {
    if (d.size() < kDdsHeader || std::memcmp(d.data(), "DDS ", 4) != 0) return false;
    if (read_u32(d, 12) != h || read_u32(d, 16) != w) return false;
    if (std::memcmp(d.data() + 84, "DXT5", 4) != 0) return false;
    return d.size() >= kDdsHeader + static_cast<size_t>(w / 4) * (h / 4) * kDxt5Block;
}

/// The terrain's normal at (x, z): central differences of its height a unit
/// either side, as the terrain mesh's vertices take theirs.
void normal_at(const map::Terrain& terrain, f32 x, f32 z, f32& nx, f32& nz) {
    const f32 dx =
        terrain.get_terrain_height(x - 1.0f, z) - terrain.get_terrain_height(x + 1.0f, z);
    const f32 dz =
        terrain.get_terrain_height(x, z - 1.0f) - terrain.get_terrain_height(x, z + 1.0f);
    const f32 length = std::sqrt(dx * dx + 4.0f + dz * dz);
    nx = dx / length;
    nz = dz / length;
}

u8 unorm(f32 v) {
    return static_cast<u8>(std::clamp(std::lround((v * 0.5f + 0.5f) * 255.0f), 0L, 255L));
}

} // namespace

std::vector<char> combine_dxt5_tiles(const std::vector<std::vector<char>>& tiles, u32 tile_w,
                                     u32 tile_h, u32 per_row) {
    if (tiles.empty() || per_row == 0 || tile_w % 4 != 0 || tile_h % 4 != 0) return {};
    for (const auto& t : tiles)
        if (!is_dxt5(t, tile_w, tile_h)) return {};
    const u32 rows = static_cast<u32>((tiles.size() + per_row - 1) / per_row);
    const u32 width = tile_w * per_row;
    const u32 height = tile_h * rows;
    const size_t tile_row_bytes = static_cast<size_t>(tile_w / 4) * kDxt5Block;
    const size_t row_bytes = static_cast<size_t>(width / 4) * kDxt5Block;
    std::vector<char> out(kDdsHeader + row_bytes * (height / 4), 0);
    std::memcpy(out.data(), tiles[0].data(), kDdsHeader);
    write_u32(out, 12, height);
    write_u32(out, 16, width);
    write_u32(out, 20, static_cast<u32>(row_bytes * (height / 4))); // linear size
    write_u32(out, 28, 1);                                          // one mip
    for (size_t t = 0; t < tiles.size(); ++t) {
        const size_t tx = t % per_row;
        const size_t ty = t / per_row;
        for (u32 r = 0; r < tile_h / 4; ++r) {
            const size_t block_row = ty * (tile_h / 4) + r;
            std::memcpy(out.data() + kDdsHeader + block_row * row_bytes + tx * tile_row_bytes,
                        tiles[t].data() + kDdsHeader + r * tile_row_bytes, tile_row_bytes);
        }
    }
    return out;
}

TerrainNormalMaps terrain_normal_maps(const map::Terrain& terrain) {
    TerrainNormalMaps out;
    const auto& maps = terrain.normal_maps();
    const u32 map_w = terrain.map_width();
    const u32 map_h = terrain.map_height();
    if (!maps.tiles.empty() && maps.tile_width > 0 && maps.tile_height > 0) {
        const u32 per_row = std::max(1u, map_w / maps.tile_width);
        out.dds =
            maps.tiles.size() == 1 && is_dxt5(maps.tiles[0], maps.tile_width, maps.tile_height)
                ? maps.tiles[0]
                : combine_dxt5_tiles(maps.tiles, maps.tile_width, maps.tile_height, per_row);
        if (!out.dds.empty()) {
            out.width = read_u32(out.dds, 16);
            out.height = read_u32(out.dds, 12);
            out.tile_width = maps.tile_width;
            out.tile_height = maps.tile_height;
            return out;
        }
        spdlog::warn("Normal maps: {} tiles of {}x{} aren't DXT5; made from the heights",
                     maps.tiles.size(), maps.tile_width, maps.tile_height);
    }
    // Made from the heights: texel (i, j) the normal at the world's (i + 0.5,
    // j + 0.5), where Moho's UV (world / width) puts its centre.
    out.width = out.tile_width = std::max(1u, map_w);
    out.height = out.tile_height = std::max(1u, map_h);
    out.rgba.resize(static_cast<size_t>(out.width) * out.height * 4);
    for (u32 j = 0; j < out.height; ++j)
        for (u32 i = 0; i < out.width; ++i) {
            f32 nx = 0;
            f32 nz = 0;
            normal_at(terrain, static_cast<f32>(i) + 0.5f, static_cast<f32>(j) + 0.5f, nx, nz);
            u8* texel = &out.rgba[(static_cast<size_t>(j) * out.width + i) * 4];
            texel[0] = 128;
            texel[1] = unorm(nz);
            texel[2] = 128;
            texel[3] = unorm(nx);
        }
    return out;
}

} // namespace osc::renderer
