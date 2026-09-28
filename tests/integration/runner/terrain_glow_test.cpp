// --terrain-glow-test (M212f), on Varga Pass (SCMP_023): terrain.fx's
// TTerrainGlow. The lava stratum's alpha is the frame's glow; it scrolls by
// the terrain's Time, which moves on only when the terrain re-tessellates
// (the camera moved, or decals changed).

#include "core/test_status.hpp"
#include "integration_tests.hpp"
#include "intel_probe.hpp"
#include "map/heightmap.hpp"
#include "map/terrain.hpp"
#include "plate_fixtures.hpp"
#include "render_probe.hpp"
#include "renderer/camera.hpp"
#include "renderer/renderer.hpp"
#include "sim/entity_registry.hpp"
#include "sim/sim_state.hpp"
#include "vfs/directory_mount.hpp"
#include "vfs/virtual_file_system.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <vector>

namespace osc::test {

namespace {

/// The map's strongest lava (its stratum 1 mask at 255), in the thickest of
/// its cracks.
constexpr f32 kLavaX = 392.0f;
constexpr f32 kLavaZ = 90.0f;
constexpr f32 kDistance = 40.0f;
/// Glow above this is the lava's (the terrain's base is 0.01).
constexpr f32 kGlowing = 0.05f;
/// The test's own terrain (Test 5): flat, this many units across; its lava
/// stratum one ramp texture across it.
constexpr u32 kSize = 64;
constexpr u32 kRamp = 256;
constexpr const char* kRoot = "/osc_terrain_glow_test";

void run(TestContext& ctx, OffscreenShots& shots, int ticks) {
    for (int i = 0; i < ticks; ++i) {
        ctx.sim.tick();
        shots.recapture();
        shots.redraw();
    }
}

renderer::Renderer::SceneImage scene(renderer::Renderer& r, OffscreenShots& shots) {
    renderer::Renderer::SceneImage image;
    r.request_scene_capture([&](renderer::Renderer::SceneImage s) { image = std::move(s); });
    (void)shots.grab();
    return image;
}

size_t glowing(const renderer::Renderer::SceneImage& s) {
    size_t n = 0;
    for (size_t i = 3; i < s.rgba.size(); i += 4) n += s.rgba[i] > kGlowing ? 1 : 0;
    return n;
}

} // namespace

void test_terrain_glow(TestContext& ctx) {
    spdlog::info("=== Terrain glow test (M212f) ===");
    Tally t;
    const map::Terrain* terrain = ctx.sim.terrain();
    if (!terrain) {
        t.check(false, "the map");
        return;
    }
    // Test 1: Varga Pass is a TTerrainGlow map
    t.check(terrain->environment().terrain_shader == "TTerrainGlow",
            fmt::format("Test 1: the map's terrain shader is {} (TTerrainGlow)",
                        terrain->environment().terrain_shader));
    OffscreenShots shots(ctx);
    if (!shots.ok()) {
        t.check(false, "renderer init (no Vulkan?)");
        return;
    }
    renderer::Renderer& r = shots.renderer();

    // No unit near the lava the test looks at: the frames it compares are
    // the terrain's
    f32 nearest = 1e9f;
    ctx.sim.entity_registry().for_each([&](const sim::Entity& e) {
        if (e.destroyed() || !e.is_unit()) return;
        nearest = std::min(nearest, std::hypot(e.position().x - kLavaX, e.position().z - kLavaZ));
    });

    // Test 2: the lava glows by its stratum's alpha; the same terrain drawn
    // as TTerrain doesn't
    (void)shots.shoot(*terrain, kLavaX, kLavaZ, kDistance);
    const auto lit = scene(r, shots);
    map::Terrain plain = *terrain;
    map::ScmapEnvironment env = terrain->environment();
    env.terrain_shader = "TTerrain";
    plain.set_lighting(terrain->lighting(), env);
    (void)shots.shoot(plain, kLavaX, kLavaZ, kDistance);
    const auto dull = scene(r, shots);
    f32 top = 0.0f;
    f32 floor = 1.0f;
    for (size_t i = 3; i < lit.rgba.size(); i += 4) {
        top = std::max(top, lit.rgba[i]);
        // The ground's least (what isn't ground, the backdrop, is 0)
        if (lit.rgba[i] > 0.0f) floor = std::min(floor, lit.rgba[i]);
    }
    const size_t lava = glowing(lit);
    t.check(nearest > 60.0f && lava > 200 && glowing(dull) * 10 < lava && top <= 1.011f &&
                std::abs(floor - 0.01f) < 1e-3f,
            fmt::format("Test 2: {} pixels glow over the lava as TTerrainGlow, {} as TTerrain "
                        "(the ground's glow {:.3f} to {:.3f}; the nearest unit {:.0f} away)",
                        lava, glowing(dull), floor, top, nearest));

    // Test 3: with the camera still, the terrain's Time stands, over ticks,
    // and so does the lava
    (void)shots.shoot(*terrain, kLavaX, kLavaZ, kDistance);
    const auto before = scene(r, shots);
    const f32 time0 = r.terrain_time();
    run(ctx, shots, 25);
    const f32 time1 = r.terrain_time();
    const auto still = scene(r, shots);
    size_t same = 0;
    size_t lava_pixels = 0;
    for (size_t i = 3; i < before.rgba.size() && i < still.rgba.size(); i += 4) {
        if (before.rgba[i] <= kGlowing) continue;
        ++lava_pixels;
        same += std::abs(before.rgba[i] - still.rgba[i]) < 1e-4f ? 1 : 0;
    }
    t.check(time1 == time0 && lava_pixels > 0 && same == lava_pixels,
            fmt::format("Test 3: 25 ticks under a still camera, the terrain's Time stays {:.2f} "
                        "({:.2f}), and {} of {} lava pixels are as they were",
                        time0, time1, same, lava_pixels));

    // Test 4: a new scene sets its Time afresh, even at the same view: the
    // same map built again after the ticks takes the tick
    {
        (void)shots.shoot(*terrain, kLavaX, kLavaZ, kDistance);
        const f32 rebuilt = r.terrain_time();
        (void)scene(r, shots); // back to the frame Test 4 moves from
        t.check(rebuilt != time1,
                fmt::format("Test 4: built again at the same view after 25 ticks, the Time is "
                            "{:.2f} (it was {:.2f})",
                            rebuilt, time1));
    }

    // Test 5: moved and back, the Time takes the tick, and the lava has
    // scrolled; the rest of the ground hasn't changed
    r.camera().set_target(kLavaX + 1.0f, kLavaZ);
    shots.redraw();
    r.camera().set_target(kLavaX, kLavaZ);
    shots.redraw();
    const f32 time2 = r.terrain_time();
    const auto moved = scene(r, shots);
    size_t scrolled = 0;
    size_t ground = 0;
    size_t ground_same = 0;
    for (size_t i = 3; i < before.rgba.size() && i < moved.rgba.size(); i += 4) {
        if (before.rgba[i] > kGlowing) {
            scrolled += std::abs(before.rgba[i] - moved.rgba[i]) > 0.02f ? 1 : 0;
        } else if (before.rgba[i] < 0.011f && moved.rgba[i] < 0.011f) {
            ++ground;
            ++ground_same;
        } else if (before.rgba[i] < 0.011f) {
            ++ground;
        }
    }
    const f32 tick = static_cast<f32>(ctx.sim.tick_count());
    // A redraw draws its tick at the interpolant 1
    t.check(time2 > time1 + 20.0f && std::abs(time2 - (tick + 1.0f)) < 1e-3f &&
                scrolled * 4 > lava_pixels && ground_same * 100 >= ground * 99,
            fmt::format("Test 5: moved and back, the Time is {:.2f} (tick {:.0f}); {} of {} lava "
                        "pixels changed, and {} of {} of the rest kept their 0.01",
                        time2, tick, scrolled, lava_pixels, ground_same, ground));

    // Test 6: by how much and which way it scrolls. On a terrain of the
    // test's own, drawn as TTerrainGlow, the lava stratum's alpha is a ramp
    // along u, one texture across the ground, its mask whole; at the middle
    // the glow is the ramp at u = 0.5 + 0.01 cos(Time / 8) (TerrainGlowPS
    // reads the offset swapped), plus 0.01. Two Times.
    {
        const auto dir = std::filesystem::temp_directory_path() / "osc_terrain_glow_test";
        std::filesystem::create_directories(dir);
        write_dds(
            dir / "ramp.dds", 1,
            [](int, u32 column, u32) {
                return std::array<u8, 4>{128, 128, 128, static_cast<u8>(column)};
            },
            kRamp, 4);
        write_dds(
            dir / "mask.dds", 1, [](int, u32, u32) { return std::array<u8, 4>{0, 255, 0, 0}; },
            kSize, kSize);
        write_dds(
            dir / "none.dds", 1, [](int, u32, u32) { return std::array<u8, 4>{0, 0, 0, 0}; }, kSize,
            kSize);
        ctx.vfs.mount(kRoot, std::make_unique<vfs::DirectoryMount>(dir));
        const auto bytes = [&](const char* name) {
            std::ifstream in(dir / name, std::ios::binary);
            return std::vector<char>((std::istreambuf_iterator<char>(in)), {});
        };
        map::Terrain ground(
            map::Heightmap(kSize, kSize, 1.0f / 128.0f,
                           std::vector<u16>(static_cast<size_t>(kSize + 1) * (kSize + 1), 0)),
            0.0f, false);
        map::ScmapEnvironment glow_env;
        glow_env.terrain_shader = "TTerrainGlow";
        ground.set_lighting(white_fill(), glow_env);
        std::vector<map::StratumInfo> strata(10);
        for (auto& st : strata) st.albedo_scale = st.normal_scale = 4.0f;
        strata[0].albedo_path = "/env/evergreen2/layers/eg_gravel005_albedo.dds";
        strata[2].albedo_path = fmt::format("{}/ramp.dds", kRoot); // FA's stratum 1
        strata[2].albedo_scale = static_cast<f32>(kSize);
        ground.set_strata(std::move(strata), bytes("mask.dds"), bytes("none.dds"));

        const sim::Vector3 middle{kSize * 0.5f, 0.0f, kSize * 0.5f};
        const auto glow_at_middle = [&] {
            const auto s = scene(r, shots);
            const auto p = screen_of(r, middle);
            if (!p || s.width == 0) return -1.0f;
            const u32 x = std::min(static_cast<u32>((*p)[0]), s.width - 1);
            const u32 y = std::min(static_cast<u32>((*p)[1]), s.height - 1);
            return s.rgba[(static_cast<size_t>(y) * s.width + x) * 4 + 3];
        };
        // The ramp's texel i is i/255 at its centre, (i + 0.5)/256 along u
        const auto want = [](f32 time) {
            const f32 u = 0.5f + 0.01f * std::cos(time * 0.125f);
            return (u * static_cast<f32>(kRamp) - 0.5f) / 255.0f + 0.01f;
        };
        (void)shots.shoot(ground, middle.x, middle.z, kDistance, false);
        const f32 glow1 = glow_at_middle();
        const f32 time1 = r.terrain_time();
        run(ctx, shots, 30);
        r.camera().set_target(middle.x + 1.0f, middle.z);
        shots.redraw();
        r.camera().set_target(middle.x, middle.z);
        const f32 glow2 = glow_at_middle();
        const f32 time2 = r.terrain_time();
        t.check(std::abs(glow1 - want(time1)) < 2e-3f && std::abs(glow2 - want(time2)) < 2e-3f &&
                    std::abs(time2 - time1) > 20.0f,
                fmt::format("Test 6: at the ramp's middle the glow is {:.4f} at Time {:.2f} "
                            "({:.4f} wanted) and {:.4f} at {:.2f} ({:.4f})",
                            glow1, time1, want(time1), glow2, time2, want(time2)));

        // Test 7: the lava's alpha is zeroed before the blend, so its
        // specular amount (1 - alpha) is whole: under a sun glinting off the
        // middle into the eye, TTerrainGlow's lava shines more than the same
        // ground as TTerrain, whose amount there is 1 - the ramp's half
        map::ScmapLighting glint = white_fill();
        {
            f32 ex = 0.0f;
            f32 ey = 0.0f;
            f32 ez = 0.0f;
            r.camera().eye_position(ex, ey, ez);
            const f32 vx = middle.x - ex;
            const f32 vy = -ey;
            const f32 vz = middle.z - ez;
            const f32 len = std::sqrt(vx * vx + vy * vy + vz * vz);
            glint.sun_direction[0] = vx / len;
            glint.sun_direction[1] = -vy / len;
            glint.sun_direction[2] = vz / len;
            for (f32& c : glint.sun_color) c = 1.0f;
            for (f32& c : glint.specular) c = 1.0f;
        }
        const auto brightness_at_middle = [&](const char* shader) {
            map::ScmapEnvironment env_of;
            env_of.terrain_shader = shader;
            ground.set_lighting(glint, env_of);
            (void)shots.shoot(ground, middle.x, middle.z, kDistance, false);
            const auto s = scene(r, shots);
            const auto p = screen_of(r, middle);
            if (!p || s.width == 0) return -1.0f;
            const u32 x = std::min(static_cast<u32>((*p)[0]), s.width - 1);
            const u32 y = std::min(static_cast<u32>((*p)[1]), s.height - 1);
            const f32* q = &s.rgba[(static_cast<size_t>(y) * s.width + x) * 4];
            return q[0] + q[1] + q[2];
        };
        const f32 lava_shine = brightness_at_middle("TTerrainGlow");
        const f32 plain_shine = brightness_at_middle("TTerrain");
        t.check(lava_shine > plain_shine + 0.1f,
                fmt::format("Test 7: under a glinting sun the lava is {:.3f} bright as "
                            "TTerrainGlow, {:.3f} as TTerrain",
                            lava_shine, plain_shine));
    }

    spdlog::info("Terrain glow test: {} passed, {} failed", t.pass, t.fail);
}

} // namespace osc::test
