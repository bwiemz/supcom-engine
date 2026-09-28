#include <catch2/catch_test_macros.hpp>

#include "map/heightmap.hpp"
#include "map/terrain.hpp"
#include "renderer/terrain_normal_maps.hpp"

#include <cmath>
#include <cstring>
#include <vector>

using namespace osc;
using renderer::combine_dxt5_tiles;
using renderer::terrain_normal_maps;

namespace {

/// A DXT5 DDS of w x h whose every block is filled with `fill`.
std::vector<char> dxt5(u32 w, u32 h, char fill) {
    std::vector<char> d(128 + static_cast<size_t>(w / 4) * (h / 4) * 16, fill);
    std::memset(d.data(), 0, 128);
    std::memcpy(d.data(), "DDS ", 4);
    const u32 size = 124;
    std::memcpy(d.data() + 4, &size, 4);
    std::memcpy(d.data() + 12, &h, 4);
    std::memcpy(d.data() + 16, &w, 4);
    std::memcpy(d.data() + 84, "DXT5", 4);
    return d;
}

u32 at(const std::vector<char>& d, size_t offset) {
    u32 v = 0;
    std::memcpy(&v, d.data() + offset, 4);
    return v;
}

map::Terrain ground(u32 size, f32 (*height)(u32, u32)) {
    constexpr f32 kScale = 1.0f / 128.0f;
    std::vector<u16> heights(static_cast<size_t>(size + 1) * (size + 1));
    for (u32 z = 0; z <= size; ++z)
        for (u32 x = 0; x <= size; ++x)
            heights[static_cast<size_t>(z) * (size + 1) + x] =
                static_cast<u16>(height(x, z) / kScale);
    return map::Terrain(map::Heightmap(size, size, kScale, std::move(heights)), 0.0f, false);
}

} // namespace

TEST_CASE("Normal-map tiles lie side by side, row by row", "[terrain_normal_maps]") {
    // Four 8x8 tiles, two a row (Moho's GetNormalMapInfo: tile i at
    // (i mod 2, i div 2)): the 16x16 whole's block rows take each tile's in
    // turn.
    const std::vector<std::vector<char>> tiles = {dxt5(8, 8, 'a'), dxt5(8, 8, 'b'), dxt5(8, 8, 'c'),
                                                  dxt5(8, 8, 'd')};
    const std::vector<char> whole = combine_dxt5_tiles(tiles, 8, 8, 2);
    REQUIRE(whole.size() == 128 + 4 * 4 * 16);
    CHECK(at(whole, 12) == 16); // height
    CHECK(at(whole, 16) == 16); // width
    CHECK(at(whole, 28) == 1);  // one mip
    const auto block = [&](u32 bx, u32 by) { return whole[128 + (by * 4 + bx) * 16]; };
    CHECK(block(0, 0) == 'a');
    CHECK(block(1, 1) == 'a');
    CHECK(block(2, 0) == 'b');
    CHECK(block(3, 1) == 'b');
    CHECK(block(0, 2) == 'c');
    CHECK(block(3, 3) == 'd');
}

TEST_CASE("Tiles that aren't DXT5 of the size given don't combine", "[terrain_normal_maps]") {
    CHECK(combine_dxt5_tiles({dxt5(8, 8, 'a'), dxt5(4, 4, 'b')}, 8, 8, 2).empty());
    std::vector<char> dxt1 = dxt5(8, 8, 'a');
    std::memcpy(dxt1.data() + 84, "DXT1", 4);
    CHECK(combine_dxt5_tiles({dxt1}, 8, 8, 1).empty());
    CHECK(combine_dxt5_tiles({}, 8, 8, 1).empty());
}

TEST_CASE("A map's one tile is used as it is", "[terrain_normal_maps]") {
    map::Terrain t = ground(16, [](u32, u32) { return 0.0f; });
    t.set_normal_maps({16, 16, {dxt5(16, 16, 'q')}});
    const auto maps = terrain_normal_maps(t);
    CHECK(maps.dds == t.normal_maps().tiles[0]);
    CHECK(maps.width == 16);
    CHECK(maps.tile_width == 16);
    CHECK(maps.rgba.empty());
}

TEST_CASE("A terrain without normal maps gets one from its heights", "[terrain_normal_maps]") {
    // Rising 1 in 2 along x: n = (-1, 2, 0) / sqrt(5), x in alpha, z in
    // green, a texel a unit.
    map::Terrain t = ground(16, [](u32 x, u32) { return 0.5f * static_cast<f32>(x); });
    const auto maps = terrain_normal_maps(t);
    REQUIRE(maps.dds.empty());
    REQUIRE(maps.width == 16);
    REQUIRE(maps.height == 16);
    CHECK(maps.tile_width == 16);
    const u8* texel = &maps.rgba[(8 * 16 + 8) * 4];
    const f32 x = texel[3] / 127.5f - 1.0f;
    const f32 z = texel[1] / 127.5f - 1.0f;
    CHECK(std::abs(x - (-1.0f / std::sqrt(5.0f))) < 0.01f);
    CHECK(std::abs(z) < 0.01f);
}
