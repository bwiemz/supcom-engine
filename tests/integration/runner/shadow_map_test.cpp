// --shadow-map-test (M210c): Moho's shadow map.
//
// The terrain takes the meshes' shadows from a blurred mask and never shadows
// itself; there is no map at shadow fidelity 0, and no light camera past
// ren_ShadowLOD. A white test ground with a ridge across it, and small plates
// (props, which stay where they're put) hovering over it.

#include "integration_tests.hpp"
#include "plate_fixtures.hpp"
#include "render_probe.hpp"

#include "blueprints/blueprint_store.hpp"
#include "lua/lua_state.hpp"
#include "map/heightmap.hpp"
#include "map/terrain.hpp"
#include "renderer/camera.hpp"
#include "sim/entity.hpp"
#include "sim/sim_state.hpp"
#include "sim/world_snapshot.hpp"
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

constexpr u32 kSize = 512;
constexpr const char* kRoot = "/osc_shadow_map_test";
// The ridge: along z at x 100, 20 high, its sides 10 wide.
constexpr f32 kRidgeX = 100.0f;
constexpr f32 kRidgeHeight = 20.0f;

f32 mean_brightness(const Pixels& p) {
    f32 sum = 0.0f;
    for (const auto& px : p) sum += (px[0] + px[1] + px[2]) / 3.0f;
    return p.empty() ? 0.0f : sum / static_cast<f32>(p.size());
}

} // namespace

void test_shadow_map(TestContext& ctx) {
    spdlog::info("=== SHADOW MAP TEST: Moho's shadow map (M210c) ===");
    Tally t;

    OffscreenShots shots(ctx);
    if (!shots.ok()) {
        t.check(false, "renderer init (no Vulkan?)");
        return;
    }
    renderer::Renderer& r = shots.renderer();

    // Flat ground at 0 but for the ridge.
    constexpr f32 kScale = 1.0f / 128.0f;
    std::vector<u16> heights(static_cast<size_t>(kSize + 1) * (kSize + 1));
    for (u32 z = 0; z <= kSize; ++z)
        for (u32 x = 0; x <= kSize; ++x) {
            const f32 d = std::abs(static_cast<f32>(x) - kRidgeX);
            const f32 h = std::max(0.0f, kRidgeHeight * (1.0f - d / 10.0f));
            heights[static_cast<size_t>(z) * (kSize + 1) + x] = static_cast<u16>(h / kScale);
        }
    map::Terrain ground(map::Heightmap(kSize, kSize, kScale, std::move(heights)), 0.0f, false);

    const auto dir = std::filesystem::temp_directory_path() / "osc_shadow_map_test";
    std::filesystem::create_directories(dir);
    write_plate_scm(dir / "plate_hover.scm", 2.0f);
    const auto flat = [](u8 r_, u8 g_, u8 b_, u8 a_) {
        return [=](int, u32, u32) { return std::array<u8, 4>{r_, g_, b_, a_}; };
    };
    write_dds(dir / "plate_normals.dds", 1, flat(0, 128, 0, 128));
    write_dds(dir / "albedo_white.dds", 1, flat(255, 255, 255, 255));
    write_dds(dir / "spec_none.dds", 1, flat(0, 0, 0, 0));
    ctx.vfs.mount(kRoot, std::make_unique<vfs::DirectoryMount>(dir));
    {
        // A white ground: its light alone shows.
        std::vector<map::StratumInfo> strata(10);
        for (auto& st : strata) st.albedo_scale = st.normal_scale = 4.0f;
        strata[0].albedo_path = std::string(kRoot) + "/albedo_white.dds";
        ground.set_strata(std::move(strata), {}, {});
    }

    std::vector<std::string> prop_bps;
    {
        const std::vector<std::string> world = sim::world_blueprints(ctx.sim);
        for (const auto* e : ctx.sim.blueprint_store()->get_all(blueprints::BlueprintType::Prop))
            if (prop_bps.size() < 2 && e->id.find("/env/") != std::string::npos &&
                std::find(world.begin(), world.end(), e->id) == world.end())
                prop_bps.push_back(e->id);
    }
    // A small white plate `lift` over the ground at (x, z): a prop.
    size_t next_prop = 0;
    const auto hover = [&](f32 x, f32 z, f32 lift) {
        if (next_prop >= prop_bps.size()) {
            spdlog::warn("out of prop blueprints");
            return;
        }
        Plate p;
        p.albedo = "albedo_white.dds";
        p.specteam = "spec_none.dds";
        p.mesh = "plate_hover.scm";
        stand_plate(ctx, kRoot, prop_bps[next_prop++], p, x, z, 0.0f, true, lift);
    };
    const auto light = [](f32 sx, f32 sy, f32 sz) {
        map::ScmapLighting l = white_fill();
        for (f32& c : l.sun_color) c = 1.0f;
        for (f32& c : l.shadow_fill) c = 0.0f;
        const f32 n = std::sqrt(sx * sx + sy * sy + sz * sz);
        l.sun_direction[0] = sx / n;
        l.sun_direction[1] = sy / n;
        l.sun_direction[2] = sz / n;
        return l;
    };
    const auto env = [] {
        map::ScmapEnvironment e;
        e.terrain_shader = "TTerrain";
        e.cubemaps.emplace_back("<default>", "/textures/environment/no_such_cube.dds");
        return e;
    };
    const auto shoot = [&](const map::ScmapLighting& l, f32 x, f32 z, f32 distance = 30.0f) {
        ground.set_lighting(l, env());
        shots.recapture();
        return shots.shoot(ground, x, z, distance);
    };
    r.camera().set_pitch(1.1f);

    // Test 1: the terrain never shadows itself. The sun low from the ridge's
    // side (1 up for 2 across): a depth comparison would put the ground 10 to
    // 40 behind it in the ridge's shadow; Moho's terrain reads only the
    // meshes' mask, so it's as lit as the ground far away.
    {
        const map::ScmapLighting low = light(-2.0f, 1.0f, 0.0f);
        const f32 behind = mean_brightness(shoot(low, kRidgeX + 25.0f, 200.0f));
        const f32 far = mean_brightness(shoot(low, 400.0f, 200.0f));
        t.check(far > 0.1f && std::abs(behind - far) < far * 0.05f,
                fmt::format("Test 1: behind a ridge from a low sun the ground is {:.3f} bright, "
                            "{:.3f} far from it",
                            behind, far));
    }

    // Test 2: a mesh's shadow on the terrain is the blurred mask's: with
    // ren_ShadowBlur its edge fades over several texels, without it over
    // one (the mask's own bilinear read).
    hover(300.0f, 300.0f, 5.0f);
    for (int i = 0; i < 5; ++i) ctx.sim.tick();
    const map::ScmapLighting high = light(0.3f, 1.0f, 0.2f);
    renderer::Renderer::VideoOptions& video = r.video_options();
    const renderer::Renderer::VideoOptions as_was = video;
    {
        const f32 lit = mean_brightness(shoot(high, 400.0f, 400.0f));
        // Pixels between shadowed and lit: the edge's fade.
        const auto partial = [&](bool blur) {
            video.shadow_blur = blur;
            int n = 0;
            int dark = 0;
            for (const auto& px : shoot(high, 300.0f, 300.0f)) {
                const f32 b = (px[0] + px[1] + px[2]) / 3.0f;
                if (b < 0.1f * lit) ++dark;
                else if (b < 0.9f * lit) ++n;
            }
            return std::array<int, 2>{n, dark};
        };
        const auto blurred = partial(true);
        const auto sharp = partial(false);
        video = as_was;
        t.check(blurred[1] + blurred[0] > 300 && sharp[1] > 300 && blurred[0] > sharp[0] * 2,
                fmt::format("Test 2: a hovering plate's shadow fades over {} pixels blurred ({} "
                            "dark), {} with ren_ShadowBlur off ({} dark)",
                            blurred[0], blurred[1], sharp[0], sharp[1]));
    }

    // Test 3: shadow fidelity 0 makes no map: nothing is shadowed.
    {
        const auto dark_at = [&](int fidelity) {
            video.shadow_fidelity = fidelity;
            int n = 0;
            for (const auto& px : shoot(high, 300.0f, 300.0f))
                if ((px[0] + px[1] + px[2]) / 3.0f < 0.05f) ++n;
            return n;
        };
        const int none = dark_at(0);
        const int some = dark_at(1);
        video = as_was;
        t.check(none == 0 && some > 300,
                fmt::format("Test 3: under the plate {} pixels are dark at shadow fidelity 0, {} "
                            "at 1",
                            none, some));
    }

    // Test 4: no light camera past ren_ShadowLOD (250), the camera's zoom.
    {
        (void)shoot(high, 300.0f, 300.0f, 30.0f);
        const bool near_valid = r.shadow_camera_valid();
        const f32 near_zoom = r.camera().zoom();
        (void)shoot(high, 300.0f, 300.0f, 400.0f);
        const bool far_valid = r.shadow_camera_valid();
        const f32 far_zoom = r.camera().zoom();
        t.check(near_valid && near_zoom <= 250.0f && !far_valid && far_zoom > 250.0f,
                fmt::format("Test 4: a light camera at zoom {:.0f}: {}; at zoom {:.0f}: {}",
                            near_zoom, near_valid, far_zoom, far_valid));
    }

    spdlog::info("Shadow map test: {}/{} passed", t.pass, t.pass + t.fail);
}

} // namespace osc::test
