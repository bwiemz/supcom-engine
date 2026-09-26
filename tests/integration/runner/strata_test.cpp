// --strata-test (M212a): the terrain's strata blend as FA's terrain.fx
// blends them.
//
// An offscreen renderer draws a small flat terrain of the test's own: known
// stratum textures from the game, and blend textures it writes. Lit by a
// white shadow fill and no sun, a frame is the blended albedo alone. Frames
// are compared per pixel with each other.

#include "integration_tests.hpp"
#include "render_probe.hpp"

#include "map/heightmap.hpp"
#include "map/scmap_parser.hpp"
#include "map/terrain.hpp"

#include <spdlog/spdlog.h>

#include <array>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace osc::test {

namespace {

constexpr u32 kSize = 64; // the terrain, in world units (and blend texels)
constexpr f32 kCentre = kSize / 2.0f;
constexpr f32 kDistance = 30.0f; // the camera's nearest

// Stratum textures unlike each other.
const char* const kGravel = "/env/evergreen2/layers/eg_gravel005_albedo.dds"; // grey, opaque (DXT1)
const char* const kGrass = "/env/evergreen2/layers/evgrass005_albedo.dds";    // green
const char* const kRockNormal = "/env/evergreen2/layers/evrock007_normal.dds";

/// A blend texture as the maps store them (uncompressed BGRA DDS), every
/// texel `rgba`: one mask per channel.
std::vector<char> blend_dds(std::array<u8, 4> rgba) {
    std::vector<char> d(128 + static_cast<size_t>(kSize) * kSize * 4, 0);
    const auto put = [&](size_t offset, u32 v) { std::memcpy(d.data() + offset, &v, 4); };
    std::memcpy(d.data(), "DDS ", 4);
    put(4, 124);                            // header size
    put(8, 0x1 | 0x2 | 0x4 | 0x8 | 0x1000); // caps, height, width, pitch, pixel format
    put(12, kSize);
    put(16, kSize);
    put(20, kSize * 4);
    put(28, 1);          // one mip
    put(76, 32);         // pixel format size
    put(80, 0x40 | 0x1); // RGB, alpha
    put(88, 32);
    put(92, 0x00FF0000); // BGRA byte order
    put(96, 0x0000FF00);
    put(100, 0x000000FF);
    put(104, 0xFF000000);
    put(108, 0x1000); // a texture
    for (size_t i = 128; i < d.size(); i += 4) {
        d[i] = static_cast<char>(rgba[2]);
        d[i + 1] = static_cast<char>(rgba[1]);
        d[i + 2] = static_cast<char>(rgba[0]);
        d[i + 3] = static_cast<char>(rgba[3]);
    }
    return d;
}

/// Ten strata, none textured, at size 4.
std::vector<map::StratumInfo> no_strata() {
    std::vector<map::StratumInfo> s(10);
    for (auto& st : s) {
        st.albedo_scale = 4.0f;
        st.normal_scale = 4.0f;
    }
    return s;
}

/// The albedo alone: no sun, a white shadow fill.
map::ScmapLighting albedo_only() {
    map::ScmapLighting l;
    for (int i = 0; i < 3; ++i) {
        l.sun_color[i] = 0.0f;
        l.sun_ambience[i] = 0.0f;
        l.shadow_fill[i] = 1.0f;
    }
    for (f32& v : l.specular) v = 0.0f;
    return l;
}

/// A white sun 0.8 high, no fill: the frame shows the normals.
map::ScmapLighting sunlit() {
    map::ScmapLighting l = albedo_only();
    l.multiplier = 1.0f;
    l.sun_direction[0] = 0.0f;
    l.sun_direction[1] = 0.8f;
    l.sun_direction[2] = 0.6f;
    for (int i = 0; i < 3; ++i) {
        l.sun_color[i] = 1.0f;
        l.shadow_fill[i] = 0.0f;
    }
    return l;
}

} // namespace

void test_strata(TestContext& ctx) {
    spdlog::info("=== STRATA TEST: the terrain's strata, as FA blends them (M212a) ===");
    Tally t;
    OffscreenShots shots(ctx);
    if (!shots.ok()) {
        t.check(false, "renderer init (no Vulkan?)");
        return;
    }

    map::Terrain terrain(
        map::Heightmap(kSize, kSize, 1.0f / 128.0f,
                       std::vector<u16>(static_cast<size_t>(kSize + 1) * (kSize + 1), 0)),
        0.0f, false);

    // Draw `strata` over the blend textures under `lighting`, as a
    // `shader` map.
    const auto shoot = [&](std::vector<map::StratumInfo> strata, std::array<u8, 4> mask0,
                           std::array<u8, 4> mask1, const char* shader = "TTerrain",
                           const map::ScmapLighting& lighting = albedo_only()) {
        map::ScmapEnvironment env;
        env.terrain_shader = shader;
        terrain.set_lighting(lighting, std::move(env));
        terrain.set_strata(std::move(strata), blend_dds(mask0), blend_dds(mask1));
        return shots.shoot(terrain, kCentre, kCentre, kDistance, /*with_world=*/false);
    };
    // Gravel below, grass as stratum 1 (the first blend texture's red) at a
    // size of its own.
    constexpr f32 kGrassSize = 6.0f;
    const auto gravel_and_grass = [] {
        auto s = no_strata();
        s[0].albedo_path = kGravel;
        s[1].albedo_path = kGrass;
        s[1].albedo_scale = kGrassSize;
        return s;
    };
    const auto only = [](const char* albedo, f32 size = 4.0f) {
        auto s = no_strata();
        s[0].albedo_path = albedo;
        s[0].albedo_scale = size;
        return s;
    };

    const Pixels none = shoot(gravel_and_grass(), {0, 0, 0, 0}, {0, 0, 0, 0});
    const Pixels half = shoot(gravel_and_grass(), {128, 0, 0, 0}, {0, 0, 0, 0});
    const Pixels three_quarters = shoot(gravel_and_grass(), {191, 0, 0, 0}, {0, 0, 0, 0});
    const Pixels full = shoot(gravel_and_grass(), {255, 0, 0, 0}, {0, 0, 0, 0});

    // Test 1: masks are sharpened, saturate(mask * 2 - 1): at one half a
    // stratum adds nothing, at one it replaces the ground below, at its own
    // size.
    const f32 d_half = mean_abs_diff(half, none);
    const f32 d_full = mean_abs_diff(full, none);
    const f32 d_grass =
        mean_abs_diff(full, shoot(only(kGrass, kGrassSize), {0, 0, 0, 0}, {0, 0, 0, 0}));
    t.check(d_half < 0.01f && d_full > 0.03f && d_grass < 0.01f,
            fmt::format("Test 1: a mask of one half changes the ground by {:.4f}, a full one by "
                        "{:.4f} (to the stratum alone, off by {:.4f})",
                        d_half, d_full, d_grass));

    // Test 2: at three quarters (191), the sharpened mask is 0.498: the frame
    // is that mix of the two.
    {
        const f32 m = 191.0f / 255.0f * 2.0f - 1.0f;
        Pixels mix = none;
        for (size_t i = 0; i < mix.size() && i < full.size(); ++i) {
            for (int c = 0; c < 3; ++c) mix[i][c] = none[i][c] * (1.0f - m) + full[i][c] * m;
        }
        const f32 d = mean_abs_diff(three_quarters, mix);
        t.check(d < 0.015f,
                fmt::format("Test 2: a mask of three quarters is {:.3f} of the stratum (off by "
                            "{:.4f})",
                            m, d));
    }

    // Test 3: a TTerrain map (the original game's) blends only the first
    // texture's four strata; a TTerrainXP map uses the second's too.
    {
        auto with_grass5 = [&] {
            auto s = no_strata();
            s[0].albedo_path = kGravel;
            s[5].albedo_path = kGrass;
            return s;
        };
        const Pixels tt = shoot(with_grass5(), {0, 0, 0, 0}, {255, 0, 0, 0}, "TTerrain");
        const Pixels xp = shoot(with_grass5(), {0, 0, 0, 0}, {255, 0, 0, 0}, "TTerrainXP");
        const Pixels grass = shoot(only(kGrass), {0, 0, 0, 0}, {0, 0, 0, 0});
        const f32 d_tt = mean_abs_diff(tt, none);
        const f32 d_xp = mean_abs_diff(xp, grass);
        t.check(d_tt < 0.01f && d_xp < 0.01f && mean_abs_diff(xp, none) > 0.03f,
                fmt::format("Test 3: stratum 5 changes a TTerrain map's ground by {:.4f}; a "
                            "TTerrainXP map's is it (off by {:.4f})",
                            d_tt, d_xp));
    }

    // Test 4: the upper stratum lies over the rest by its alpha, at its own
    // size: an opaque one (DXT1) is all that shows.
    {
        auto s = only(kGrass);
        s[9].albedo_path = kGravel;
        s[9].albedo_scale = 8.0f;
        const Pixels upper = shoot(s, {0, 0, 0, 0}, {0, 0, 0, 0});
        const Pixels gravel = shoot(only(kGravel, 8.0f), {0, 0, 0, 0}, {0, 0, 0, 0});
        const Pixels grass = shoot(only(kGrass), {0, 0, 0, 0}, {0, 0, 0, 0});
        const f32 d = mean_abs_diff(upper, gravel);
        t.check(
            d < 0.01f && mean_abs_diff(gravel, grass) > 0.03f,
            fmt::format("Test 4: an opaque upper stratum covers the ground (off by {:.4f})", d));
    }

    // Test 5: a normal map repeats at its own size, not its albedo's.
    {
        auto at = [&](f32 normal_size) {
            auto s = only(kGravel);
            s[0].normal_path = kRockNormal;
            s[0].normal_scale = normal_size;
            return shoot(s, {0, 0, 0, 0}, {0, 0, 0, 0}, "TTerrain", sunlit());
        };
        const Pixels small = at(4.0f);
        const Pixels large = at(16.0f);
        const Pixels again = at(4.0f);
        const f32 d = mean_abs_diff(small, large);
        t.check(d > 0.01f && mean_abs_diff(small, again) < 0.002f,
                fmt::format("Test 5: the normal map at size 16 rather than 4 changes the lit "
                            "ground by {:.4f}",
                            d));
    }

    // Test 6: normals blend by the raw mask (TerrainNormalsPS). At one half
    // a stratum without a normal map flattens half the relief below it,
    // though its albedo adds nothing.
    {
        auto rock_under = [&] {
            auto s = gravel_and_grass();
            s[0].normal_path = kRockNormal;
            return s;
        };
        const Pixels flat_half =
            shoot(rock_under(), {128, 0, 0, 0}, {0, 0, 0, 0}, "TTerrain", sunlit());
        const Pixels rock = shoot(rock_under(), {0, 0, 0, 0}, {0, 0, 0, 0}, "TTerrain", sunlit());
        const f32 d = mean_abs_diff(flat_half, rock);
        t.check(d > 0.005f,
                fmt::format("Test 6: a mask of one half changes the lit relief by {:.4f}", d));
    }

    spdlog::info("Strata test: {}/{} passed", t.pass, t.pass + t.fail);
}

} // namespace osc::test
