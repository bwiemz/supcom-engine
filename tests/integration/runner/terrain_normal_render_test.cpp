// --terrain-normal-render-test (M212e): Moho's terrain normals.
//
// The normal pass draws the terrain's normals into a target first: its
// strata's tangent normal in RG, the map's normal maps (sampled bicubic) in
// BA, then the normal decals blended into RG. The terrain, decals and splats
// light by frame.fx's BasisPS composite of their own pixel of it. Ground and
// textures of the test's own: a grey decal's light (its frame under the
// map's light over its frame under a white fill, light 1: the albedo alone)
// is FA's formula for the normal wanted.

#include "integration_tests.hpp"
#include "intel_probe.hpp"
#include "plate_fixtures.hpp"
#include "render_probe.hpp"

#include "core/image.hpp"
#include "core/test_status.hpp"
#include "lua/lua_state.hpp"
#include "map/heightmap.hpp"
#include "map/terrain.hpp"
#include "renderer/dds_decode.hpp"
#include "renderer/renderer.hpp"
#include "renderer/terrain_normal_maps.hpp"
#include "sim/sim_state.hpp"
#include "vfs/directory_mount.hpp"
#include "vfs/virtual_file_system.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace osc::test {

namespace {

constexpr const char* kRoot = "/osc_tnormal_test";
constexpr u32 kSize = 64;
constexpr f32 kDistance = 40.0f;
constexpr f32 kQuarter = 1.5707964f;

using Vec3 = std::array<f32, 3>;

Rgb pixel_at(renderer::Renderer& r, const ImageRGBA8& img, const sim::Vector3& p) {
    const auto s = screen_of(r, p);
    if (!s || img.width == 0) return {0, 0, 0};
    const u32 x = static_cast<u32>(std::clamp((*s)[0], 0.0f, static_cast<f32>(img.width - 1)));
    const u32 y = static_cast<u32>(std::clamp((*s)[1], 0.0f, static_cast<f32>(img.height - 1)));
    const u8* q = &img.pixels[(static_cast<size_t>(y) * img.width + x) * 4];
    return {q[0] / 255.0f, q[1] / 255.0f, q[2] / 255.0f};
}

Vec3 normalized(Vec3 v) {
    const f32 l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    return {v[0] / l, v[1] / l, v[2] / l};
}

/// FA's light for normal `n`, without specular or shadow:
/// L = sun * saturate(N.S) + ambience; L = multiplier * L + fill * (1 - L).
Rgb light_for(const map::ScmapLighting& l, const Vec3& n) {
    const Vec3 s = normalized({l.sun_direction[0], l.sun_direction[1], l.sun_direction[2]});
    const f32 d = std::max(0.0f, s[0] * n[0] + s[1] * n[1] + s[2] * n[2]);
    Rgb out{};
    for (size_t c = 0; c < 3; ++c) {
        const f32 light = l.sun_color[c] * d + l.sun_ambience[c];
        out[c] = l.multiplier * light + l.shadow_fill[c] * (1.0f - light);
    }
    return out;
}

/// The light a frame shows at `p`: its colour there over the fill frame's.
Rgb light_seen(renderer::Renderer& r, const ImageRGBA8& lit, const ImageRGBA8& fill,
               const sim::Vector3& p) {
    const Rgb a = pixel_at(r, lit, p);
    const Rgb b = pixel_at(r, fill, p);
    return {b[0] > 0 ? a[0] / b[0] : 0, b[1] > 0 ? a[1] / b[1] : 0, b[2] > 0 ? a[2] / b[2] : 0};
}

f32 apart(const Rgb& a, const Rgb& b) {
    return std::max({std::abs(a[0] - b[0]), std::abs(a[1] - b[1]), std::abs(a[2] - b[2])});
}

std::string rgb_text(const Rgb& c) {
    return fmt::format("({:.3f} {:.3f} {:.3f})", c[0], c[1], c[2]);
}

u8 unorm(f32 v) {
    return static_cast<u8>(std::lround((v * 0.5f + 0.5f) * 255.0f));
}

} // namespace

void test_terrain_normal_render(TestContext& ctx) {
    spdlog::info("=== TERRAIN NORMAL TEST: Moho's normal pass (M212e) ===");
    Tally t;
    const map::ScmapLighting original = ctx.sim.terrain()->lighting();
    const map::ScmapEnvironment env = ctx.sim.terrain()->environment();

    // Test 6 first, on the sim's own map: SCMP_009's normal map is one
    // 1024 tile, a texel a unit, x in alpha and z in green: it agrees with
    // the normal its heights give.
    {
        const map::Terrain& scmp = *ctx.sim.terrain();
        const auto& maps = scmp.normal_maps();
        bool parsed = maps.tiles.size() == 1 && maps.tile_width == 1024 && maps.tile_height == 1024;
        f32 err_x = 0;
        f32 err_z = 0;
        f32 dot = 0;
        u32 n = 0;
        if (parsed) {
            const auto& dds = maps.tiles[0];
            const std::vector<u8> rgba = renderer::decode_bc3_to_rgba(
                reinterpret_cast<const u8*>(dds.data() + 128), 1024, 1024);
            for (u32 j = 64; j < 960; j += 37)
                for (u32 i = 64; i < 960; i += 41) {
                    const u8* texel = &rgba[(static_cast<size_t>(j) * 1024 + i) * 4];
                    const f32 x = texel[3] / 127.5f - 1.0f;
                    const f32 z = texel[1] / 127.5f - 1.0f;
                    const f32 wx = static_cast<f32>(i) + 0.5f;
                    const f32 wz = static_cast<f32>(j) + 0.5f;
                    const f32 dx = scmp.get_terrain_height(wx - 1.0f, wz) -
                                   scmp.get_terrain_height(wx + 1.0f, wz);
                    const f32 dz = scmp.get_terrain_height(wx, wz - 1.0f) -
                                   scmp.get_terrain_height(wx, wz + 1.0f);
                    const Vec3 h = normalized({dx, 2.0f, dz});
                    err_x += std::abs(x - h[0]);
                    err_z += std::abs(z - h[2]);
                    dot += x * h[0] + z * h[2];
                    ++n;
                }
            err_x /= static_cast<f32>(std::max(n, 1u));
            err_z /= static_cast<f32>(std::max(n, 1u));
        }
        t.check(parsed && err_x < 0.05f && err_z < 0.05f && dot > 0.0f,
                fmt::format("Test 6: SCMP_009's normal map: {} tile(s) of {}x{}; against its "
                            "heights' normals, x off {:.3f}, z off {:.3f} on average, agreeing "
                            "({:.2f})",
                            maps.tiles.size(), maps.tile_width, maps.tile_height, err_x, err_z,
                            dot));
    }

    const auto dir = std::filesystem::temp_directory_path() / "osc_tnormal_test";
    std::filesystem::create_directories(dir);
    const auto flat = [](u8 r_, u8 g_, u8 b_, u8 a_) {
        return [=](int, u32, u32) { return std::array<u8, 4>{r_, g_, b_, a_}; };
    };
    // A tilt of 0.6 along x: n = (0.6, 0.8, 0), y up.
    const f32 kTilt = 0.6f;
    write_dds(dir / "grey.dds", 1, flat(128, 128, 128, 255));
    // A stratum's normal map: tangent space, RGB, z up: (0.6, 0, 0.8).
    write_dds(dir / "stratum_tilt.dds", 1, flat(unorm(kTilt), unorm(0.0f), unorm(0.8f), 255));
    // A decal's: x in alpha, z in green, its blend in red.
    write_dds(dir / "decal_tilt.dds", 1, flat(255, unorm(0.0f), 0, unorm(kTilt)));
    write_dds(dir / "decal_tilt_half.dds", 1, flat(128, unorm(0.0f), 0, unorm(kTilt)));
    ctx.vfs.mount(kRoot, std::make_unique<vfs::DirectoryMount>(dir));
    const auto path = [](const char* name) { return fmt::format("{}/{}", kRoot, name); };

    const auto make_ground = [&](const std::function<f32(u32, u32)>& height,
                                 const std::string& stratum_normal = {}) {
        constexpr f32 kScale = 1.0f / 128.0f;
        std::vector<u16> heights(static_cast<size_t>(kSize + 1) * (kSize + 1));
        for (u32 z = 0; z <= kSize; ++z)
            for (u32 x = 0; x <= kSize; ++x)
                heights[static_cast<size_t>(z) * (kSize + 1) + x] =
                    static_cast<u16>(height(x, z) / kScale);
        auto ground = std::make_unique<map::Terrain>(
            map::Heightmap(kSize, kSize, kScale, std::move(heights)), 0.0f, false);
        std::vector<map::StratumInfo> strata(10);
        for (auto& st : strata) st.albedo_scale = st.normal_scale = 4.0f;
        strata[0].albedo_path = "/env/evergreen2/layers/eg_gravel005_albedo.dds";
        strata[0].normal_path = stratum_normal;
        ground->set_strata(std::move(strata), {}, {});
        return ground;
    };
    const auto decal = [&](map::DecalType type, const char* texture, f32 x, f32 z, f32 turn) {
        map::DecalInfo d;
        d.type = type;
        d.texture_path = path(texture);
        d.position_x = x;
        d.position_z = z;
        d.scale_x = d.scale_z = 8.0f;
        d.rotation_y = turn;
        d.cut_off_lod = 1000.0f;
        return d;
    };

    OffscreenShots shots(ctx);
    if (!shots.ok()) {
        t.check(false, "renderer init (no Vulkan?)");
        return;
    }
    renderer::Renderer& r = shots.renderer();
    r.set_decals_enabled(true);
    const map::ScmapLighting fill = white_fill();
    // A pair of frames, under the map's light and the fill, at (32, 32).
    const auto frames = [&](map::Terrain& g, const std::vector<map::DecalInfo>& decals) {
        g.set_decals(decals);
        std::array<ImageRGBA8, 2> out;
        for (size_t k = 0; k < 2; ++k) {
            g.set_lighting(k == 0 ? original : fill, env);
            (void)shots.shoot_frame(g, 32, 32, kDistance); // textures load on the first
            out[k] = shots.shoot_frame(g, 32, 32, kDistance);
        }
        return out;
    };
    // A grey decal at (28, 28), 8 by 8, to measure the light on.
    const map::DecalInfo grey = decal(map::DecalType::Albedo, "grey.dds", 28, 28, 0);
    const sim::Vector3 mid{32, 0, 32};
    // Within 0.02 a channel.
    const auto near = [](const Rgb& a, const Rgb& b) { return apart(a, b) < 0.02f; };

    // Test 1: the base normal, from the map's normal maps: here, made from
    // the heights. On ground rising 1 in 2 along x, n = (-1, 2, 0) over
    // its length.
    {
        auto slope = make_ground([](u32 x, u32) { return 0.5f * static_cast<f32>(x); });
        const auto f = frames(*slope, {grey});
        const sim::Vector3 at{32, 16, 32};
        const Rgb seen = light_seen(r, f[0], f[1], at);
        const Rgb want = light_for(original, normalized({-1.0f, 2.0f, 0.0f}));
        t.check(near(seen, want), fmt::format("Test 1: on a 1 in 2 slope, the light {}; FA's "
                                              "formula for its normal gives {}",
                                              rgb_text(seen), rgb_text(want)));
    }

    // Test 2: the strata's normal. A lower stratum tilted 0.6 along x
    // (tangent space, z up) on flat ground: BasisPS gives (0.6, 0.8, 0).
    {
        auto tilted = make_ground([](u32, u32) { return 0.0f; }, path("stratum_tilt.dds"));
        const auto f = frames(*tilted, {grey});
        const Rgb seen = light_seen(r, f[0], f[1], mid);
        const Rgb want = light_for(original, {kTilt, 0.8f, 0.0f});
        t.check(near(seen, want), fmt::format("Test 2: a stratum tilted along x lights the ground "
                                              "{}; its normal gives {}",
                                              rgb_text(seen), rgb_text(want)));
    }

    auto ground = make_ground([](u32, u32) { return 0.0f; });

    // Test 3: a normal decal (TDecalsNormals), its normal tilted 0.6 along
    // its own x: unturned, the ground's normal is (0.6, 0.8, 0); a quarter
    // turn tilts it along z instead, (0, 0.8, 0.6) (TangentMatrix,
    // RotationY as mul(M, v)); at blend a half, the normal's x is half
    // way, 0.3.
    {
        const auto plain =
            frames(*ground, {decal(map::DecalType::Normals, "decal_tilt.dds", 28, 28, 0), grey});
        const auto turned = frames(
            *ground, {decal(map::DecalType::Normals, "decal_tilt.dds", 36, 28, kQuarter), grey});
        const auto half = frames(
            *ground, {decal(map::DecalType::Normals, "decal_tilt_half.dds", 28, 28, 0), grey});
        const Rgb a = light_seen(r, plain[0], plain[1], mid);
        const Rgb b = light_seen(r, turned[0], turned[1], mid);
        const Rgb c = light_seen(r, half[0], half[1], mid);
        const Rgb want_a = light_for(original, {kTilt, 0.8f, 0.0f});
        const Rgb want_b = light_for(original, {0.0f, 0.8f, kTilt});
        const f32 hx = kTilt * 0.5f;
        const Rgb want_c = light_for(original, {hx, std::sqrt(1.0f - hx * hx), 0.0f});
        t.check(near(a, want_a) && near(b, want_b) && near(c, want_c),
                fmt::format("Test 3: a normal decal lights {} ({} wanted); turned a quarter {} "
                            "({}); at blend a half {} ({})",
                            rgb_text(a), rgb_text(want_a), rgb_text(b), rgb_text(want_b),
                            rgb_text(c), rgb_text(want_c)));
    }

    // Test 4: a script's Alpha Normals decal (a tarmac's normals) draws, and
    // fades as the scripts' do: destroyed, a beat on it blends 0.8 of the
    // way: x 0.48.
    {
        ground->set_decals({grey});
        for (const auto& fx : ctx.sim.effect_registry().all())
            if (fx && fx->decal()) fx->mark_destroyed();
        // Past tick 1: a removal fades once the tick passes its removal tick,
        // which Destroy() makes 1 (CDecalManager::RemoveDecals).
        while (ctx.sim.tick_count() < 3) ctx.sim.tick();
        const auto made = ctx.lua_state.do_string(fmt::format(
            "rawset(_G, '_tn_decal', CreateDecal({{32, 0, 32}}, 0, '{}', '', 'Alpha Normals', 8, "
            "8, 1000, 0, 1))",
            path("decal_tilt.dds")));
        if (!made) test_status::fail("Lua: {}", made.error().message);
        std::array<ImageRGBA8, 2> whole;
        for (size_t k = 0; k < 2; ++k) {
            ground->set_lighting(k == 0 ? original : fill, env);
            shots.recapture();
            (void)shots.shoot_frame(*ground, 32, 32, kDistance);
            whole[k] = shots.shoot_frame(*ground, 32, 32, kDistance);
        }
        const Rgb drawn = light_seen(r, whole[0], whole[1], mid);
        // The fade: the next tick after Destroy(), under the light, then the
        // fill frame (unlit: the same whatever the normal).
        (void)ctx.lua_state.do_string("rawget(_G, '_tn_decal'):Destroy()");
        ground->set_lighting(original, env);
        (void)shots.shoot_frame(*ground, 32, 32, kDistance);
        ctx.sim.tick();
        shots.recapture();
        const ImageRGBA8 faded = shots.grab();
        const Rgb fading = light_seen(r, faded, whole[1], mid);
        const f32 fx = kTilt * 0.8f;
        const Rgb want_whole = light_for(original, {kTilt, 0.8f, 0.0f});
        const Rgb want_faded = light_for(original, {fx, std::sqrt(1.0f - fx * fx), 0.0f});
        t.check(near(drawn, want_whole) && near(fading, want_faded),
                fmt::format("Test 4: a script's Alpha Normals decal lights {} ({} wanted); a "
                            "beat into its fade {} ({})",
                            rgb_text(drawn), rgb_text(want_whole), rgb_text(fading),
                            rgb_text(want_faded)));
        for (const auto& fx : ctx.sim.effect_registry().all())
            if (fx && fx->decal()) fx->mark_destroyed();
    }

    // Test 5: a splat lights by the normal target's normal at its pixel,
    // which the normal decals shape: a grey splat over a normal decal tilted
    // along x lights as the tilt gives, not as the flat ground's heights.
    {
        ground->set_decals({decal(map::DecalType::Normals, "decal_tilt.dds", 28, 28, 0)});
        const auto made = ctx.lua_state.do_string(
            fmt::format("CreateSplat({{32, 0, 32}}, 0, '{}', 6, 6, 1000, 0, 1)", path("grey.dds")));
        if (!made) test_status::fail("Lua: {}", made.error().message);
        std::array<ImageRGBA8, 2> f;
        for (size_t k = 0; k < 2; ++k) {
            ground->set_lighting(k == 0 ? original : fill, env);
            shots.recapture();
            (void)shots.shoot_frame(*ground, 32, 32, kDistance);
            f[k] = shots.shoot_frame(*ground, 32, 32, kDistance);
        }
        const Rgb seen = light_seen(r, f[0], f[1], mid);
        const Rgb want = light_for(original, {kTilt, 0.8f, 0.0f});
        t.check(near(seen, want), fmt::format("Test 5: a splat over a tilting normal decal lights "
                                              "{}; the tilt gives {}",
                                              rgb_text(seen), rgb_text(want)));
        for (const auto& fx : ctx.sim.effect_registry().all())
            if (fx && fx->decal()) fx->mark_destroyed();
    }

    spdlog::info("Terrain normal test: {}/{} passed", t.pass, t.pass + t.fail);
}

} // namespace osc::test
