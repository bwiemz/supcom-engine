#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "map/heightmap.hpp"
#include "map/scmap_parser.hpp"
#include "map/terrain.hpp"

#include <array>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <vector>

using namespace osc;
using namespace osc::map;
using Catch::Matchers::WithinAbs;

// ================================================================
// Heightmap tests
// ================================================================

TEST_CASE("Heightmap grid point queries", "[map]") {
    // 2x2 map → 3x3 grid
    // Grid:
    //   100  200  300
    //   400  500  600
    //   700  800  900
    std::vector<u16> data = {100, 200, 300, 400, 500, 600, 700, 800, 900};
    f32 scale = 1.0f; // 1:1 for easy math

    Heightmap hm(2, 2, scale, data);

    REQUIRE(hm.map_width() == 2);
    REQUIRE(hm.map_height() == 2);
    REQUIRE(hm.grid_width() == 3);
    REQUIRE(hm.grid_height() == 3);

    // Exact grid points
    CHECK_THAT(hm.get_height(0, 0), WithinAbs(100.0, 0.01));
    CHECK_THAT(hm.get_height(1, 0), WithinAbs(200.0, 0.01));
    CHECK_THAT(hm.get_height(2, 0), WithinAbs(300.0, 0.01));
    CHECK_THAT(hm.get_height(0, 1), WithinAbs(400.0, 0.01));
    CHECK_THAT(hm.get_height(1, 1), WithinAbs(500.0, 0.01));
    CHECK_THAT(hm.get_height(2, 2), WithinAbs(900.0, 0.01));
}

TEST_CASE("Heightmap bilinear interpolation", "[map]") {
    // 2x2 map → 3x3 grid, scale = 0.1
    std::vector<u16> data = {0, 100, 0, 0, 100, 0, 0, 0, 0};
    f32 scale = 0.1f;

    Heightmap hm(2, 2, scale, data);

    // Grid point (1,0) = 100 * 0.1 = 10.0
    CHECK_THAT(hm.get_height(1, 0), WithinAbs(10.0, 0.01));

    // Midpoint between (0,0)=0 and (1,0)=10 → should be 5.0
    CHECK_THAT(hm.get_height(0.5f, 0), WithinAbs(5.0, 0.01));

    // Midpoint between (1,0)=10 and (1,1)=10 → should be 10.0
    CHECK_THAT(hm.get_height(1, 0.5f), WithinAbs(10.0, 0.01));

    // Center of first cell: bilinear of (0,0)=0, (1,0)=10, (0,1)=0, (1,1)=10
    // fx=0.5, fz=0.5 → h0 = lerp(0,10,0.5)=5, h1 = lerp(0,10,0.5)=5, result=5
    CHECK_THAT(hm.get_height(0.5f, 0.5f), WithinAbs(5.0, 0.01));
}

TEST_CASE("Heightmap clamps out-of-range coordinates", "[map]") {
    std::vector<u16> data = {10, 20, 30, 40};
    Heightmap hm(1, 1, 1.0f, data); // 1x1 map → 2x2 grid

    // Negative coords should clamp to (0,0) = 10
    CHECK_THAT(hm.get_height(-5.0f, -5.0f), WithinAbs(10.0, 0.01));

    // Beyond max should clamp to (1,1) = 40
    CHECK_THAT(hm.get_height(10.0f, 10.0f), WithinAbs(40.0, 0.01));
}

TEST_CASE("Heightmap with real-world scale", "[map]") {
    // Typical SupCom scale: 1/128
    f32 scale = 1.0f / 128.0f;
    std::vector<u16> data = {0, 0, 0, 3200, 0, 0, 0, 0, 0};
    Heightmap hm(2, 2, scale, data);

    // Grid point (0,1) = 3200 / 128 = 25.0
    CHECK_THAT(hm.get_height(0, 1), WithinAbs(25.0, 0.01));
}

TEST_CASE("The map's water ratio samples as Moho's does", "[map]") {
    // A 64x64 map: Moho samples columns and rows 8..48 (6 x 6), not the
    // border. Low ground (height 0) where x <= 24, and along the far edge
    // (x >= 56), which isn't sampled; the rest at height 100.
    const u32 size = 64;
    std::vector<u16> data((size + 1) * (size + 1), 100);
    for (u32 z = 0; z <= size; ++z)
        for (u32 x = 0; x <= size; ++x)
            if (x <= 24 || x >= 56) data[z * (size + 1) + x] = 0;
    const Heightmap hm(size, size, 1.0f, data);

    CHECK_THAT(Terrain(hm, 50.0f, true).water_ratio(), WithinAbs(0.5, 1e-6)); // 3 of 6 columns
    CHECK(Terrain(hm, 50.0f, false).water_ratio() == 0.0f);                   // no water on the map
    CHECK(Terrain(hm, 150.0f, true).water_ratio() == 1.0f);                   // all of it under
    CHECK(Terrain(hm, 0.0f, true).water_ratio() == 0.0f); // level with it isn't under
    CHECK(Terrain(Heightmap(8, 8, 1.0f, std::vector<u16>(81, 0)), 50.0f, true).water_ratio() ==
          0.0f); // too small to sample
}

namespace {
/// A size x size map at `height`, with the given grid points changed.
Heightmap grid(u32 size, u16 height, std::initializer_list<std::array<u32, 3>> points = {}) {
    std::vector<u16> data((size + 1) * (size + 1), height);
    for (const auto& [x, z, h] : points) data[z * (size + 1) + x] = static_cast<u16>(h);
    return Heightmap(size, size, 1.0f, std::move(data));
}
} // namespace

TEST_CASE("A line meets the heightfield where Moho's intersection does", "[map]") {
    const Heightmap flat = grid(32, 10);
    CHECK(flat.max_height() == 10.0f);

    // Down through y = 10 halfway along; above it all the way; from under it.
    auto hit = flat.intersect(5, 20, 5, 15, -20, 15, 0.0f, 1.0f);
    REQUIRE(hit);
    CHECK_THAT(*hit, WithinAbs(0.5, 1e-5));
    CHECK_FALSE(flat.intersect(5, 30, 5, 15, -10, 15, 0.0f, 1.0f));
    hit = flat.intersect(5, 5, 5, 1, 0, 0, 0.0f, 10.0f);
    REQUIRE(hit);
    CHECK(*hit == 0.0f); // under the terrain where it starts

    // Past the far end of [start, end], or off the grid: no hit.
    CHECK_FALSE(flat.intersect(5, 20, 5, 15, -20, 15, 0.0f, 0.4f));
    CHECK_FALSE(flat.intersect(-10, 5, 5, 0, 0, 1, 0.0f, 10.0f));

    // Straight down.
    hit = flat.intersect(10.5f, 50, 10.5f, 0, -1, 0, 0.0f, 100.0f);
    REQUIRE(hit);
    CHECK_THAT(*hit, WithinAbs(40.0, 1e-4));

    // Each cell is two triangles, split along its (x, z)-(x+1, z+1) diagonal.
    // Raising the corner (6, 5) raises only the cell's upper triangle, the
    // plane y = 100 (x - z): along z = 5.2 it reaches y = 5 at x = 5.25.
    const Heightmap corner = grid(16, 0, {{6, 5, 100}});
    hit = corner.intersect(0, 5, 5.2f, 1, 0, 0, 0.0f, 16.0f);
    REQUIRE(hit);
    CHECK_THAT(*hit, WithinAbs(5.25, 1e-4));
    // Along the diagonal itself the ground is flat (bilinear would rise to 25).
    CHECK_FALSE(corner.intersect(5.1f, 1, 5.1f, 1, 0, 1, 0.0f, 0.8f));
    // Wholly in the upper triangle: y = 50 at x = 5.7, 0.2 along.
    hit = corner.intersect(5.5f, 50, 5.2f, 1, 0, 0, 0.0f, 10.0f);
    REQUIRE(hit);
    CHECK_THAT(*hit, WithinAbs(0.2, 1e-4));
    // A dip at (6, 5) on ground at 50: the upper triangle's plane, carried into
    // the lower one, would rise above y = 55 there; the lower one is flat.
    const Heightmap dip = grid(16, 50, {{6, 5, 0}});
    CHECK_FALSE(dip.intersect(0, 55, 5.6f, 1, 0, 0, 0.0f, 5.4f));

    // A line at 3:1 across cells, one axis at a time. With (10, 7) raised to
    // 100, cell (9, 6)'s lower triangle is y = 100 (x - 9): the line
    // (1, 5, 4.2) + t (3, 0, 1) meets y = 5 there at x = 9.05, t = 8.05 / 3.
    const Heightmap spike = grid(32, 0, {{10, 7, 100}});
    hit = spike.intersect(1, 5, 4.2f, 3, 0, 1, 0.0f, 4.0f);
    REQUIRE(hit);
    CHECK_THAT(*hit, WithinAbs(8.05 / 3.0, 1e-4));
    // The same line passes (10, 9)'s cells by: it is in row 7 there.
    CHECK_FALSE(grid(32, 0, {{10, 9, 100}}).intersect(1, 5, 4.2f, 3, 0, 1, 0.0f, 4.0f));

    // Non-finite input meets nothing.
    CHECK_FALSE(flat.intersect(5, std::numeric_limits<f32>::quiet_NaN(), 5, 1, 0, 0, 0.0f, 10.0f));
}

TEST_CASE("Terrain blocks a shot as CheckBlockingTerrain says", "[map]") {
    // A ridge 12 high along x = 30 on flat ground.
    std::vector<u16> data(65 * 65, 0);
    for (u32 z = 0; z <= 64; ++z) data[z * 65 + 30] = 12;
    const Heightmap ridge(64, 64, 1.0f, std::move(data));

    // Straight and low shots across it are blocked; a high arc (its peak 20
    // over the line) clears it; on flat ground nothing is.
    CHECK(terrain_blocks_shot(ridge, 10, 0, 32, 50, 0, 32, ShotArc::Straight));
    CHECK(terrain_blocks_shot(ridge, 10, 0, 32, 50, 0, 32, ShotArc::Low));
    CHECK_FALSE(terrain_blocks_shot(ridge, 10, 0, 32, 50, 0, 32, ShotArc::High));
    const Heightmap flat = grid(64, 0);
    CHECK_FALSE(terrain_blocks_shot(flat, 10, 0, 32, 50, 0, 32, ShotArc::Straight));
    CHECK_FALSE(terrain_blocks_shot(flat, 10, 0, 32, 50, 0, 32, ShotArc::Low));

    // The start is lifted 1 and the end 0.5 above what is given.
    const Heightmap raised = grid(64, 10);
    CHECK_FALSE(terrain_blocks_shot(raised, 10, 9.2f, 32, 50, 9.6f, 32, ShotArc::Straight));
    CHECK(terrain_blocks_shot(raised, 10, 8.5f, 32, 50, 9.6f, 32, ShotArc::Straight));
    CHECK(terrain_blocks_shot(raised, 10, 9.2f, 32, 50, 9.0f, 32, ShotArc::Straight));

    // A shot of no length (once lifted) is never blocked, even underground.
    CHECK_FALSE(terrain_blocks_shot(raised, 10, -50, 32, 10, -49.5f, 32, ShotArc::Straight));
}

// ================================================================
// SCMAP parser tests
// ================================================================

namespace {
/// Build a minimal synthetic .scmap file for testing.
/// With `waves`, the water block follows the elevations (whether the water
/// is on or not, as in retail's maps), its generators written at `version`'s
/// layout; the file ends there.
std::vector<u8> build_test_scmap(u32 map_w, u32 map_h, f32 height_scale,
                                 const std::vector<u16>& heights, bool has_water = false,
                                 f32 water_elev = 0.0f,
                                 const std::vector<ScmapWaveGenerator>* waves = nullptr,
                                 i32 version = 56) {
    std::vector<u8> buf;
    auto write_u8 = [&](u8 v) { buf.push_back(v); };
    auto write_i16 = [&](i16 v) {
        buf.push_back(static_cast<u8>(v & 0xFF));
        buf.push_back(static_cast<u8>((v >> 8) & 0xFF));
    };
    auto write_i32 = [&](i32 v) {
        u32 uv;
        std::memcpy(&uv, &v, 4);
        buf.push_back(static_cast<u8>(uv & 0xFF));
        buf.push_back(static_cast<u8>((uv >> 8) & 0xFF));
        buf.push_back(static_cast<u8>((uv >> 16) & 0xFF));
        buf.push_back(static_cast<u8>((uv >> 24) & 0xFF));
    };
    auto write_f32 = [&](f32 v) {
        u32 uv;
        std::memcpy(&uv, &v, 4);
        write_i32(static_cast<i32>(uv));
    };
    auto write_cstring = [&](const char* s) {
        while (*s) buf.push_back(static_cast<u8>(*s++));
        buf.push_back(0);
    };

    // Magic
    buf.push_back('M'); buf.push_back('a'); buf.push_back('p'); buf.push_back(0x1a);

    // Version major
    write_i32(2);

    // Unknown x2
    write_i32(0);
    write_i32(0);

    // Scaled dimensions
    write_f32(static_cast<f32>(map_w));
    write_f32(static_cast<f32>(map_h));

    // Unknown int32 + int16
    write_i32(0);
    write_i16(0);

    // Preview image (empty)
    write_i32(0);

    // Version minor
    write_i32(version);

    // Dimensions
    write_i32(static_cast<i32>(map_w));
    write_i32(static_cast<i32>(map_h));

    // Height scale
    write_f32(height_scale);

    // Heightmap data
    for (auto h : heights) {
        write_i16(h);
    }

    // Flag byte + shader/env strings + cubemap count
    write_u8(0);                        // unknown flag byte
    write_cstring("TTerrainXP");        // terrain shader
    write_cstring("/textures/bg.dds");  // background texture
    write_cstring("/textures/sky.dds"); // sky cubemap
    write_i32(1);                       // env cubemaps
    write_cstring("<default>");
    write_cstring("/textures/envcube.dds");

    // 23 lighting floats, each distinct: 1.0, 1.1, ... 3.2
    for (int i = 0; i < 23; i++) {
        write_f32(1.0f + 0.1f * static_cast<f32>(i));
    }

    // Water: the flag, then the elevations either way (-10000 on a dry map)
    write_u8(has_water ? 1 : 0);
    write_f32(has_water ? water_elev : -10000.0f);
    write_f32(has_water ? water_elev - 5.0f : -10000.0f);  // deep
    write_f32(has_water ? water_elev - 10.0f : -10000.0f); // abyss
    if (!waves) return buf;

    // The water's properties: 20 floats, two textures, four repeats, four
    // normal layers
    for (int i = 0; i < 20; i++) write_f32(0.5f);
    write_cstring("/textures/cube.dds");
    write_cstring("/textures/ramp.dds");
    for (int i = 0; i < 4; i++) write_f32(0.01f);
    for (int i = 0; i < 4; i++) {
        write_f32(0.1f);
        write_f32(0.2f);
        write_cstring("/textures/waves.dds");
    }
    write_i32(static_cast<i32>(waves->size()));
    for (const ScmapWaveGenerator& g : *waves) {
        write_cstring(g.texture.c_str());
        write_cstring(g.ramp.c_str());
        for (f32 v : g.position) write_f32(v);
        write_f32(g.angle);
        for (f32 v : g.direction) write_f32(v);
        for (f32 v : g.lifetime) write_f32(v);
        for (f32 v : g.interval) write_f32(v);
        write_f32(g.begin_size);
        write_f32(g.end_size);
        if (version > 51) {
            write_f32(g.frame_count);
            for (f32 v : g.frame_rate) write_f32(v);
            write_f32(g.strip_count);
        }
    }
    return buf;
}
} // namespace

TEST_CASE("SCMAP parser extracts heightmap", "[map]") {
    // 4x4 map → 5x5 grid = 25 samples
    u32 w = 4, h = 4;
    std::vector<u16> heights;
    for (u32 z = 0; z <= h; z++) {
        for (u32 x = 0; x <= w; x++) {
            heights.push_back(static_cast<i16>((z * (w + 1) + x) * 100));
        }
    }

    auto scmap = build_test_scmap(w, h, 1.0f / 128.0f, heights);
    auto result = parse_scmap(scmap);

    REQUIRE(result.ok());
    auto& data = result.value();
    CHECK(data.map_width == 4);
    CHECK(data.map_height == 4);
    CHECK(data.heightmap.size() == 25);
    CHECK(data.height_scale == 1.0f / 128.0f);
    CHECK(data.version_minor == 56);

    // First sample should be 0
    CHECK(data.heightmap[0] == 0);
    // Last sample: (4*5+4)*100 = 2400
    CHECK(data.heightmap[24] == 2400);
}

TEST_CASE("SCMAP parser extracts water data", "[map]") {
    u32 w = 2, h = 2;
    std::vector<u16> heights(9, 100);

    auto scmap = build_test_scmap(w, h, 1.0f, heights, true, 25.0f);
    auto result = parse_scmap(scmap);

    REQUIRE(result.ok());
    auto& data = result.value();
    CHECK(data.has_water == true);
    CHECK_THAT(data.water_elevation, WithinAbs(25.0, 0.01));
}

TEST_CASE("SCMAP parser reads the map's lighting and environment (M210a)", "[map]") {
    std::vector<u16> heights(9, 100);
    auto result = parse_scmap(build_test_scmap(2, 2, 1.0f, heights, true, 25.0f));
    REQUIRE(result.ok());
    const auto& d = result.value();
    CHECK(d.environment.terrain_shader == "TTerrainXP");
    CHECK(d.environment.background == "/textures/bg.dds");
    CHECK(d.environment.sky_cubemap == "/textures/sky.dds");
    REQUIRE(d.environment.cubemaps.size() == 1);
    CHECK(d.environment.cubemaps[0].first == "<default>");
    CHECK(d.environment.cubemaps[0].second == "/textures/envcube.dds");
    // The 23 floats in order: multiplier, sun direction, ambience, sun
    // colour, shadow fill, specular (4), bloom, fog colour, start, end.
    const auto& l = d.lighting;
    const auto at = [](int i) { return 1.0 + 0.1 * i; };
    CHECK_THAT(l.multiplier, WithinAbs(at(0), 1e-5));
    CHECK_THAT(l.sun_direction[0], WithinAbs(at(1), 1e-5));
    CHECK_THAT(l.sun_direction[2], WithinAbs(at(3), 1e-5));
    CHECK_THAT(l.sun_ambience[0], WithinAbs(at(4), 1e-5));
    CHECK_THAT(l.sun_color[0], WithinAbs(at(7), 1e-5));
    CHECK_THAT(l.shadow_fill[2], WithinAbs(at(12), 1e-5));
    CHECK_THAT(l.specular[0], WithinAbs(at(13), 1e-5));
    CHECK_THAT(l.specular[3], WithinAbs(at(16), 1e-5));
    CHECK_THAT(l.bloom, WithinAbs(at(17), 1e-5));
    CHECK_THAT(l.fog_color[0], WithinAbs(at(18), 1e-5));
    CHECK_THAT(l.fog_start, WithinAbs(at(21), 1e-5));
    CHECK_THAT(l.fog_end, WithinAbs(at(22), 1e-5));
    // What follows still parses: the water after the block.
    CHECK(d.has_water);
    CHECK_THAT(d.water_elevation, WithinAbs(25.0, 0.01));
}

namespace {
ScmapWaveGenerator sample_wave(f32 x) {
    ScmapWaveGenerator g;
    g.texture = "/env/common/decals/shoreline/turbulance02_albedo.dds";
    g.ramp = "/env/common/decals/shoreline/waveramptest.dds";
    g.position[0] = x;
    g.position[1] = 17.5f;
    g.position[2] = 40.0f;
    g.angle = 1.25f;
    g.direction[0] = 0.1f;
    g.direction[2] = -0.2f;
    g.lifetime[0] = 30.0f;
    g.lifetime[1] = 45.0f;
    g.interval[0] = 2.0f;
    g.interval[1] = 5.0f;
    g.begin_size = 3.0f;
    g.end_size = 6.0f;
    g.frame_count = 4.0f;
    g.frame_rate[0] = 0.5f;
    g.frame_rate[1] = 0.75f;
    g.strip_count = 3.0f;
    return g;
}
} // namespace

TEST_CASE("SCMAP parser reads the water's wave generators (M213c)", "[map]") {
    const std::vector<ScmapWaveGenerator> waves = {sample_wave(10.0f), sample_wave(20.0f)};
    std::vector<u16> heights(9, 100);
    auto result = parse_scmap(build_test_scmap(2, 2, 1.0f, heights, true, 25.0f, &waves));
    REQUIRE(result.ok());
    const auto& d = result.value();
    REQUIRE(d.waves.size() == 2);
    const ScmapWaveGenerator& g = d.waves[1];
    CHECK(g.texture == waves[1].texture);
    CHECK(g.ramp == waves[1].ramp);
    CHECK(g.position[0] == 20.0f);
    CHECK(g.position[1] == 17.5f);
    CHECK(g.position[2] == 40.0f);
    CHECK(g.angle == 1.25f);
    CHECK(g.direction[0] == 0.1f);
    CHECK(g.direction[2] == -0.2f);
    CHECK(g.lifetime[0] == 30.0f);
    CHECK(g.lifetime[1] == 45.0f);
    CHECK(g.interval[0] == 2.0f);
    CHECK(g.interval[1] == 5.0f);
    CHECK(g.begin_size == 3.0f);
    CHECK(g.end_size == 6.0f);
    CHECK(g.frame_count == 4.0f);
    CHECK(g.frame_rate[0] == 0.5f);
    CHECK(g.frame_rate[1] == 0.75f);
    CHECK(g.strip_count == 3.0f);
    // The file ends after them: read short, not whole
    CHECK_FALSE(d.read_whole);
}

TEST_CASE("A map before v52 has no wave frames: Moho's defaults", "[map]") {
    const std::vector<ScmapWaveGenerator> waves = {sample_wave(10.0f), sample_wave(20.0f)};
    std::vector<u16> heights(9, 100);
    auto result = parse_scmap(build_test_scmap(2, 2, 1.0f, heights, true, 25.0f, &waves, 51));
    REQUIRE(result.ok());
    const auto& d = result.value();
    REQUIRE(d.waves.size() == 2);
    // Each read in step: the first's strings and defaults, the second's
    // strings and fields
    CHECK(d.waves[0].frame_count == 1.0f);
    CHECK(d.waves[0].strip_count == 1.0f);
    CHECK(d.waves[1].texture == waves[1].texture);
    CHECK(d.waves[1].ramp == waves[1].ramp);
    CHECK(d.waves[1].position[0] == 20.0f);
    CHECK(d.waves[1].end_size == 6.0f);
    CHECK(d.waves[1].frame_count == 1.0f);
    CHECK(d.waves[1].frame_rate[0] == 1.0f);
    CHECK(d.waves[1].frame_rate[1] == 0.0f);
    CHECK(d.waves[1].strip_count == 1.0f);
}

TEST_CASE("A dry map's water block is read all the same", "[map]") {
    // Retail's dry maps (water flag off) still hold the elevations, the
    // water's properties and its waves: the parser had skipped them and read
    // everything after from the wrong place
    const std::vector<ScmapWaveGenerator> waves = {sample_wave(10.0f)};
    std::vector<u16> heights(9, 100);
    auto result = parse_scmap(build_test_scmap(2, 2, 1.0f, heights, false, 0.0f, &waves));
    REQUIRE(result.ok());
    const auto& d = result.value();
    CHECK_FALSE(d.has_water);
    CHECK(d.water_elevation == 0.0f); // the -10000 isn't kept
    REQUIRE(d.waves.size() == 1);
    CHECK(d.waves[0].texture == waves[0].texture);
    CHECK(d.waves[0].strip_count == 3.0f);
}

TEST_CASE("A wave count the file can't hold is refused", "[map]") {
    std::vector<ScmapWaveGenerator> waves = {sample_wave(10.0f)};
    std::vector<u16> heights(9, 100);
    std::vector<u8> bytes = build_test_scmap(2, 2, 1.0f, heights, true, 25.0f, &waves);
    // The count sits before the one generator's two strings and 17 floats
    const size_t record = waves[0].texture.size() + 1 + waves[0].ramp.size() + 1 + 17 * 4;
    const size_t at = bytes.size() - record - 4;
    const u32 huge = 20000000;
    std::memcpy(bytes.data() + at, &huge, 4);
    auto result = parse_scmap(bytes);
    REQUIRE(result.ok()); // the header still loads
    CHECK(result.value().waves.empty());
}

TEST_CASE("SCMAP parser rejects invalid magic", "[map]") {
    std::vector<u8> bad_data = {'N', 'O', 'T', 'M'};
    bad_data.resize(100, 0);
    auto result = parse_scmap(bad_data);
    REQUIRE_FALSE(result.ok());
}

TEST_CASE("SCMAP parser rejects truncated file", "[map]") {
    std::vector<u8> tiny = {'M', 'a', 'p', 0x1a};
    auto result = parse_scmap(tiny);
    REQUIRE_FALSE(result.ok());
}

// ================================================================
// Terrain tests
// ================================================================

TEST_CASE("Terrain height queries with water", "[map]") {
    // 2x2 map, all heights at 10.0, water at 20.0
    std::vector<u16> data(9, 1280); // 1280 * (1/128) = 10.0
    Heightmap hm(2, 2, 1.0f / 128.0f, data);
    Terrain terrain(std::move(hm), 20.0f, true);

    CHECK(terrain.map_width() == 2);
    CHECK(terrain.map_height() == 2);
    CHECK(terrain.has_water() == true);
    CHECK_THAT(terrain.water_elevation(), WithinAbs(20.0, 0.01));

    // Terrain is at 10.0, surface should be max(10, 20) = 20
    CHECK_THAT(terrain.get_terrain_height(1, 1), WithinAbs(10.0, 0.01));
    CHECK_THAT(terrain.get_surface_height(1, 1), WithinAbs(20.0, 0.01));
}

TEST_CASE("Terrain height queries above water", "[map]") {
    // Heights at 30.0, water at 20.0 → surface = terrain (above water)
    std::vector<u16> data(9, 3840); // 3840 * (1/128) = 30.0
    Heightmap hm(2, 2, 1.0f / 128.0f, data);
    Terrain terrain(std::move(hm), 20.0f, true);

    CHECK_THAT(terrain.get_terrain_height(1, 1), WithinAbs(30.0, 0.01));
    CHECK_THAT(terrain.get_surface_height(1, 1), WithinAbs(30.0, 0.01));
}

TEST_CASE("Terrain without water", "[map]") {
    std::vector<u16> data(9, 2560); // 2560 * (1/128) = 20.0
    Heightmap hm(2, 2, 1.0f / 128.0f, data);
    Terrain terrain(std::move(hm), 0.0f);

    CHECK(terrain.has_water() == false);
    CHECK_THAT(terrain.get_terrain_height(1, 1), WithinAbs(20.0, 0.01));
    CHECK_THAT(terrain.get_surface_height(1, 1), WithinAbs(20.0, 0.01));
}
