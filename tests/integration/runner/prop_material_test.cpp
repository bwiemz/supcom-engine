// --prop-material-test (M211i): props draw with FA's own techniques.
//
// VertexNormal (rocks, pipes: lit by the vertex's normal, blended, tested
// over 0x23), NormalMappedTerrain (clutter: unshadowed, no highlight) and
// UndulatingNormalMappedAlpha (trees, swaying in FA's wind). Plates of the
// test's own stand in for props off the test's ground, over the sky's clear
// colour.

#include "integration_tests.hpp"
#include "support/temp_path.hpp"
#include "plate_fixtures.hpp"
#include "render_probe.hpp"

#include "blueprints/blueprint_store.hpp"
#include "lua/lua_state.hpp"
#include "map/heightmap.hpp"
#include "map/terrain.hpp"
#include "renderer/camera.hpp"
#include "renderer/mesh_cache.hpp"
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

constexpr u32 kSize = 64;
constexpr const char* kRoot = "/osc_prop_material_test";

int differing(const Pixels& a, const Pixels& b) {
    int n = 0;
    for (size_t k = 0; k < a.size() && k < b.size(); ++k)
        for (int c = 0; c < 3; ++c)
            if (std::abs(a[k][c] - b[k][c]) > 12.0f / 255.0f) {
                ++n;
                break;
            }
    return n;
}

} // namespace

void test_prop_materials(TestContext& ctx) {
    spdlog::info("=== PROP MATERIAL TEST: the props' techniques (M211i) ===");
    Tally t;

    OffscreenShots shots(ctx);
    if (!shots.ok()) {
        t.check(false, "renderer init (no Vulkan?)");
        return;
    }
    renderer::Renderer& r = shots.renderer();
    // The test units' lifebars (Moho draws a friendly unit's when zoomed in)
    // would darken the pixels Test 3 counts as shadow: off, as
    // ui_RenderUnitBars turns them off.
    r.set_unit_bars(false);
    using renderer::MeshTechnique;

    // The test's ground: flat, at a structure's height, in the map's corner.
    const auto probe = ctx.lua_state.do_string(
        "__osc_prop_material_probe = CreateUnitHPR('ueb0101', 'ARMY_1', 32, 0, 32, 0, 0, 0)\n");
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

    const TempDir scratch("osc_prop_material_test");
    const auto& dir = scratch.path();
    write_plate_scm(dir / "plate.scm", 4.0f);
    write_plate_scm(dir / "plate_hover.scm", 2.0f);
    write_plate_scm(dir / "plate_wide.scm", 8.0f);
    // A wall 100 tall in its mesh, which the test scales to 5: FA's sway
    // goes by the mesh's height, in the world's units.
    write_wall_scm(dir / "wall.scm", 100.0f, 100.0f);
    const auto flat = [](u8 r_, u8 g_, u8 b_, u8 a_) {
        return [=](int, u32, u32) { return std::array<u8, 4>{r_, g_, b_, a_}; };
    };
    write_dds(dir / "plate_normals.dds", 1, flat(0, 128, 0, 128));
    // Lying along the plate (-binormal): no light from overhead.
    write_dds(dir / "normals_tilted.dds", 1, flat(0, 0, 0, 128));
    write_dds(dir / "albedo_white.dds", 1, flat(255, 255, 255, 255));
    write_dds(dir / "albedo_half.dds", 1, flat(200, 100, 50, 128));
    write_dds(dir / "albedo_faint.dds", 1, flat(200, 100, 50, 32));
    write_dds(dir / "albedo_grey.dds", 1, flat(128, 128, 128, 255));
    write_dds(
        dir / "albedo_checker.dds", 1,
        [](int, u32 column, u32 row) {
            const u8 v = (column + row) % 2 ? 255 : 0;
            return std::array<u8, 4>{v, v, v, 255};
        },
        16, 16);
    write_dds(dir / "spec_none.dds", 1, flat(0, 0, 0, 0));
    write_dds(dir / "spec_highlight.dds", 1, flat(0, 255, 0, 0));
    ctx.vfs.mount(kRoot, std::make_unique<vfs::DirectoryMount>(dir));

    // Prop blueprints the map has none of (none are drawn but the test's).
    std::vector<std::string> prop_bps;
    {
        const std::vector<std::string> world = sim::world_blueprints(ctx.sim);
        for (const auto* e : ctx.sim.blueprint_store()->get_all(blueprints::BlueprintType::Prop))
            if (prop_bps.size() < 16 && e->id.find("/env/") != std::string::npos &&
                std::find(world.begin(), world.end(), e->id) == world.end())
                prop_bps.push_back(e->id);
    }
    size_t next_prop = 0;
    size_t next_unit = 0;
    struct Look {
        const char* shader = "NormalMappedAlpha";
        const char* albedo = "albedo_white.dds";
        const char* normals = "plate_normals.dds";
        const char* specteam = "spec_none.dds";
        const char* mesh = "plate.scm";
    };
    // A prop plate at (x, z); or a unit's, `lift` over it.
    const auto stand = [&](const Look& look, f32 x, f32 z, bool prop = true, f32 lift = 0.0f) {
        Plate p;
        p.shader = look.shader;
        p.albedo = look.albedo;
        p.normals = look.normals;
        p.specteam = look.specteam;
        p.mesh = look.mesh;
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
    const auto env_with = [](const char* cube) {
        map::ScmapEnvironment env;
        env.terrain_shader = "TTerrain";
        env.cubemaps.emplace_back("<default>", cube);
        return env;
    };
    const auto frame = [&](f32 x, f32 z, const map::ScmapLighting& lighting) {
        ground.set_lighting(lighting, env_with("/textures/environment/no_such_cube.dds"));
        shots.recapture();
        return shots.shoot_frame(ground, x, z, 30.0f);
    };
    // White fill, the sun's colour black, the sun overhead (for highlights).
    map::ScmapLighting fill = white_fill();
    fill.sun_direction[0] = 0.0f;
    fill.sun_direction[1] = 1.0f;
    fill.sun_direction[2] = 0.0f;
    // The sun overhead, and no fill.
    map::ScmapLighting sun = fill;
    for (f32& c : sun.sun_color) c = 1.0f;
    for (f32& c : sun.shadow_fill) c = 0.0f;

    r.camera().set_pitch(1.1f);
    const Rgb sky = middle(frame(400.0f, 400.0f, fill));

    // Test 1: retail's props resolve to their techniques: a rock
    // (VertexNormal), Crystalline clutter (NormalMappedTerrain), an
    // Evergreen birch (UndulatingNormalMappedAlpha), and a lava steam vent
    // (TMeshNoNormals, which is VertexNormal).
    {
        struct Retail {
            const char* bp;
            MeshTechnique technique;
        };
        const Retail props[] = {
            {"/env/devtest/props/rock02_prop.bp", MeshTechnique::VertexNormal},
            {"/env/crystalline/props/clutter/crys_clutter01_prop.bp",
             MeshTechnique::NormalMappedTerrain},
            {"/env/evergreen/props/trees/brch01_prop.bp",
             MeshTechnique::UndulatingNormalMappedAlpha},
            {"/env/common/props/lavasteam01_prop.bp", MeshTechnique::VertexNormal},
        };
        bool ok = true;
        std::string seen;
        for (const Retail& p : props) {
            const MeshTechnique got = r.mesh_technique(p.bp, ctx.L);
            ok = ok && got == p.technique;
            seen += fmt::format(" {} {};", p.bp, static_cast<u32>(got));
        }
        t.check(ok, fmt::format("Test 1: retail props' techniques:{}", seen));
    }

    // Test 2: VertexNormalPS: blended by f * albedo.a, tested over 0x23. A
    // half-alpha albedo, lit by the fill (itself), at 0.5 over the sky; one
    // at 0x20, cut. Lit by the vertex's normal: under a sun overhead, a
    // normal map lying flat along the plate leaves a NormalMappedAlpha plate
    // unlit, and a VertexNormal one lit.
    {
        Look half;
        half.shader = "VertexNormal";
        half.albedo = "albedo_half.dds";
        stand(half, 100.0f, 100.0f);
        Look faint = half;
        faint.albedo = "albedo_faint.dds";
        stand(faint, 120.0f, 100.0f);
        Look tilted;
        tilted.shader = "VertexNormal";
        tilted.normals = "normals_tilted.dds";
        stand(tilted, 140.0f, 100.0f);
        Look mapped = tilted;
        mapped.shader = "NormalMappedAlpha";
        stand(mapped, 160.0f, 100.0f);
        const Rgb albedo = {200.0f / 255.0f, 100.0f / 255.0f, 50.0f / 255.0f};
        const Rgb blended = middle(frame(100.0f, 100.0f, fill));
        const Rgb cut = middle(frame(120.0f, 100.0f, fill));
        const Rgb vertex_lit = middle(frame(140.0f, 100.0f, sun));
        const Rgb map_lit = middle(frame(160.0f, 100.0f, sun));
        const Rgb expected = over(sky, albedo, 128.0f / 255.0f);
        t.check(
            near(blended, expected) && near(cut, sky) && vertex_lit[0] > 0.2f && map_lit[0] < 0.05f,
            fmt::format("Test 2: VertexNormal plates show {} at half alpha ({} expected), {} "
                        "at 0x20 (the sky); under a sun overhead with a flat-lying normal "
                        "map, {}, where NormalMappedAlpha shows {}",
                        show(blended), show(expected), show(cut), show(vertex_lit), show(map_lit)));
    }

    // Test 3: NormalMappedTerrainPS: the albedo lit (by the fill: itself),
    // without FA's highlight; NormalMappedAlpha with the same SpecTeam
    // (green: the highlight) and a sun overhead shows it. And unshadowed: a
    // small plate hovering over it under the sun leaves it lit.
    {
        Look terrain;
        terrain.shader = "NormalMappedTerrain";
        terrain.albedo = "albedo_grey.dds";
        terrain.specteam = "spec_highlight.dds";
        stand(terrain, 100.0f, 130.0f);
        Look shiny = terrain;
        shiny.shader = "NormalMappedAlpha";
        stand(shiny, 120.0f, 130.0f);
        const Rgb grey = {128.0f / 255.0f, 128.0f / 255.0f, 128.0f / 255.0f};
        const Rgb plain = middle(frame(100.0f, 130.0f, fill));
        const Rgb highlighted = middle(frame(120.0f, 130.0f, fill));

        // Shadows: a hovering plate beside the sun's path, as the camera
        // sees, over wide plates on the test's ground (Moho's light camera
        // fits the terrain in view, M210c), the shadow all on them.
        constexpr f32 kLowZ = 52.0f;
        Look lower;
        lower.shader = "NormalMappedTerrain";
        lower.mesh = "plate_wide.scm";
        stand(lower, 12.0f, kLowZ);
        Look lower_unit = lower;
        lower_unit.shader = "NormalMappedAlpha";
        stand(lower_unit, 52.0f, kLowZ);
        Look hover;
        hover.shader = "Unit";
        hover.mesh = "plate_hover.scm";
        stand(hover, 12.0f, kLowZ, false, 2.5f);
        stand(hover, 52.0f, kLowZ, false, 2.5f);
        for (int i = 0; i < 30; ++i) ctx.sim.tick();
        (void)frame(12.0f, kLowZ, sun);
        f32 ex = 0, ey = 0, ez = 0;
        r.camera().eye_position(ex, ey, ez);
        f32 dx = r.camera().target_x() - ex;
        f32 dz = r.camera().target_z() - ez;
        const f32 dl = std::sqrt(dx * dx + dz * dz);
        map::ScmapLighting side = sun;
        side.sun_direction[0] = -dz / dl * 0.7071f;
        side.sun_direction[1] = 0.7071f;
        side.sun_direction[2] = dx / dl * 0.7071f;
        const auto dark = [&](f32 x) {
            ground.set_lighting(side, env_with("/textures/environment/no_such_cube.dds"));
            shots.recapture();
            int n = 0;
            for (const auto& px : shots.shoot(ground, x, kLowZ, 30.0f))
                if (px[0] < 0.08f && px[1] < 0.08f && px[2] < 0.08f) ++n;
            return n;
        };
        const int terrain_dark = dark(12.0f);
        const int unit_dark = dark(52.0f);
        t.check(near(plain, grey) && highlighted[1] > plain[1] + 0.1f && terrain_dark < 20 &&
                    unit_dark > 300,
                fmt::format("Test 3: NormalMappedTerrain shows {} (the albedo, {}), where "
                            "NormalMappedAlpha highlights {}; under a hovering plate, {} of its "
                            "pixels are in shadow, {} of NormalMappedAlpha's",
                            show(plain), show(grey), show(highlighted), terrain_dark, unit_dark));
    }

    // Test 4: UndulatingNormalMappedVS: each vertex moves 0.003 y * sin²(0.05
    // time - wind . p) * wind, wind (0.707, 0, 0.707). A checkered wall, 100
    // tall in its mesh and 5 in the world, sways 0.3 of a unit at its top
    // between frames at sin² 0 and 1; a NormalMappedAlpha wall doesn't.
    {
        Look wall;
        wall.shader = "UndulatingNormalMappedAlpha";
        wall.albedo = "albedo_checker.dds";
        wall.mesh = "wall.scm";
        stand(wall, 100.0f, 190.0f);
        const auto scaled_down = ctx.lua_state.do_string("__osc_last_plate:SetScale(0.05)\n");
        if (!scaled_down) spdlog::warn("SetScale: {}", scaled_down.error().message);
        Look still = wall;
        still.shader = "NormalMappedAlpha";
        stand(still, 140.0f, 190.0f);
        const auto scaled_too = ctx.lua_state.do_string("__osc_last_plate:SetScale(0.05)\n");
        if (!scaled_too) spdlog::warn("SetScale: {}", scaled_too.error().message);
        ctx.sim.tick();
        // The sway's phase: the shader's time is the tick plus the frame's
        // interpolant (1 here); p is the wall's position.
        f32 wall_x = 100.0f;
        f32 wall_z = 190.0f;
        const auto phase = [&] {
            const f32 time = static_cast<f32>(ctx.sim.tick_count()) + 1.0f;
            return std::sin(0.05f * time - 0.707f * (wall_x + wall_z));
        };
        for (int i = 0; i < 70 && std::abs(phase()) > 0.05f; ++i) ctx.sim.tick();
        r.camera().set_pitch(0.6f);
        const auto view = [&](f32 x) {
            ground.set_lighting(fill, env_with("/textures/environment/no_such_cube.dds"));
            shots.recapture();
            return shots.shoot(ground, x, 190.0f, 30.0f);
        };
        const Pixels calm = view(100.0f);
        const Pixels still_calm = view(140.0f);
        for (int i = 0; i < 31; ++i) ctx.sim.tick();
        const Pixels gust = view(100.0f);
        const Pixels still_gust = view(140.0f);
        r.camera().set_pitch(1.1f);
        const int swayed = differing(calm, gust);
        const int stayed = differing(still_calm, still_gust);
        t.check(swayed > 100 && stayed == 0,
                fmt::format("Test 4: over half a sway, an Undulating wall changes {} pixels, a "
                            "NormalMappedAlpha one {} (sin at {:.2f})",
                            swayed, stayed, phase()));
    }

    spdlog::info("Prop material test: {}/{} passed", t.pass, t.pass + t.fail);
}

} // namespace osc::test
