// --lighting-test (M210a): the map's lighting reaches the lit shaders.
//
// The map data (SCMP_009's and SCMP_010's lighting and environment) is
// checked, then an offscreen renderer draws flat ground of the test's own
// under each lighting. (A map's own land has slopes, trees and their
// shadows, which a close view of it can't avoid.) Each comparison is per
// pixel, between two frames of the same view, and takes the median.

#include "integration_tests.hpp"
#include "render_probe.hpp"

#include "core/image.hpp"
#include "core/test_status.hpp"
#include "map/scmap_parser.hpp"
#include "map/terrain.hpp"
#include "renderer/renderer.hpp"
#include "sim/entity.hpp"
#include "sim/sim_state.hpp"
#include "sim/world_snapshot.hpp"
#include "vfs/virtual_file_system.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace osc::test {

namespace {

bool near(f32 a, f32 b, f32 tolerance) {
    return std::abs(a - b) <= tolerance;
}

bool near3(const f32* v, f32 x, f32 y, f32 z, f32 tolerance) {
    return near(v[0], x, tolerance) && near(v[1], y, tolerance) && near(v[2], z, tolerance);
}

/// The median red-over-blue of a frame's measurable pixels.
f32 median_redness(const Pixels& pixels) {
    std::vector<f32> ratios;
    for (const auto& p : pixels) {
        if (p[2] >= 0.1f) ratios.push_back(p[0] / p[2]);
    }
    return median(ratios);
}

/// FA's light on flat ground (N = up), without specular:
/// L = sun * N.S + ambience; L = multiplier * L + fill * (1 - L).
std::array<f32, 3> flat_ground_light(const map::ScmapLighting& l) {
    const f32* s = l.sun_direction;
    const f32 up = s[1] / std::sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
    std::array<f32, 3> out{};
    for (int c = 0; c < 3; ++c) {
        const f32 light = l.sun_color[c] * up + l.sun_ambience[c];
        out[c] = l.multiplier * light + l.shadow_fill[c] * (1.0f - light);
    }
    return out;
}

map::ScmapLighting unlit(f32 r, f32 g, f32 b) {
    map::ScmapLighting l;
    for (int i = 0; i < 3; ++i) {
        l.sun_color[i] = 0.0f;
        l.sun_ambience[i] = 0.0f;
    }
    l.shadow_fill[0] = r;
    l.shadow_fill[1] = g;
    l.shadow_fill[2] = b;
    for (f32& s : l.specular) s = 0.0f;
    return l;
}

/// A white sun and no fill: the frame is albedo * multiplier * N.S.
map::ScmapLighting sun_only(f32 multiplier, f32 height) {
    map::ScmapLighting l = unlit(0.0f, 0.0f, 0.0f);
    l.multiplier = multiplier;
    l.sun_direction[0] = 0.0f;
    l.sun_direction[1] = height;
    l.sun_direction[2] = std::sqrt(1.0f - height * height);
    for (f32& c : l.sun_color) c = 1.0f;
    return l;
}

} // namespace

void test_lighting(TestContext& ctx) {
    spdlog::info("=== LIGHTING TEST: the map's lighting (M210a) ===");
    Tally t;
    map::Terrain* terrain = ctx.sim.terrain();
    if (!terrain) {
        osc::test_status::fail("[FAIL] no terrain loaded");
        return;
    }
    const map::ScmapLighting original = terrain->lighting();
    const map::ScmapEnvironment original_env = terrain->environment();

    // Test 1: SCMP_009's lighting, as its .scmap holds it. The engine's
    // defaults are SCMP_009's too, so its environment (which defaults to
    // nothing) shows the map was read.
    {
        const auto& l = original;
        const auto& env = original_env;
        const f32 len = std::sqrt(l.sun_direction[0] * l.sun_direction[0] +
                                  l.sun_direction[1] * l.sun_direction[1] +
                                  l.sun_direction[2] * l.sun_direction[2]);
        const bool ok = env.terrain_shader == "TTerrain" && !env.sky_cubemap.empty() &&
                        !env.cubemaps.empty() && near(l.multiplier, 1.54f, 0.01f) &&
                        near3(l.sun_direction, 0.616f, 0.559f, 0.555f, 0.01f) &&
                        near(len, 1.0f, 0.01f) && near3(l.sun_color, 1.38f, 1.29f, 1.14f, 0.01f) &&
                        near3(l.shadow_fill, 0.54f, 0.54f, 0.70f, 0.01f) &&
                        near(l.specular[0], 0.31f, 0.01f) && near(l.bloom, 0.036f, 0.002f) &&
                        near(l.fog_end, 740.0f, 1.0f);
        t.check(ok, fmt::format("Test 1: SCMP_009 is {} (sky '{}', {} cubemaps), x{:.2f}, sun "
                                "({:.2f}, {:.2f}, {:.2f}), fill ({:.2f}, {:.2f}, {:.2f})",
                                env.terrain_shader, env.sky_cubemap, env.cubemaps.size(),
                                l.multiplier, l.sun_color[0], l.sun_color[1], l.sun_color[2],
                                l.shadow_fill[0], l.shadow_fill[1], l.shadow_fill[2]));
    }

    // Test 2: a tropical map's warmer light (SCMP_010, read from its file).
    map::ScmapLighting tropical;
    {
        const auto file = ctx.vfs.read_file("/maps/SCMP_010/SCMP_010.scmap");
        bool ok = false;
        if (file) {
            const std::vector<u8> bytes(file->begin(), file->end());
            auto parsed = map::parse_scmap(bytes);
            if (parsed.ok()) {
                tropical = parsed.value().lighting;
                const auto& l = tropical;
                ok = parsed.value().environment.terrain_shader == "TTerrain" &&
                     near(l.multiplier, 1.43f, 0.01f) &&
                     near3(l.sun_color, 1.83f, 1.29f, 0.95f, 0.01f) &&
                     near3(l.shadow_fill, 0.64f, 0.61f, 0.45f, 0.01f);
            }
        }
        t.check(ok, fmt::format("Test 2: SCMP_010 is x{:.2f}, sun ({:.2f}, {:.2f}, {:.2f}), "
                                "fill ({:.2f}, {:.2f}, {:.2f})",
                                tropical.multiplier, tropical.sun_color[0], tropical.sun_color[1],
                                tropical.sun_color[2], tropical.shadow_fill[0],
                                tropical.shadow_fill[1], tropical.shadow_fill[2]));
    }

    // The rendering: flat ground of the test's own -- one stratum, no normal
    // map, no props or units -- so a frame's middle is the lit albedo alone,
    // with the normal straight up.
    constexpr u32 kSize = 64;
    constexpr f32 kCentre = kSize / 2.0f;
    constexpr f32 kDistance = 30.0f;
    map::Terrain ground(
        map::Heightmap(kSize, kSize, 1.0f / 128.0f,
                       std::vector<u16>(static_cast<size_t>(kSize + 1) * (kSize + 1), 0)),
        0.0f, false);
    {
        std::vector<map::StratumInfo> strata(10);
        for (auto& st : strata) {
            st.albedo_scale = 4.0f;
            st.normal_scale = 4.0f;
        }
        // Grey gravel, opaque (DXT1): alpha 1 for the glint in Test 9.
        strata[0].albedo_path = "/env/evergreen2/layers/eg_gravel005_albedo.dds";
        ground.set_strata(std::move(strata), {}, {});
    }

    OffscreenShots shots(ctx);
    if (!shots.ok()) {
        t.check(false, "Tests 3-10: renderer init (no Vulkan?)");
        spdlog::info("Lighting test: {}/{} passed", t.pass, t.pass + t.fail);
        return;
    }

    // The ground under `lighting` (and SCMP_009's environment unless one is
    // given), from close up.
    const auto shoot = [&](const map::ScmapLighting& lighting,
                           const map::ScmapEnvironment& env = map::ScmapEnvironment{}) {
        ground.set_lighting(lighting, env.terrain_shader.empty() ? original_env : env);
        return shots.shoot(ground, kCentre, kCentre, kDistance, /*with_world=*/false);
    };

    const auto evergreen_px = shoot(original);
    const auto tropical_px = shoot(tropical);
    const auto white_fill_px = shoot(unlit(1.0f, 1.0f, 1.0f));
    const auto tinted_fill_px = shoot(unlit(0.5f, 0.25f, 1.0f));
    const auto sun_px = shoot(sun_only(1.0f, 1.0f));
    const auto brighter_sun_px = shoot(sun_only(1.5f, 1.0f));
    const auto lower_sun_px = shoot(sun_only(1.0f, 0.8f));

    // A sun where the view of the centre reflects off flat ground, with a
    // full specular colour: both terrain shaders glint there, TTerrain where
    // the albedo's alpha is low and TTerrainXP where it is high.
    map::ScmapLighting glint = original;
    {
        renderer::Camera cam; // placed as shoot() places the renderer's
        cam.init(static_cast<f32>(kSize), static_cast<f32>(kSize));
        cam.set_pitch(OffscreenShots::kPitch); // the shots' held pitch
        cam.set_target(kCentre, kCentre);
        cam.set_eye_distance(kDistance);
        f32 ex = 0, ey = 0, ez = 0;
        cam.eye_position(ex, ey, ez);
        const f32 vx = kCentre - ex;
        const f32 vy = -ey; // the ground is at height 0
        const f32 vz = kCentre - ez;
        const f32 len = std::sqrt(vx * vx + vy * vy + vz * vz);
        glint.sun_direction[0] = vx / len;
        glint.sun_direction[1] = -vy / len;
        glint.sun_direction[2] = vz / len;
        for (f32& c : glint.specular) c = 1.0f;
    }
    map::ScmapEnvironment xp_env = original_env;
    xp_env.terrain_shader = "TTerrainXP";
    const auto glint_px = shoot(glint);
    const auto glint_xp_px = shoot(glint, xp_env);

    if (evergreen_px.empty() || tinted_fill_px.empty() || lower_sun_px.empty()) {
        t.check(false, "Tests 3-8: frames captured");
    } else {
        // Tests 3-4: under each map's own light, flat ground takes FA's
        // light: its frame over the white fill's (the albedo alone). The
        // first frame is SCMP_009's, drawn on the scene's first build.
        for (const auto& [name, lighting, px] : {std::tuple{"SCMP_009", original, &evergreen_px},
                                                 std::tuple{"SCMP_010", tropical, &tropical_px}}) {
            const auto want = flat_ground_light(lighting);
            std::array<f32, 3> got{};
            for (int c = 0; c < 3; ++c) got[c] = median_ratio(*px, white_fill_px, c);
            t.check(near(got[0], want[0], 0.02f) && near(got[1], want[1], 0.02f) &&
                        near(got[2], want[2], 0.02f),
                    fmt::format("Test {}: flat ground under {}'s light takes ({:.3f}, {:.3f}, "
                                "{:.3f}); FA's formula gives ({:.3f}, {:.3f}, {:.3f})",
                                std::string(name) == "SCMP_009" ? 3 : 4, name, got[0], got[1],
                                got[2], want[0], want[1], want[2]));
        }

        // Test 5: the tropical sun (1.83, 1.29, 0.95) makes a redder frame
        // than the evergreen one (1.38, 1.29, 1.14): about 1.4 times the
        // red over blue, less where bright ground clips.
        const f32 evergreen = median_redness(evergreen_px);
        const f32 warm = median_redness(tropical_px);
        t.check(warm > evergreen * 1.15f,
                fmt::format("Test 5: red over blue is {:.3f} under SCMP_010's light, {:.3f} "
                            "under SCMP_009's",
                            warm, evergreen));

        // Test 6: with no sun, the ground takes the shadow fill colour:
        // albedo * fill, so a (0.5, 0.25, 1) fill scales a white fill's
        // frame by that.
        const f32 r = median_ratio(tinted_fill_px, white_fill_px, 0);
        const f32 g = median_ratio(tinted_fill_px, white_fill_px, 1);
        const f32 b = median_ratio(tinted_fill_px, white_fill_px, 2);
        t.check(near(r, 0.5f, 0.04f) && near(g, 0.25f, 0.04f) && near(b, 1.0f, 0.04f),
                fmt::format("Test 6: a (0.5, 0.25, 1) shadow fill scales the frame by "
                            "({:.3f}, {:.3f}, {:.3f})",
                            r, g, b));

        // Test 7: the lighting multiplier scales the sunlight. The sun is
        // overhead, where the shadow map must still look down it.
        const f32 m = median_ratio(brighter_sun_px, sun_px, 1);
        t.check(near(m, 1.5f, 0.06f),
                fmt::format("Test 7: a 1.5 multiplier scales the sunlit frame by {:.3f}", m));

        // Test 8: the light follows the sun's direction: flat ground takes
        // N.S of it, so a sun 0.8 high lights it 0.8 as much as one overhead
        // (the terrain's normal maps, decoded as FA does, stand up from it).
        const f32 d = median_ratio(lower_sun_px, sun_px, 1);
        t.check(near(d, 0.8f, 0.02f),
                fmt::format("Test 8: a sun 0.8 high lights flat ground {:.3f} as much as "
                            "one overhead",
                            d));
    }
    // Test 9: the map's terrain shader picks the formula. Under a glint,
    // TTerrain's frame and TTerrainXP's differ.
    if (!glint_px.empty() && glint_px.size() == glint_xp_px.size()) {
        f32 diff = 0.0f;
        for (size_t i = 0; i < glint_px.size(); ++i) {
            for (int c = 0; c < 3; ++c) diff += std::abs(glint_px[i][c] - glint_xp_px[i][c]);
        }
        diff /= static_cast<f32>(glint_px.size() * 3);
        t.check(diff > 0.02f,
                fmt::format("Test 9: under a glint, TTerrain's and TTerrainXP's frames differ by "
                            "{:.3f} a channel",
                            diff));
    } else {
        t.check(false, "Test 9: glint frames captured");
    }

    // Test 10: all of it without a Vulkan validation error (when the layers
    // are on, as in Debug builds): the frames draw with bloom off.
    t.check(renderer::Renderer::validation_error_count() == 0,
            fmt::format("Test 10: {} Vulkan validation errors",
                        renderer::Renderer::validation_error_count()));

    spdlog::info("Lighting test: {}/{} passed", t.pass, t.pass + t.fail);
}

} // namespace osc::test
