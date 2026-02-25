#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "map/heightmap.hpp"
#include "map/scmap_parser.hpp"
#include "map/terrain.hpp"

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
    std::vector<i16> data = {100, 200, 300, 400, 500, 600, 700, 800, 900};
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
    std::vector<i16> data = {0, 100, 0, 0, 100, 0, 0, 0, 0};
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
    std::vector<i16> data = {10, 20, 30, 40};
    Heightmap hm(1, 1, 1.0f, data); // 1x1 map → 2x2 grid

    // Negative coords should clamp to (0,0) = 10
    CHECK_THAT(hm.get_height(-5.0f, -5.0f), WithinAbs(10.0, 0.01));

    // Beyond max should clamp to (1,1) = 40
    CHECK_THAT(hm.get_height(10.0f, 10.0f), WithinAbs(40.0, 0.01));
}

TEST_CASE("Heightmap with real-world scale", "[map]") {
    // Typical SupCom scale: 1/128
    f32 scale = 1.0f / 128.0f;
    std::vector<i16> data = {0, 0, 0, 3200, 0, 0, 0, 0, 0};
    Heightmap hm(2, 2, scale, data);

    // Grid point (0,1) = 3200 / 128 = 25.0
    CHECK_THAT(hm.get_height(0, 1), WithinAbs(25.0, 0.01));
}

// ================================================================
// SCMAP parser tests
// ================================================================

namespace {
/// Build a minimal synthetic .scmap file for testing.
std::vector<u8> build_test_scmap(u32 map_w, u32 map_h, f32 height_scale,
                                  const std::vector<i16>& heights,
                                  bool has_water = false,
                                  f32 water_elev = 0.0f) {
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
    write_i32(56);

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
    write_u8(0);            // unknown flag byte
    write_cstring("");      // terrain shader
    write_cstring("");      // background texture
    write_cstring("");      // sky cubemap
    write_i32(0);           // env cubemap count (none)

    // 23 lighting floats
    for (int i = 0; i < 23; i++) {
        write_f32(0.0f);
    }

    // Water
    write_u8(has_water ? 1 : 0);
    if (has_water) {
        write_f32(water_elev);
        write_f32(water_elev - 5.0f); // deep
        write_f32(water_elev - 10.0f); // abyss
    }

    return buf;
}
} // namespace

TEST_CASE("SCMAP parser extracts heightmap", "[map]") {
    // 4x4 map → 5x5 grid = 25 samples
    u32 w = 4, h = 4;
    std::vector<i16> heights;
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
    std::vector<i16> heights(9, 100);

    auto scmap = build_test_scmap(w, h, 1.0f, heights, true, 25.0f);
    auto result = parse_scmap(scmap);

    REQUIRE(result.ok());
    auto& data = result.value();
    CHECK(data.has_water == true);
    CHECK_THAT(data.water_elevation, WithinAbs(25.0, 0.01));
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
    std::vector<i16> data(9, 1280); // 1280 * (1/128) = 10.0
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
    std::vector<i16> data(9, 3840); // 3840 * (1/128) = 30.0
    Heightmap hm(2, 2, 1.0f / 128.0f, data);
    Terrain terrain(std::move(hm), 20.0f, true);

    CHECK_THAT(terrain.get_terrain_height(1, 1), WithinAbs(30.0, 0.01));
    CHECK_THAT(terrain.get_surface_height(1, 1), WithinAbs(30.0, 0.01));
}

TEST_CASE("Terrain without water", "[map]") {
    std::vector<i16> data(9, 2560); // 2560 * (1/128) = 20.0
    Heightmap hm(2, 2, 1.0f / 128.0f, data);
    Terrain terrain(std::move(hm), 0.0f);

    CHECK(terrain.has_water() == false);
    CHECK_THAT(terrain.get_terrain_height(1, 1), WithinAbs(20.0, 0.01));
    CHECK_THAT(terrain.get_surface_height(1, 1), WithinAbs(20.0, 0.01));
}
