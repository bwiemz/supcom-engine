// --water-reflection-test (M213b): units reflected in the water, and the
// meshes Moho draws after it.
//
// Moho draws the units mirrored in the water's plane into a target of their
// own (RenderReflections), clipped at the surface, at half alpha, which the
// surface lerps over its sky reflection by the map's unitReflection. Only a
// unit's mesh instance is reflected: every other entity's clears the flag.
// Techniques mesh.fx gives the POSTWATER stage are drawn after the water,
// which writes no depth, so they show over it. Walls of the test's own stand
// in for the units and props, on SCMP_009's water.

#include "integration_tests.hpp"
#include "intel_probe.hpp"
#include "plate_fixtures.hpp"
#include "render_probe.hpp"

#include "blueprints/blueprint_store.hpp"
#include "core/image.hpp"
#include "lua/lua_state.hpp"
#include "map/terrain.hpp"
#include "renderer/renderer.hpp"
#include "renderer/water_renderer.hpp"
#include "sim/sim_state.hpp"
#include "sim/world_snapshot.hpp"
#include "vfs/directory_mount.hpp"
#include "vfs/virtual_file_system.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace osc::test {

namespace {

constexpr const char* kRoot = "/osc_water_reflection_test";
/// The walls: 8 wide, 6 tall, facing along z. The camera looks along -z.
constexpr f32 kWallHalf = 4.0f;
constexpr f32 kWallHeight = 6.0f;

/// The pixel of `img` at world point `p`, 0-1 a channel.
std::array<f32, 3> pixel(renderer::Renderer& r, const ImageRGBA8& img, const sim::Vector3& p) {
    const auto s = screen_of(r, p);
    if (!s || img.width == 0) return {0, 0, 0};
    const u32 x = static_cast<u32>(std::clamp((*s)[0], 0.0f, static_cast<f32>(img.width - 1)));
    const u32 y = static_cast<u32>(std::clamp((*s)[1], 0.0f, static_cast<f32>(img.height - 1)));
    const u8* q = &img.pixels[(static_cast<size_t>(y) * img.width + x) * 4];
    return {q[0] / 255.0f, q[1] / 255.0f, q[2] / 255.0f};
}

f32 apart(const std::array<f32, 3>& a, const std::array<f32, 3>& b) {
    return std::abs(a[0] - b[0]) + std::abs(a[1] - b[1]) + std::abs(a[2] - b[2]);
}

} // namespace

void test_water_reflection(TestContext& ctx) {
    spdlog::info("=== WATER REFLECTION TEST: units in the water, meshes after it (M213b) ===");
    Tally t;
    map::Terrain& terrain = *ctx.sim.terrain();
    if (!terrain.has_water()) {
        t.check(false, "SCMP_009 has water");
        return;
    }
    const f32 w = terrain.water_elevation();
    const f32 abyss = terrain.water_abyss_elevation();
    // Copies: each look below replaces the terrain's own.
    const map::ScmapWater original = terrain.water();
    const map::ScmapWaterMasks masks = terrain.water_masks();

    OffscreenShots shots(ctx);
    if (!shots.ok()) {
        t.check(false, "renderer init (no Vulkan?)");
        return;
    }
    renderer::Renderer& r = shots.renderer();

    const auto dir = std::filesystem::temp_directory_path() / "osc_water_reflection_test";
    std::filesystem::create_directories(dir);
    write_wall_scm(dir / "wall.scm", kWallHalf, kWallHeight);
    // One whose normal faces the camera (+z).
    write_wall_scm(dir / "wall_facing.scm", kWallHalf, kWallHeight, 1.0f);
    const auto flat = [](u8 r_, u8 g_, u8 b_, u8 a_) {
        return [=](int, u32, u32) { return std::array<u8, 4>{r_, g_, b_, a_}; };
    };
    write_dds(dir / "plate_normals.dds", 1, flat(0, 128, 0, 128));
    write_dds(dir / "albedo_white.dds", 1, flat(255, 255, 255, 255));
    write_dds(dir / "spec_none.dds", 1, flat(0, 0, 0, 0));
    // All environment reflection (SpecTeam's red), and an environment white
    // above and black below.
    write_dds(dir / "spec_env.dds", 1, flat(255, 0, 0, 0));
    write_dds(dir / "cube.dds", 6, [](int face, u32, u32) {
        const u8 v = face == 2 ? 255 : face == 3 ? 0 : 128; // +Y, -Y, the sides
        return std::array<u8, 4>{v, v, v, 255};
    });
    ctx.vfs.mount(kRoot, std::make_unique<vfs::DirectoryMount>(dir));

    // Prop blueprints the map has none of, which the walls stand in for.
    std::vector<std::string> prop_bps;
    {
        const std::vector<std::string> world = sim::world_blueprints(ctx.sim);
        for (const auto* e : ctx.sim.blueprint_store()->get_all(blueprints::BlueprintType::Prop))
            if (prop_bps.size() < 8 && e->id.find("/env/") != std::string::npos &&
                std::find(world.begin(), world.end(), e->id) == world.end())
                prop_bps.push_back(e->id);
    }
    size_t next_prop = 0;
    size_t next_unit = 0;
    // A white wall with `shader` at (x, z), from half a unit over `ground_y`
    // up: a prop, or a unit of ARMY_1's. Lua's __osc_last_plate holds it.
    const auto stand = [&](const char* shader, f32 x, f32 z, f32 ground_y, bool prop,
                           const char* specteam = "spec_none.dds", const char* mesh = "wall.scm") {
        Plate p;
        p.shader = shader;
        p.albedo = "albedo_white.dds";
        p.specteam = specteam;
        p.mesh = mesh;
        const bool out = prop ? next_prop >= prop_bps.size() : next_unit >= kPlateBlueprints.size();
        if (out) {
            spdlog::warn("out of blueprints for the walls");
            return false;
        }
        const std::string bp = prop ? prop_bps[next_prop++] : kPlateBlueprints[next_unit++];
        stand_plate(ctx, kRoot, bp, p, x, z, ground_y, prop);
        return true;
    };
    const auto knock_down = [&] {
        const auto gone = ctx.lua_state.do_string("__osc_last_plate:Destroy()\n");
        if (!gone) spdlog::warn("Destroy: {}", gone.error().message);
        ctx.sim.tick();
    };
    // The whole frame looking at (x, z) with the water's parameters `water`,
    // at the sim's current tick (so the same waves every time).
    const auto look = [&](const map::ScmapWater& water, f32 x, f32 z) {
        terrain.set_water(water, masks, abyss);
        shots.recapture();
        return shots.shoot_frame(terrain, x, z, 50.0f);
    };

    // Places in open water by depth (the water map's G: 17 a unit here), away
    // from the map's edge, their neighbours alike: the middle of a texel.
    (void)shots.shoot(terrain, 32.0f, 32.0f, 70.0f);
    const renderer::WaterRenderer& water_map = r.water_renderer();
    const auto find = [&](u8 lo, u8 hi) -> std::optional<std::array<f32, 2>> {
        const std::vector<u8>& map = water_map.water_map();
        const u32 mw = water_map.water_map_width();
        for (u32 hz = 16; hz + 16 < water_map.water_map_height(); ++hz)
            for (u32 hx = 16; hx + 16 < mw; ++hx) {
                bool ok = true;
                for (u32 z = hz - 2; z <= hz + 2 && ok; ++z)
                    for (u32 x = hx - 2; x <= hx + 2 && ok; ++x) {
                        const size_t i = (static_cast<size_t>(z) * mw + x) * 4;
                        ok = map[i + 2] == 0 && map[i + 1] >= lo && map[i + 1] <= hi;
                    }
                if (ok)
                    return std::array<f32, 2>{static_cast<f32>(hx * 2 + 1),
                                              static_cast<f32>(hz * 2 + 1)};
            }
        return std::nullopt;
    };
    const auto shallow = find(35, 80);
    const auto deep = find(210, 255);
    if (!shallow || !deep || prop_bps.size() < 3) {
        t.check(false, fmt::format("places and props: shallow {}, deep {}, {} prop blueprints",
                                   shallow.has_value(), deep.has_value(), prop_bps.size()));
        return;
    }

    // Test 1: the meshes Moho draws after the water. A VertexNormal wall
    // (POSTWATER) standing in shallow water keeps its pixels under the
    // surface when the water turns red; a NormalMappedAlpha one (PREWATER),
    // seen through the surface, turns red.
    {
        map::ScmapWater red = original;
        red.surface_color[0] = 1.0f;
        red.surface_color[1] = red.surface_color[2] = 0.0f;
        red.color_lerp[0] = red.color_lerp[1] = 1.0f;
        const auto [x, z] = *shallow;
        const f32 ground = terrain.get_terrain_height(x, z);
        // Under the surface, between the wall's foot and the water.
        const sim::Vector3 under{x, (ground + 0.5f + w) / 2.0f, z};
        const auto change = [&](const char* shader) {
            if (!stand(shader, x, z, ground, true)) return std::array<f32, 3>{};
            const auto a = pixel(r, look(original, x, z), under);
            const auto b = pixel(r, look(red, x, z), under);
            knock_down();
            return std::array<f32, 3>{b[0] - a[0], b[1] - a[1], b[2] - a[2]};
        };
        const auto after = change("VertexNormal");
        const auto before = change("NormalMappedAlpha");
        const f32 after_moved = std::abs(after[0]) + std::abs(after[1]) + std::abs(after[2]);
        t.check(after_moved < 0.01f && before[0] > 0.2f,
                fmt::format("Test 1: turning the water red moves a post-water wall's submerged "
                            "pixels {:.3f}, reddens a pre-water one's {:+.2f} ({:.1f} under)",
                            after_moved, before[0], w - ground));
    }

    // The reflection, boosted: the unit's reflection over all the sky's
    // (unitReflection 4 at alpha 0.5), and the sky's over all the water.
    map::ScmapWater boosted = original;
    boosted.unit_reflection = 4.0f;
    boosted.sky_reflection = 10.0f;
    map::ScmapWater unreflected = boosted;
    unreflected.unit_reflection = 0.0f;
    const auto [dx, dz] = *deep;
    // With a wall standing from `ground_y`, how the water's parameters `a`
    // and `b` differ at the water that shows world point `seen` (below the
    // surface: the mirror of what's above) and at the same place 16 to the
    // side.
    struct Reflected {
        f32 there = 0;
        f32 aside = 0;
        f32 shown = 0; ///< the water's brightness there under `a`, 0-1
        f32 face = 0;  ///< the wall's own, its middle, under `a`
    };
    const auto brightness = [](const std::array<f32, 3>& c) { return (c[0] + c[1] + c[2]) / 3.0f; };
    const auto reflected = [&](bool prop, f32 ground_y, f32 seen_y, const map::ScmapWater& a,
                               const map::ScmapWater& b, const char* specteam = "spec_none.dds",
                               const char* mesh = "wall.scm") {
        Reflected out;
        if (!stand("Unit", dx, dz, ground_y, prop, specteam, mesh)) return out;
        // For a look at it by eye: the map's own reflection, not boosted.
        if (const char* dump = std::getenv("OSC_REFLECTION_TEST_PNG"); dump && !prop)
            write_png(fmt::format("{}_{:.0f}.png", dump, ground_y), look(original, dx, dz));
        const ImageRGBA8 with = look(a, dx, dz);
        const ImageRGBA8 without = look(b, dx, dz);
        knock_down();
        out.there = apart(pixel(r, with, {dx, seen_y, dz}), pixel(r, without, {dx, seen_y, dz}));
        out.aside = apart(pixel(r, with, {dx + 16.0f, seen_y, dz}),
                          pixel(r, without, {dx + 16.0f, seen_y, dz}));
        out.shown = brightness(pixel(r, with, {dx, seen_y, dz}));
        out.face = brightness(pixel(r, with, {dx, ground_y + 0.5f + kWallHeight / 2.0f, dz}));
        return out;
    };
    // A wall standing on the surface, from w up; its mirror, from w down.
    const f32 afloat = w - 0.5f;
    const f32 mirror_middle = w - kWallHeight / 2.0f;

    // Test 2: a unit on the water is reflected, mirrored: the water changes
    // where its mirror image shows, below it, and not beside it.
    {
        const Reflected unit = reflected(false, afloat, mirror_middle, boosted, unreflected);
        t.check(unit.there > 0.1f && unit.aside < 1e-6f,
                fmt::format("Test 2: a unit's reflection changes the water at its mirror image "
                            "{:.3f}, beside it {:.3f}",
                            unit.there, unit.aside));
    }

    // Test 3: only units are reflected: the same wall as a prop isn't.
    {
        const Reflected prop = reflected(true, afloat, mirror_middle, boosted, unreflected);
        t.check(prop.there < 1e-6f,
                fmt::format("Test 3: a prop's reflection changes the water {:.3f}", prop.there));
    }

    // Test 4: nothing under the water is reflected: a unit sunk 2 under the
    // surface leaves the water where its mirror would show (w + 2 up) as it
    // was.
    {
        const f32 sunk = w - kWallHeight - 2.5f;
        const Reflected under =
            reflected(false, sunk, w + 2.0f + kWallHeight / 2.0f, boosted, unreflected);
        t.check(
            under.there < 1e-6f,
            fmt::format("Test 4: a sunk unit's reflection changes the water {:.3f}", under.there));
    }

    // Test 5: the reflection is drawn at half alpha: by unitReflection 1 it
    // lerps in by a half, by 2 wholly, and by 4 no more.
    {
        map::ScmapWater by1 = boosted;
        by1.unit_reflection = 1.0f;
        map::ScmapWater by2 = boosted;
        by2.unit_reflection = 2.0f;
        const Reflected half = reflected(false, afloat, mirror_middle, by1, by2);
        const Reflected whole = reflected(false, afloat, mirror_middle, by2, boosted);
        t.check(half.there > 0.05f && whole.there < 1e-6f,
                fmt::format("Test 5: the reflection by unitReflection 1 against 2 differs {:.3f}, "
                            "by 2 against 4 {:.3f}",
                            half.there, whole.there));
    }

    // The next two light the walls themselves. The camera looks along -z,
    // from +z.
    const map::ScmapLighting lighting = terrain.lighting();
    const map::ScmapEnvironment environment = terrain.environment();
    map::ScmapLighting dark = lighting;
    for (int i = 0; i < 3; ++i) dark.sun_color[i] = dark.sun_ambience[i] = dark.shadow_fill[i] = 0;
    dark.multiplier = 1.0f;

    // Test 6: the reflection is lit by the sun negated, with no shadow. A
    // low sun behind a wall facing the camera leaves its face dark; its
    // reflection, lit by the sun turned about, shows it lit, though another
    // wall behind it shades it from that sun.
    {
        map::ScmapLighting behind = dark;
        for (f32& c : behind.sun_color) c = 1.0f;
        const f32 n = std::sqrt(0.3f * 0.3f + 1.0f);
        behind.sun_direction[0] = 0.0f;
        behind.sun_direction[1] = 0.3f / n;
        behind.sun_direction[2] = -1.0f / n;
        terrain.set_lighting(behind, environment);
        // The wall that shades it, between it and the sun: a prop, which
        // isn't reflected.
        const bool shaded = stand("NormalMappedAlpha", dx, dz - 3.0f, afloat, true);
        const auto held = ctx.lua_state.do_string("__osc_shade = __osc_last_plate\n");
        if (!held) spdlog::warn("the shading wall: {}", held.error().message);
        const Reflected lit = reflected(false, afloat, mirror_middle, boosted, unreflected,
                                        "spec_none.dds", "wall_facing.scm");
        if (shaded) {
            const auto gone = ctx.lua_state.do_string("__osc_shade:Destroy()\n");
            if (!gone) spdlog::warn("Destroy: {}", gone.error().message);
            ctx.sim.tick();
        }
        terrain.set_lighting(lighting, environment);
        t.check(lit.shown > 0.5f && lit.face < 0.1f,
                fmt::format("Test 6: under a low sun behind it, a shaded wall's face shows {:.2f}, "
                            "its reflection {:.2f}",
                            lit.face, lit.shown));
    }

    // Test 7: the reflection is seen from the eye mirrored in the water. In
    // the dark, a wall that reflects all its environment (white above, black
    // below) shows the black from above; its reflection, seen from under
    // the water, the white.
    {
        map::ScmapEnvironment cube = environment;
        cube.cubemaps.clear();
        cube.cubemaps.emplace_back("<default>", fmt::format("{}/cube.dds", kRoot));
        terrain.set_lighting(dark, cube);
        const Reflected seen =
            reflected(false, afloat, mirror_middle, boosted, unreflected, "spec_env.dds");
        terrain.set_lighting(lighting, environment);
        t.check(seen.shown > 0.5f && seen.face < 0.1f,
                fmt::format("Test 7: reflecting a sky white above and black below, a wall shows "
                            "{:.2f}, its reflection {:.2f}",
                            seen.face, seen.shown));
    }

    terrain.set_water(original, masks, abyss);
    spdlog::info("Water reflection test: {}/{} passed", t.pass, t.pass + t.fail);
}

} // namespace osc::test
