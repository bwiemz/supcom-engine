// --runtime-decal-test (M212c): scripts' decals and splats.
//
// CreateDecal, CreateSplat and CreateSplatOnBone as faf-re's bindings make
// them (centred on their transform), who sees them (CDecalBuffer), how they
// fade once gone (CDecalManager::ProcessRemovals), and the splats' quads
// (TSplats: no mask, no specular). Ground and textures of the test's own;
// the decals made by the sim's Lua.

#include "integration_tests.hpp"
#include "intel_probe.hpp"
#include "plate_fixtures.hpp"
#include "render_probe.hpp"

#include "core/image.hpp"
#include "core/test_status.hpp"
#include "lua/lua_state.hpp"
#include "map/heightmap.hpp"
#include "map/terrain.hpp"
#include "map/visibility_grid.hpp"
#include "renderer/camera.hpp"
#include "renderer/renderer.hpp"
#include "sim/decal.hpp"
#include "sim/sim_state.hpp"
#include "vfs/directory_mount.hpp"
#include "vfs/virtual_file_system.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace osc::test {

namespace {

constexpr const char* kRoot = "/osc_rdecal_test";
constexpr u32 kSize = 64;
constexpr f32 kDistance = 40.0f;
constexpr f32 kQuarter = 1.5707964f;

/// The pixel of `img` at world point `p`, 0-1 a channel.
Rgb pixel_at(renderer::Renderer& r, const ImageRGBA8& img, const sim::Vector3& p) {
    const auto s = screen_of(r, p);
    if (!s || img.width == 0) return {0, 0, 0};
    const u32 x = static_cast<u32>(std::clamp((*s)[0], 0.0f, static_cast<f32>(img.width - 1)));
    const u32 y = static_cast<u32>(std::clamp((*s)[1], 0.0f, static_cast<f32>(img.height - 1)));
    const u8* q = &img.pixels[(static_cast<size_t>(y) * img.width + x) * 4];
    return {q[0] / 255.0f, q[1] / 255.0f, q[2] / 255.0f};
}

f32 redness(const Rgb& c) {
    return c[0] - std::max(c[1], c[2]);
}

f32 apart(const Rgb& a, const Rgb& b) {
    return std::max({std::abs(a[0] - b[0]), std::abs(a[1] - b[1]), std::abs(a[2] - b[2])});
}

/// How far a red decal (albedo alone, under the white fill) covers the
/// ground: its green and blue are the ground's times one less its alpha.
f32 cover(const Rgb& with, const Rgb& bare) {
    const f32 ground = bare[1] + bare[2];
    return ground > 0.0f ? 1.0f - (with[1] + with[2]) / ground : 0.0f;
}

} // namespace

void test_runtime_decal(TestContext& ctx) {
    spdlog::info("=== RUNTIME DECAL TEST: scripts' decals and splats (M212c) ===");
    Tally t;
    const map::ScmapLighting original = ctx.sim.terrain()->lighting();
    const map::ScmapEnvironment env = ctx.sim.terrain()->environment();

    const auto dir = std::filesystem::temp_directory_path() / "osc_rdecal_test";
    std::filesystem::create_directories(dir);
    const auto flat = [](u8 r_, u8 g_, u8 b_, u8 a_) {
        return [=](int, u32, u32) { return std::array<u8, 4>{r_, g_, b_, a_}; };
    };
    write_dds(dir / "red.dds", 1, flat(255, 0, 0, 255));
    write_dds(dir / "red_half.dds", 1, flat(255, 0, 0, 128));
    write_dds(dir / "grey.dds", 1, flat(128, 128, 128, 255));
    write_dds(dir / "spec_r.dds", 1, flat(255, 0, 0, 0));
    ctx.vfs.mount(kRoot, std::make_unique<vfs::DirectoryMount>(dir));
    const auto tex = [](const char* name) { return fmt::format("'{}/{}'", kRoot, name); };

    // Flat ground of the test's own, one stratum.
    auto ground = [&] {
        constexpr f32 kScale = 1.0f / 128.0f;
        std::vector<u16> heights(static_cast<size_t>(kSize + 1) * (kSize + 1), 0);
        auto g = std::make_unique<map::Terrain>(
            map::Heightmap(kSize, kSize, kScale, std::move(heights)), 0.0f, false);
        std::vector<map::StratumInfo> strata(10);
        for (auto& st : strata) st.albedo_scale = st.normal_scale = 4.0f;
        strata[0].albedo_path = "/env/evergreen2/layers/eg_gravel005_albedo.dds";
        g->set_strata(std::move(strata), {}, {});
        return g;
    }();

    OffscreenShots shots(ctx);
    if (!shots.ok()) {
        t.check(false, "renderer init (no Vulkan?)");
        return;
    }
    renderer::Renderer& r = shots.renderer();
    r.set_decals_enabled(true); // the offscreen shots draw none by default
    const map::ScmapLighting fill = white_fill();

    const auto lua = [&](const std::string& code) {
        const auto result = ctx.lua_state.do_string(code);
        if (!result) test_status::fail("Lua: {}", result.error().message);
    };
    // Every decal and splat scripts made, gone (as long since).
    const auto forget = [&] {
        for (const auto& fx : ctx.sim.effect_registry().all())
            if (fx && fx->decal()) fx->mark_destroyed();
    };
    // A frame of the sim's decals as they are now, under `light`.
    const auto shoot = [&](const map::ScmapLighting& light) {
        ground->set_lighting(light, env);
        shots.recapture();
        // Twice: textures load on the first build.
        (void)shots.shoot_frame(*ground, 32, 32, kDistance);
        return shots.shoot_frame(*ground, 32, 32, kDistance);
    };
    // The next tick's frame, the renderer carrying on from the last.
    const auto next_tick = [&] {
        ctx.sim.tick();
        shots.recapture();
        return shots.grab();
    };
    forget();
    const ImageRGBA8 bare = shoot(fill);

    // Test 1: CreateDecal is centred on its position (CreateDecalFromTransform).
    // An 8 by 4 red decal at (32, 32), headed a quarter turn: its 8 run along
    // z (right is -z), its 4 along x: x in [30, 34], z in [28, 36]. Placed by
    // its corner as a map decal is, it would cover x in [32, 36], z in [24,
    // 32] instead.
    {
        forget();
        lua(fmt::format("CreateDecal({{32, 0, 32}}, {}, {}, '', 'Albedo', 8, 4, 1000, 0, 1)",
                        kQuarter, tex("red.dds")));
        const ImageRGBA8 img = shoot(fill);
        const f32 in = std::min(redness(pixel_at(r, img, {31, 0, 34})),
                                redness(pixel_at(r, img, {33, 0, 30})));
        f32 out = 0;
        for (const sim::Vector3& p :
             {sim::Vector3{35, 0, 26}, sim::Vector3{29, 0, 32}, sim::Vector3{35, 0, 33},
              sim::Vector3{31, 0, 27}, sim::Vector3{32, 0, 37}})
            out = std::max(out, apart(pixel_at(r, img, p), pixel_at(r, bare, p)));
        t.check(in > 0.5f && out < 0.05f,
                fmt::format("Test 1: CreateDecal colours its footprint about its position {:.2f} "
                            "red, and moves the ground around it at most {:.3f}",
                            in, out));
    }

    // Test 2: it draws as the map decal it amounts to does (M212b): the one
    // at its corner (30, 36), turned -h. A grey decal under SCMP_009's light.
    {
        forget();
        lua(fmt::format("CreateDecal({{32, 0, 32}}, {}, {}, '', 'Albedo', 8, 4, 1000, 0, 1)",
                        kQuarter, tex("grey.dds")));
        const ImageRGBA8 runtime = shoot(original);
        forget();
        map::DecalInfo d;
        d.type = map::DecalType::Albedo;
        d.texture_path = fmt::format("{}/grey.dds", kRoot);
        d.position_x = 30;
        d.position_z = 36;
        d.scale_x = 8;
        d.scale_z = 4;
        d.rotation_y = -kQuarter;
        d.cut_off_lod = 1000;
        ground->set_decals({d});
        const ImageRGBA8 mapped = shoot(original);
        ground->set_decals({});
        f32 worst = 0;
        for (const sim::Vector3& p :
             {sim::Vector3{32, 0, 32}, sim::Vector3{31, 0, 29}, sim::Vector3{33, 0, 35}})
            worst = std::max(worst, apart(pixel_at(r, runtime, p), pixel_at(r, mapped, p)));
        t.check(worst < 0.01f,
                fmt::format("Test 2: a runtime decal and its map decal differ by {:.3f}", worst));
    }

    // Test 3: its lifetime. Lasting 0.5 s, a decal goes at its tick + 5; then
    // it fades 0.2 a beat (ProcessRemovals), gone in five.
    {
        forget();
        lua(fmt::format("CreateDecal({{32, 0, 32}}, 0, {}, '', 'Albedo', 8, 8, 1000, 0.5, 1)",
                        tex("red.dds")));
        (void)shoot(fill);
        std::array<f32, 10> seen{};
        for (size_t k = 0; k < seen.size(); ++k)
            seen[k] = cover(pixel_at(r, next_tick(), {32, 0, 32}), pixel_at(r, bare, {32, 0, 32}));
        // seen[k] is tick + k + 1.
        t.check(seen[3] > 0.97f && std::abs(seen[4] - 0.8f) < 0.03f &&
                    std::abs(seen[6] - 0.4f) < 0.03f && seen[8] < 0.02f,
                fmt::format("Test 3: a 0.5 s decal covers {:.2f} at 4 ticks, {:.2f} at 5, {:.2f} "
                            "at 7, {:.2f} at 9",
                            seen[3], seen[4], seen[6], seen[8]));
    }

    // Test 4: Destroy() starts the same fade.
    {
        forget();
        lua(fmt::format("rawset(_G, '_rd_decal', CreateDecal({{32, 0, 32}}, 0, {}, '', 'Albedo', "
                        "8, 8, 1000, 0, 1))",
                        tex("red.dds")));
        const f32 whole =
            cover(pixel_at(r, shoot(fill), {32, 0, 32}), pixel_at(r, bare, {32, 0, 32}));
        lua("rawget(_G, '_rd_decal'):Destroy()");
        const f32 first =
            cover(pixel_at(r, next_tick(), {32, 0, 32}), pixel_at(r, bare, {32, 0, 32}));
        const f32 second =
            cover(pixel_at(r, next_tick(), {32, 0, 32}), pixel_at(r, bare, {32, 0, 32}));
        t.check(whole > 0.97f && std::abs(first - 0.8f) < 0.03f && std::abs(second - 0.6f) < 0.03f,
                fmt::format("Test 4: destroyed, a decal covers {:.2f}, then {:.2f}, {:.2f}", whole,
                            first, second));
    }

    // Test 5: a splat's alpha is its albedo's (SplatsPS: decalAlbedo.w times
    // the splat's), all over, to its corners. A half-alpha red splat 8 by 8
    // at (32, 32) covers a half at its middle and a quarter unit in from its
    // corner. (It has no mask, but the decals' mask is whole but for its
    // border texel, which only zeroes the lookup beyond a decal's footprint:
    // within one the two agree.)
    {
        forget();
        lua(fmt::format("CreateSplat({{32, 0, 32}}, 0, {}, 8, 8, 1000, 0, 1)",
                        tex("red_half.dds")));
        const ImageRGBA8 splat = shoot(fill);
        const sim::Vector3 mid{32, 0, 32};
        const sim::Vector3 corner{28.25f, 0, 28.25f};
        const sim::Vector3 beyond{27.5f, 0, 27.5f};
        const f32 splat_mid = cover(pixel_at(r, splat, mid), pixel_at(r, bare, mid));
        const f32 splat_corner = cover(pixel_at(r, splat, corner), pixel_at(r, bare, corner));
        const f32 splat_beyond = cover(pixel_at(r, splat, beyond), pixel_at(r, bare, beyond));
        t.check(std::abs(splat_mid - 0.5f) < 0.03f && std::abs(splat_corner - 0.5f) < 0.03f &&
                    std::abs(splat_beyond) < 0.02f,
                fmt::format("Test 5: a half-alpha splat covers {:.2f} at its middle, {:.2f} at "
                            "its corner, {:.2f} beyond it",
                            splat_mid, splat_corner, splat_beyond));
    }

    // Test 6: a splat takes no specular (SplatsPS's CalculateLighting at 0).
    // Under a sun the view glints off, a grey splat lights as a decal with no
    // specular texture does, and less than one with a full red specular.
    {
        map::ScmapLighting glint = original;
        {
            renderer::Camera cam; // placed as shoot places the renderer's
            cam.init(static_cast<f32>(kSize), static_cast<f32>(kSize));
            cam.set_pitch(OffscreenShots::kPitch); // the shots' held pitch
            cam.set_target(32.0f, 32.0f);
            cam.set_eye_distance(kDistance);
            f32 ex = 0;
            f32 ey = 0;
            f32 ez = 0;
            cam.eye_position(ex, ey, ez);
            const f32 vx = 32.0f - ex;
            const f32 vy = -ey;
            const f32 vz = 32.0f - ez;
            const f32 len = std::sqrt(vx * vx + vy * vy + vz * vz);
            glint.sun_direction[0] = vx / len;
            glint.sun_direction[1] = -vy / len;
            glint.sun_direction[2] = vz / len;
            for (f32& c : glint.specular) c = 1.0f;
        }
        const auto shine = [&](const std::string& make) {
            forget();
            lua(make);
            const Rgb c = pixel_at(r, shoot(glint), {32, 0, 32});
            return (c[0] + c[1] + c[2]) / 3.0f;
        };
        const f32 splat = shine(
            fmt::format("CreateSplat({{32, 0, 32}}, 0, {}, 8, 8, 1000, 0, 1)", tex("grey.dds")));
        const f32 plain = shine(fmt::format(
            "CreateDecal({{32, 0, 32}}, 0, {}, '', 'Albedo', 8, 8, 1000, 0, 1)", tex("grey.dds")));
        const f32 shiny =
            shine(fmt::format("CreateDecal({{32, 0, 32}}, 0, {}, {}, 'Albedo', 8, 8, 1000, 0, 1)",
                              tex("grey.dds"), tex("spec_r.dds")));
        t.check(std::abs(splat - plain) < 0.02f && shiny - splat > 0.1f,
                fmt::format("Test 6: glinting, a splat {:.2f}, a decal without specular {:.2f}, "
                            "one with {:.2f}",
                            splat, plain, shiny));
    }

    // Test 7: a splat's fade is 0.03 a beat. Lasting 0.3 s, it goes at its
    // tick + 3; ten beats on it covers 0.7.
    {
        forget();
        lua(fmt::format("CreateSplat({{32, 0, 32}}, 0, {}, 8, 8, 1000, 0.3, 1)", tex("red.dds")));
        (void)shoot(fill);
        std::array<f32, 12> seen{};
        for (size_t k = 0; k < seen.size(); ++k)
            seen[k] = cover(pixel_at(r, next_tick(), {32, 0, 32}), pixel_at(r, bare, {32, 0, 32}));
        // seen[k] is tick + k + 1: the fade's first beat at + 3.
        t.check(seen[1] > 0.97f && std::abs(seen[2] - 0.97f) < 0.02f &&
                    std::abs(seen[11] - 0.7f) < 0.03f,
                fmt::format("Test 7: a 0.3 s splat covers {:.2f} at 2 ticks, {:.2f} at 3, {:.2f} "
                            "at 12",
                            seen[1], seen[2], seen[11]));
    }

    // Test 8: who sees them (CDecalBuffer). With the fog on for army 1, an
    // enemy's decal where army 1 can't see isn't drawn, and shows once army 1
    // sees the spot; an enemy's splat is drawn anyway.
    {
        forget();
        const map::VisibilityGrid* grid = ctx.sim.visibility_grid();
        const bool unseen =
            grid && !grid->any_vision(28, 28, 36, 36, 0) && !grid->any_vision(28, 40, 36, 48, 0);
        lua(fmt::format("CreateDecal({{32, 0, 32}}, 0, {}, '', 'Albedo', 8, 8, 1000, 0, 2)",
                        tex("red.dds")));
        lua(fmt::format("CreateSplat({{32, 0, 44}}, 0, {}, 8, 8, 1000, 0, 2)", tex("red.dds")));
        r.set_fog_enabled(true);
        r.set_player_army(0);
        const ImageRGBA8 before = shoot(fill);
        const f32 decal_before = redness(pixel_at(r, before, {32, 0, 32}));
        const f32 splat_before = redness(pixel_at(r, before, {32, 0, 44}));
        // Army 1 sees the spot, and on its turn (a tick in the armies'
        // round) comes to see the decal.
        ctx.sim.add_temp_vision(0, 32, 32, 24, 5.0f);
        ImageRGBA8 after;
        for (size_t k = 0; k <= ctx.sim.army_count(); ++k) after = next_tick();
        const f32 decal_after = redness(pixel_at(r, after, {32, 0, 32}));
        r.set_fog_enabled(false);
        t.check(
            unseen && decal_before < 0.03f && splat_before > 0.15f && decal_after > 0.3f,
            fmt::format("Test 8: army 1 {} the spot; an enemy's decal {:.2f} red unseen, {:.2f} "
                        "seen; its splat {:.2f}",
                        unseen ? "can't see" : "already sees", decal_before, decal_after,
                        splat_before));
    }

    // Test 9: CreateSplatOnBone lies at its bone, turned by it, the offset
    // turned too. Army 1's commander at (22, 32) headed a quarter turn (+x):
    // an 8 by 2 splat 10 ahead of it lies at (32, 32), its 8 along z.
    {
        forget();
        const u32 acu = army_acu_id(ctx.sim, 0);
        sim::Entity* e = ctx.sim.entity_registry().find(acu);
        if (e) {
            e->set_position({22, 0, 32});
            e->set_orientation(sim::heading_quaternion(kQuarter));
        }
        lua(fmt::format("CreateSplatOnBone(GetEntityById({}), {{0, 0, 10}}, 0, {}, 8, 2, 1000, 0, "
                        "1)",
                        acu, tex("red.dds")));
        const ImageRGBA8 img = shoot(fill);
        const f32 along = redness(pixel_at(r, img, {32, 0, 35}));
        const f32 across = redness(pixel_at(r, img, {35, 0, 32}));
        const f32 unturned = redness(pixel_at(r, img, {22, 0, 42}));
        t.check(e && along > 0.5f && across < 0.05f && unturned < 0.05f,
                fmt::format("Test 9: a splat on a turned bone: {:.2f} red along its turn, {:.2f} "
                            "across, {:.2f} where an unturned offset would put it",
                            along, across, unturned));
    }

    // Test 10: a splat's LOD fade, by the camera's metric at its first
    // corner (HighFidelityTerrain's splat pass), not its middle: a red splat
    // 8 by 8 at (32, 32), its first corner (28, 28), whose cutoff puts that
    // corner's metric at 7/8 of it, covers a half; within 3/4, all; past
    // it, it isn't drawn.
    {
        const renderer::Camera& cam = r.camera(); // as the shots place it
        f32 ex = 0;
        f32 ey = 0;
        f32 ez = 0;
        cam.eye_position(ex, ey, ez);
        const auto v = cam.view();
        const auto metric_at = [&](f32 x, f32 z) {
            const f32 depth = -(v[2] * (x - ex) + v[6] * (0.0f - ey) + v[10] * (z - ez));
            const f32 aspect = static_cast<f32>(bare.width) / static_cast<f32>(bare.height);
            return 2.0f * cam.tan_half_fov_y(aspect) * aspect * depth;
        };
        const f32 corner = metric_at(28, 28);
        const f32 middle = metric_at(32, 32);
        const auto covered = [&](f32 cutoff) {
            forget();
            lua(fmt::format("CreateSplat({{32, 0, 32}}, 0, {}, 8, 8, {}, 0, 1)", tex("red.dds"),
                            cutoff));
            return cover(pixel_at(r, shoot(fill), {32, 0, 32}), pixel_at(r, bare, {32, 0, 32}));
        };
        const f32 whole = covered(corner / 0.7f);
        const f32 half = covered(corner / 0.875f);
        const f32 gone = covered(corner / 1.05f);
        t.check(whole > 0.97f && std::abs(half - 0.5f) < 0.06f && gone < 0.02f,
                fmt::format("Test 10: at its corner's metric {:.1f} (its middle's {:.1f}), a splat "
                            "covers {:.2f} within 3/4 of its cutoff, {:.2f} at 7/8, {:.2f} past it",
                            corner, middle, whole, half, gone));
    }

    // Test 11: a script's Glow decal (M212d) adds to the frame's glow as a
    // map's does: its albedo's alpha times a quarter of the mask.
    {
        const auto glow_at_middle = [&] {
            (void)shoot(fill);
            renderer::Renderer::SceneImage scene;
            r.request_scene_capture(
                [&](renderer::Renderer::SceneImage image) { scene = std::move(image); });
            (void)shots.grab();
            const auto s = screen_of(r, {32, 0, 32});
            if (!s || scene.width == 0) return -1.0f;
            const u32 x = static_cast<u32>((*s)[0]);
            const u32 y = static_cast<u32>((*s)[1]);
            return scene.rgba[(static_cast<size_t>(y) * scene.width + x) * 4 + 3];
        };
        forget();
        const f32 before = glow_at_middle();
        lua(fmt::format("CreateDecal({{32, 0, 32}}, 0, {}, '', 'Glow', 8, 8, 1000, 0, 1)",
                        tex("red.dds")));
        const f32 after = glow_at_middle();
        t.check(std::abs(after - before - 0.25f) < 0.005f,
                fmt::format("Test 11: a script's Glow decal adds {:.3f} glow (0.25 wanted)",
                            after - before));
    }
    forget();

    spdlog::info("Runtime decal test: {}/{} passed", t.pass, t.pass + t.fail);
}

} // namespace osc::test
