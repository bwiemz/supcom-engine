// --clipped-shadow-test (M211j): shadows cut by their albedo's alpha.
//
// FA draws an alpha-tested mesh into the shadow map with DepthClip (or, for
// the swaying trees, UndulatingDepthClip): clip(albedo.a - 0.5). Small
// plates, opaque on one half and clear on the other, hover over wide ones
// under a low sun from the side; their shadows fall clear of them, and the
// test counts the dark pixels.

#include "integration_tests.hpp"
#include "support/temp_path.hpp"
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

// Flat ground under every plate: Moho's light camera fits the terrain in
// view, and casts nothing where there is none (M210c).
constexpr u32 kSize = 512;
constexpr const char* kRoot = "/osc_clipped_shadow_test";
constexpr f32 kLift = 2.5f;

} // namespace

void test_clipped_shadows(TestContext& ctx) {
    spdlog::info("=== CLIPPED SHADOW TEST: DepthClip (M211j) ===");
    Tally t;

    OffscreenShots shots(ctx);
    if (!shots.ok()) {
        t.check(false, "renderer init (no Vulkan?)");
        return;
    }
    renderer::Renderer& r = shots.renderer();

    const auto probe = ctx.lua_state.do_string(
        "__osc_clipped_probe = CreateUnitHPR('ueb0101', 'ARMY_1', 32, 0, 32, 0, 0, 0)\n");
    if (!probe) spdlog::warn("CreateUnitHPR failed: {}", probe.error().message);
    ctx.sim.tick();
    f32 ground_y = 0.0f;
    ctx.sim.entity_registry().for_each([&](const sim::Entity& e) {
        if (e.is_unit() && !e.destroyed() && e.blueprint_id() == "ueb0101")
            ground_y = e.position().y;
    });
    constexpr f32 kScale = 1.0f / 128.0f;
    map::Terrain ground(
        map::Heightmap(kSize, kSize, kScale,
                       std::vector<u16>(static_cast<size_t>(kSize + 1) * (kSize + 1),
                                        static_cast<u16>(ground_y / kScale))),
        0.0f, false);
    {
        std::vector<map::StratumInfo> strata(10);
        for (auto& st : strata) st.albedo_scale = st.normal_scale = 4.0f;
        strata[0].albedo_path = "/env/evergreen2/layers/eg_gravel005_albedo.dds";
        ground.set_strata(std::move(strata), {}, {});
    }

    const TempDir scratch("osc_clipped_shadow_test");
    const auto& dir = scratch.path();
    write_plate_scm(dir / "plate_wide.scm", 8.0f);
    write_plate_scm(dir / "plate_hover.scm", 2.0f);
    write_wall_scm(dir / "wall.scm", 100.0f, 100.0f);
    const auto flat = [](u8 r_, u8 g_, u8 b_, u8 a_) {
        return [=](int, u32, u32) { return std::array<u8, 4>{r_, g_, b_, a_}; };
    };
    write_dds(dir / "plate_normals.dds", 1, flat(0, 128, 0, 128));
    write_dds(dir / "albedo_white.dds", 1, flat(255, 255, 255, 255));
    // Clear on the texture's left half, opaque on its right.
    write_dds(
        dir / "albedo_half.dds", 1,
        [](int, u32 column, u32) {
            return std::array<u8, 4>{255, 255, 255, static_cast<u8>(column < 8 ? 0 : 255)};
        },
        16, 4);
    write_dds(dir / "spec_none.dds", 1, flat(0, 0, 0, 0));
    ctx.vfs.mount(kRoot, std::make_unique<vfs::DirectoryMount>(dir));

    std::vector<std::string> prop_bps;
    {
        const std::vector<std::string> world = sim::world_blueprints(ctx.sim);
        for (const auto* e : ctx.sim.blueprint_store()->get_all(blueprints::BlueprintType::Prop))
            if (prop_bps.size() < 2 && e->id.find("/env/") != std::string::npos &&
                std::find(world.begin(), world.end(), e->id) == world.end())
                prop_bps.push_back(e->id);
    }
    size_t next_unit = 0;
    size_t next_prop = 0;
    const auto stand = [&](const char* shader, const char* albedo, const char* mesh, f32 x, f32 z,
                           f32 lift, bool prop = false) {
        Plate p;
        p.shader = shader;
        p.albedo = albedo;
        p.specteam = "spec_none.dds";
        p.mesh = mesh;
        if (prop) {
            if (next_prop >= prop_bps.size()) {
                spdlog::warn("out of prop blueprints");
                return;
            }
            stand_plate(ctx, kRoot, prop_bps[next_prop++], p, x, z, ground_y, true, lift);
        } else {
            stand_plate(ctx, kRoot, kPlateBlueprints[next_unit++], p, x, z, ground_y, false, lift);
        }
    };
    const auto env = [] {
        map::ScmapEnvironment e;
        e.terrain_shader = "TTerrain";
        e.cubemaps.emplace_back("<default>", "/textures/environment/no_such_cube.dds");
        return e;
    };

    // The sun low (1 up for 2 across), to one side of the camera's view: a
    // plate 2.5 up casts its shadow 5 across, clear of it.
    r.camera().set_pitch(1.1f);
    ground.set_lighting(white_fill(), env());
    shots.recapture();
    (void)shots.shoot_frame(ground, 400.0f, 400.0f, 30.0f);
    f32 ex = 0, ey = 0, ez = 0;
    r.camera().eye_position(ex, ey, ez);
    f32 dx = r.camera().target_x() - ex;
    f32 dz = r.camera().target_z() - ez;
    const f32 dl = std::sqrt(dx * dx + dz * dz);
    dx /= dl;
    dz /= dl;
    map::ScmapLighting sun = white_fill();
    for (f32& c : sun.sun_color) c = 1.0f;
    for (f32& c : sun.shadow_fill) c = 0.0f;
    const f32 across = 0.8944f;
    const f32 up = 0.4472f;
    sun.sun_direction[0] = -dz * across;
    sun.sun_direction[1] = up;
    sun.sun_direction[2] = dx * across;
    // Where the shadow of something `lift` over (x, z) falls under `l`: away
    // from its sun.
    const auto shadow_at = [&](f32 x, f32 z, f32 lift, const map::ScmapLighting& l) {
        return std::array<f32, 2>{x - l.sun_direction[0] / l.sun_direction[1] * lift,
                                  z - l.sun_direction[2] / l.sun_direction[1] * lift};
    };
    const auto dark = [&](f32 x, f32 z) {
        ground.set_lighting(sun, env());
        shots.recapture();
        int n = 0;
        for (const auto& px : shots.shoot(ground, x, z, 30.0f))
            if (px[0] < 0.08f && px[1] < 0.08f && px[2] < 0.08f) ++n;
        return n;
    };

    // Test 1: half-clear hovering plates. Drawn by Unit (FA's Depth), the
    // whole plate casts; by NormalMappedAlpha, VertexNormal and
    // UndulatingNormalMappedAlpha (DepthClip, UndulatingDepthClip), and as a
    // prop of a technique the engine hasn't ported, half.
    {
        struct Hover {
            const char* shader;
            bool prop;
        };
        const Hover hovers[] = {{"Unit", false},
                                {"NormalMappedAlpha", false},
                                {"VertexNormal", false},
                                {"UndulatingNormalMappedAlpha", false},
                                {"Clutter", true}};
        f32 x = 100.0f;
        for (const Hover& h : hovers) {
            stand("Unit", "albedo_white.dds", "plate_wide.scm", x, 100.0f, 0.0f);
            stand(h.shader, "albedo_half.dds", "plate_hover.scm", x, 100.0f, kLift, h.prop);
            x += 20.0f;
        }
        for (int i = 0; i < 30; ++i) ctx.sim.tick();
        std::vector<int> counts;
        x = 100.0f;
        for (size_t i = 0; i < std::size(hovers); ++i) {
            const auto at = shadow_at(x, 100.0f, kLift, sun);
            counts.push_back(dark(at[0], at[1]));
            x += 20.0f;
        }
        const int full = counts[0];
        bool ok = full > 1500;
        std::string seen = fmt::format(" Unit {};", full);
        for (size_t i = 1; i < counts.size(); ++i) {
            const f32 share = full > 0 ? static_cast<f32>(counts[i]) / static_cast<f32>(full) : 0;
            ok = ok && share > 0.3f && share < 0.7f;
            seen += fmt::format(" {} {} ({:.2f});", hovers[i].shader, counts[i], share);
        }
        t.check(ok, fmt::format("Test 1: shadowed pixels under half-clear plates:{}", seen));
    }

    // Test 2: UndulatingDepthClip sways the shadow as the tree sways. A wall,
    // 100 tall in its mesh and 3 in the world, hangs 8 up with the sun low
    // behind it: its shadow falls on a plate some 19 toward the camera, the
    // wall itself out of the middle of the frame. Over half a sway (0.3 at the top,
    // whatever the wall's scale) the shadow moves; a NormalMappedAlpha
    // wall's doesn't.
    {
        map::ScmapLighting behind = sun;
        behind.sun_direction[0] = dx * across;
        behind.sun_direction[1] = up;
        behind.sun_direction[2] = dz * across;
        constexpr f32 kHang = 8.0f;
        const auto hang = [&](const char* shader, f32 x) {
            stand(shader, "albedo_white.dds", "wall.scm", x, 200.0f, kHang);
            const auto small = ctx.lua_state.do_string("__osc_last_plate:SetScale(0.03)\n");
            if (!small) spdlog::warn("SetScale: {}", small.error().message);
            const auto at = shadow_at(x, 200.0f, kHang + 1.5f, behind);
            stand("Unit", "albedo_white.dds", "plate_wide.scm", at[0], at[1], 0.0f);
            return at;
        };
        const auto swaying = hang("UndulatingNormalMappedAlpha", 300.0f);
        const auto rigid = hang("NormalMappedAlpha", 340.0f);
        for (int i = 0; i < 30; ++i) ctx.sim.tick();
        const auto phase = [&] {
            const f32 time = static_cast<f32>(ctx.sim.tick_count()) + 1.0f;
            return std::sin(0.05f * time - 0.707f * (300.0f + 200.0f));
        };
        for (int i = 0; i < 70 && std::abs(phase()) > 0.05f; ++i) ctx.sim.tick();
        const auto mask = [&](const std::array<f32, 2>& at) {
            ground.set_lighting(behind, env());
            shots.recapture();
            std::vector<bool> out;
            for (const auto& px : shots.shoot(ground, at[0], at[1], 30.0f))
                out.push_back(px[0] < 0.08f && px[1] < 0.08f && px[2] < 0.08f);
            return out;
        };
        const auto calm = mask(swaying);
        const auto still_calm = mask(rigid);
        for (int i = 0; i < 31; ++i) ctx.sim.tick();
        const auto gust = mask(swaying);
        const auto still_gust = mask(rigid);
        const auto moved = [](const std::vector<bool>& a, const std::vector<bool>& b) {
            int n = 0;
            for (size_t k = 0; k < a.size() && k < b.size(); ++k) n += a[k] != b[k] ? 1 : 0;
            return n;
        };
        int shadowed = 0;
        for (const bool d : calm) shadowed += d ? 1 : 0;
        const int swayed = moved(calm, gust);
        const int stayed = moved(still_calm, still_gust);
        t.check(shadowed > 300 && swayed > 30 && stayed == 0,
                fmt::format("Test 2: an Undulating wall's shadow ({} pixels) changes {} pixels "
                            "over half a sway, a NormalMappedAlpha wall's {}",
                            shadowed, swayed, stayed));
    }

    spdlog::info("Clipped shadow test: {}/{} passed", t.pass, t.pass + t.fail);
}

} // namespace osc::test
