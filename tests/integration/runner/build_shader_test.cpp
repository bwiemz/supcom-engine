// --build-shader-test (M211f): units under construction draw with FA's
// build shaders.
//
// Retail's Unit.StartBeingBuiltEffects puts a unit under construction into
// its build mesh (Blueprints.lua's ExtractBuildMeshBlueprint: ShaderName
// '<Faction>Build', SecondaryName '<Faction>BuildSpecular.dds'), and
// StopBeingBuiltEffects puts its own back. Test 1 builds real units. The
// rest stand plates of the test's own off its ground, over the sky's clear
// colour, and compare a frame's middle pixel with FA's formulas (mesh.fx),
// reduced by the test's light: a white fill, no sun colour and a black cube,
// SpecTeam red and green 0. No environment, no highlight, and a lit albedo
// shows as itself.

#include "integration_tests.hpp"
#include "plate_fixtures.hpp"
#include "render_probe.hpp"

#include "blueprints/blueprint_store.hpp"
#include "lua/lua_state.hpp"
#include "map/heightmap.hpp"
#include "map/terrain.hpp"
#include "renderer/camera.hpp"
#include "renderer/mesh_cache.hpp"
#include "sim/army_brain.hpp"
#include "sim/entity.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "vfs/directory_mount.hpp"
#include "vfs/virtual_file_system.hpp"

#include <lua.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace osc::test {

namespace {

constexpr u32 kSize = 64;
constexpr const char* kRoot = "/osc_build_shader_test";
constexpr f32 kPitch = 1.1f;

/// The string at `__blueprints[bp].Display[field]`, empty without one.
std::string display_field(TestContext& ctx, const std::string& bp, const char* field) {
    const auto* entry = ctx.store.find(bp);
    if (!entry) return {};
    ctx.store.push_lua_table(*entry, ctx.L);
    std::string out;
    if (lua_istable(ctx.L, -1)) {
        lua_pushstring(ctx.L, "Display");
        lua_rawget(ctx.L, -2);
        if (lua_istable(ctx.L, -1)) {
            lua_pushstring(ctx.L, field);
            lua_rawget(ctx.L, -2);
            if (lua_type(ctx.L, -1) == LUA_TSTRING) out = lua_tostring(ctx.L, -1);
            lua_pop(ctx.L, 1);
        }
        lua_pop(ctx.L, 1);
    }
    lua_pop(ctx.L, 1);
    return out;
}

} // namespace

void test_build_shaders(TestContext& ctx) {
    spdlog::info("=== BUILD SHADER TEST: FA's build shaders (M211f) ===");
    Tally t;

    OffscreenShots shots(ctx);
    if (!shots.ok()) {
        t.check(false, "renderer init (no Vulkan?)");
        return;
    }
    renderer::Renderer& r = shots.renderer();
    using renderer::MeshTechnique;

    // The test's ground: flat, at a structure's height, in the map's corner.
    const auto probe = ctx.lua_state.do_string(
        "__osc_build_shader_probe = CreateUnitHPR('ueb0101', 'ARMY_1', 32, 0, 32, 0, 0, 0)\n");
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

    // The plates' files.
    const auto dir = std::filesystem::temp_directory_path() / "osc_build_shader_test";
    std::filesystem::create_directories(dir);
    write_plate_scm(dir / "plate.scm", 4.0f);
    write_plate_scm(dir / "plate_hover.scm", 2.0f);
    const auto flat = [](u8 r_, u8 g_, u8 b_, u8 a_) {
        return [=](int, u32, u32) { return std::array<u8, 4>{r_, g_, b_, a_}; };
    };
    write_dds(dir / "plate_normals.dds", 1, flat(0, 128, 0, 128));
    write_dds(dir / "albedo_white.dds", 1, flat(255, 255, 255, 255));
    write_dds(dir / "albedo_black.dds", 1, flat(0, 0, 0, 255));
    // White on the texture's left half, black on its right.
    write_dds(
        dir / "albedo_stripes.dds", 1,
        [](int, u32 column, u32) {
            const u8 v = column < 8 ? 255 : 0;
            return std::array<u8, 4>{v, v, v, 255};
        },
        16, 4);
    write_dds(dir / "spec_none.dds", 1, flat(0, 0, 0, 0));
    write_dds(dir / "spec_team.dds", 1, flat(0, 0, 0, 255));
    write_dds(dir / "sec_black.dds", 1, flat(0, 0, 0, 0));
    write_dds(dir / "sec_red.dds", 1, flat(51, 0, 0, 64));
    write_dds(dir / "sec_cybran.dds", 1, flat(102, 0, 0, 128));
    write_dds(dir / "sec_sheen.dds", 1, flat(153, 51, 204, 102));
    write_dds(dir / "sec_distort.dds", 1, flat(255, 0, 0, 0));
    write_dds(dir / "sec_checker.dds", 1, [](int, u32 column, u32 row) {
        const u8 v = (column + row) % 2 ? 255 : 0;
        return std::array<u8, 4>{v, v, v, v};
    });
    write_dds(
        dir / "falloff_lookup.dds", 1,
        [](int, u32 column, u32 row) {
            return std::array<u8, 4>{static_cast<u8>(column * 17), static_cast<u8>(row * 12), 0,
                                     static_cast<u8>(column >= 8 ? 255 : 0)};
        },
        16, 20);
    ctx.vfs.mount(kRoot, std::make_unique<vfs::DirectoryMount>(dir));

    size_t next_bp = 0;
    struct Build {
        const char* shader = "UEFBuild";
        const char* albedo = "albedo_black.dds";
        const char* specteam = "spec_none.dds";
        const char* secondary = "sec_black.dds";
        const char* lookup = "";
        const char* mesh = "plate.scm";
    };
    // A plate `lift` over the ground at (x, z), `f` built: standing in for
    // the next of the blueprints, or `bp`.
    const auto stand = [&](const Build& b, f32 f, f32 x, f32 z, f32 lift = 0.0f,
                           const char* bp = nullptr) {
        if (!bp && next_bp >= std::size(kPlateBlueprints)) {
            spdlog::warn("out of plate blueprints");
            return;
        }
        Plate p;
        p.shader = b.shader;
        p.albedo = b.albedo;
        p.specteam = b.specteam;
        p.secondary = b.secondary;
        p.lookup = b.lookup;
        p.mesh = b.mesh;
        stand_plate(ctx, kRoot, bp ? bp : kPlateBlueprints[next_bp++], p, x, z, ground_y, false,
                    lift);
        const auto set =
            ctx.lua_state.do_string(fmt::format("__osc_last_plate:SetFractionComplete({})\n", f));
        if (!set) spdlog::warn("SetFractionComplete: {}", set.error().message);
        ctx.sim.tick();
    };

    auto* army = ctx.sim.get_army(0);
    // The test's light: the sun's colour black (its direction, for Aeon's
    // overlay, below the ground unless given), a white fill.
    const auto light = [](f32 sx = 0.0f, f32 sy = -1.0f, f32 sz = 0.0f) {
        map::ScmapLighting l = white_fill();
        l.sun_direction[0] = sx;
        l.sun_direction[1] = sy;
        l.sun_direction[2] = sz;
        return l;
    };
    const auto frame = [&](f32 x, f32 z, const map::ScmapLighting& lighting) {
        map::ScmapEnvironment env;
        env.terrain_shader = "TTerrain";
        env.cubemaps.emplace_back("<default>", "/textures/environment/no_such_cube.dds");
        ground.set_lighting(lighting, std::move(env));
        shots.recapture();
        return shots.shoot_frame(ground, x, z, 30.0f);
    };
    const auto at = [&](f32 x, f32 z) { return middle(frame(x, z, light())); };
    // Until the tick is `phase` in FA's 50-tick UEF pulse (the shader's time
    // is the tick plus the frame's interpolant, 1 here).
    const auto to_phase = [&](u32 phase) {
        while (ctx.sim.tick_count() % 50 != phase) ctx.sim.tick();
    };
    // FA's view direction at the frame's middle: toward the eye.
    const auto view_direction = [&] {
        const renderer::Camera& cam = r.camera();
        f32 ex = 0, ey = 0, ez = 0;
        cam.eye_position(ex, ey, ez);
        std::array<f32, 3> v = {ex - cam.target_x(), ey - cam.target_y(), ez - cam.target_z()};
        const f32 n = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        for (f32& c : v) c /= n;
        return v;
    };

    r.camera().set_pitch(kPitch);
    const Rgb sky = at(400.0f, 400.0f);
    const Rgb white = {1, 1, 1};
    const Rgb black = {0, 0, 0};
    const Rgb blue = {0, 0, 1};

    // Test 1: construction puts a unit into its faction's build mesh, and
    // finishing puts it back. An engineer of each faction builds its T1 power
    // generator, near ARMY_1's start. (The renderer's meshes, which say how
    // each is drawn, are there once it has drawn a frame: the sky's.)
    {
        struct Job {
            const char* engineer;
            const char* structure;
            MeshTechnique building;
        };
        const Job jobs[] = {{"uel0105", "ueb1101", MeshTechnique::UEFBuild},
                            {"ual0105", "uab1101", MeshTechnique::AeonBuild},
                            {"url0105", "urb1101", MeshTechnique::CybranBuild},
                            {"xsl0105", "xsb1101", MeshTechnique::SeraphimBuild}};
        const auto started = ctx.lua_state.do_string(
            "local brain = GetArmyBrain('ARMY_1')\n"
            "brain:GiveResource('MASS', 5000)\n"
            "brain:GiveResource('ENERGY', 50000)\n"
            "local sx, sz = brain:GetArmyStartPos()\n"
            "__osc_build_engineers = {}\n"
            "local jobs = { {'uel0105', 'ueb1101'}, {'ual0105', 'uab1101'},\n"
            "               {'url0105', 'urb1101'}, {'xsl0105', 'xsb1101'} }\n"
            "for i, job in jobs do\n"
            "    local x = sx - 25 + 10 * i\n"
            "    local e = CreateUnitHPR(job[1], 'ARMY_1', x, 0, sz + 12, 0, 0, 0)\n"
            "    IssueBuildMobile({e}, Vector(x, 0, sz + 18), job[2], {})\n"
            "    table.insert(__osc_build_engineers, e)\n"
            "end\n");
        if (!started) spdlog::warn("the engineers: {}", started.error().message);
        // What each structure wears, and how it's drawn.
        struct Seen {
            std::string mesh;
            MeshTechnique technique = MeshTechnique::Unit;
            f32 fraction = -1.0f;
        };
        const auto look = [&](const char* structure) {
            Seen seen;
            ctx.sim.entity_registry().for_each([&](const sim::Entity& e) {
                if (!e.is_unit() || e.destroyed() || e.blueprint_id() != structure) return;
                seen.mesh = e.mesh_override();
                seen.fraction = e.fraction_complete();
                seen.technique =
                    r.mesh_technique(seen.mesh.empty() ? e.blueprint_id() : seen.mesh, ctx.L);
            });
            return seen;
        };
        // Until every structure is under way.
        const auto under_way = [&] {
            for (const Job& job : jobs) {
                const Seen seen = look(job.structure);
                if (seen.fraction <= 0.0f || seen.fraction >= 1.0f) return false;
            }
            return true;
        };
        for (int i = 0; i < 300 && !under_way(); ++i) ctx.sim.tick();
        for (int i = 0; i < 5; ++i) ctx.sim.tick();
        std::string during;
        bool building_ok = true;
        for (const Job& job : jobs) {
            const Seen seen = look(job.structure);
            const std::string wanted = display_field(ctx, job.structure, "BuildMeshBlueprint");
            const bool ok = !wanted.empty() && seen.mesh == wanted &&
                            seen.technique == job.building && seen.fraction > 0.0f &&
                            seen.fraction < 1.0f;
            building_ok = building_ok && ok;
            during +=
                fmt::format(" {} at {:.2f} wears '{}' ({}), technique {};", job.structure,
                            seen.fraction, seen.mesh, wanted, static_cast<u32>(seen.technique));
        }
        // Finish them. (Aeon structures keep the build mesh two seconds
        // longer: retail's StructureUnit.StopBeingBuiltEffects.)
        const auto hurried = ctx.lua_state.do_string(
            "for _, e in __osc_build_engineers do e:SetBuildRate(1000) end\n");
        if (!hurried) spdlog::warn("the build rate: {}", hurried.error().message);
        for (int i = 0; i < 60; ++i) ctx.sim.tick();
        std::string after;
        bool built_ok = true;
        for (const Job& job : jobs) {
            const Seen seen = look(job.structure);
            const std::string own = display_field(ctx, job.structure, "MeshBlueprint");
            const bool ok = seen.fraction == 1.0f && (seen.mesh.empty() || seen.mesh == own) &&
                            seen.technique != job.building;
            built_ok = built_ok && ok;
            after += fmt::format(" {} at {:.2f} wears '{}', technique {};", job.structure,
                                 seen.fraction, seen.mesh, static_cast<u32>(seen.technique));
        }
        t.check(building_ok && built_ok,
                fmt::format("Test 1: under construction:{} built:{}", during, after));
        const auto cleared = ctx.lua_state.do_string(
            "for _, e in __osc_build_engineers do e:Destroy() end\n"
            "local built = {ueb1101 = true, uab1101 = true, urb1101 = true, xsb1101 = true}\n"
            "for _, u in GetArmyBrain('ARMY_1'):GetListOfUnits(categories.ALLUNITS, false) do\n"
            "    if built[u:GetUnitId()] then u:Destroy() end\n"
            "end\n");
        if (!cleared) spdlog::warn("clearing up: {}", cleared.error().message);
        ctx.sim.tick();
    }

    // Test 2: UEFBuildHiFiPS: the colour lerped to blue by t = clamp(frac(0.02
    // time), 0.35, 0.7), then back to itself by the fraction f, at alpha
    // max(f, 0.5); under the overlay, whose colour is the secondary's (black)
    // and whose alpha is at least 0.25. The albedo is black: (0, 0, t) (1 -
    // f).
    {
        stand(Build{}, 0.0f, 100.0f, 100.0f);
        stand(Build{}, 0.8f, 120.0f, 100.0f);
        const auto expected = [&](f32 f, f32 pulse) {
            return over(over(sky, scaled(blue, pulse * (1.0f - f)), std::max(f, 0.5f)), black,
                        0.25f);
        };
        to_phase(5); // time 6: t 0.35
        const Rgb low = at(100.0f, 100.0f);
        const Rgb low_80 = at(120.0f, 100.0f);
        to_phase(40); // time 41: t 0.7
        const Rgb high = at(100.0f, 100.0f);
        const Rgb high_80 = at(120.0f, 100.0f);
        t.check(near(low, expected(0.0f, 0.35f)) && near(high, expected(0.0f, 0.7f)) &&
                    near(low_80, expected(0.8f, 0.35f)) && near(high_80, expected(0.8f, 0.7f)),
                fmt::format("Test 2: UEF plates show {} and {} at t 0.35 ({} and {} expected), {} "
                            "and {} at 0.7 ({} and {})",
                            show(low), show(low_80), show(expected(0.0f, 0.35f)),
                            show(expected(0.8f, 0.35f)), show(high), show(high_80),
                            show(expected(0.0f, 0.7f)), show(expected(0.8f, 0.7f))));
    }

    // Test 3: UEFBuildOverlayHiFiPS: the overlay adds two reads of the
    // secondary, (0.2, 0, 0) each, at alpha max((a + a) (1 - f), 0.25) for
    // its alpha a (0.25), faded out over the last 5%. Its base pass adds the
    // secondary before the pulse.
    {
        Build red;
        red.secondary = "sec_red.dds";
        stand(red, 0.0f, 140.0f, 100.0f);
        stand(red, 0.97f, 160.0f, 100.0f);
        const f32 a = 64.0f / 255.0f;
        const Rgb secondary = {51.0f / 255.0f, 0, 0};
        const auto expected = [&](f32 f) {
            const Rgb current = over(secondary, blue, 0.35f);
            const f32 fade = f >= 0.95f ? 1.0f - (f - 0.95f) * 20.0f : 1.0f;
            return over(over(sky, scaled(current, 1.0f - f), std::max(f, 0.5f)),
                        scaled(secondary, 2.0f), std::max(2.0f * a * (1.0f - f), 0.25f) * fade);
        };
        to_phase(5);
        const Rgb started = at(140.0f, 100.0f);
        const Rgb nearly = at(160.0f, 100.0f);
        t.check(near(started, expected(0.0f)) && near(nearly, expected(0.97f)),
                fmt::format("Test 3: UEF plates under a red overlay show {} at 0% ({} expected), "
                            "{} at 97% ({})",
                            show(started), show(expected(0.0f)), show(nearly),
                            show(expected(0.97f))));
    }

    // Test 4: AeonBuild. The mesh grows from 75% (max(f, 0.75)); its base
    // pass is opaque, the albedo lit (white); its overlay's colour is sheen *
    // sat(N . S) + pow(sat(reflect . V), 8), its alpha twice its red. With
    // the sun below, the overlay is the highlight alone, pow(V.y, 8) (the
    // reflection is straight up). With the sun low in the west, the sheen:
    // the secondary's reads (uniform here) make it, and bend the normal.
    {
        Build aeon;
        aeon.shader = "AeonBuild";
        aeon.albedo = "albedo_white.dds";
        stand(aeon, 0.0f, 180.0f, 100.0f);
        stand(aeon, 1.0f, 200.0f, 100.0f);
        Build sheen = aeon;
        sheen.secondary = "sec_sheen.dds";
        stand(sheen, 0.5f, 220.0f, 100.0f);
        const int small = plate_width(frame(180.0f, 100.0f, light()), sky);
        const auto v = view_direction();
        const Rgb started = at(180.0f, 100.0f);
        const int full = plate_width(frame(200.0f, 100.0f, light()), sky);
        const Rgb built = at(200.0f, 100.0f);
        const Rgb lit = middle(frame(220.0f, 100.0f, light(-1.0f, 0.0f, 0.0f)));
        const f32 highlight = std::pow(v[1], 8.0f);
        const Rgb expected_started =
            over(white, Rgb{highlight, highlight, highlight}, 2.0f * highlight);
        // The sheen, as AeonBuildOverlayPS makes it from uniform reads: s the
        // secondary, n the normal map's (0.5, 0.5) (its .gaa).
        const std::array<f32, 4> s = {153.0f / 255.0f, 51.0f / 255.0f, 204.0f / 255.0f,
                                      102.0f / 255.0f};
        const f32 d = s[0] - s[1] + s[1] * s[0];
        const f32 diffuse = d * 0.25f + 0.5f * 0.75f;
        const f32 n_map = 128.0f / 255.0f;
        std::array<f32, 3> n = {n_map, n_map, n_map};
        const std::array<f32, 3> baa = {s[2], s[3], s[3]};
        for (int c = 0; c < 3; ++c) {
            n[c] = (n[c] + baa[c]) * 0.5f;
            n[c] = (n[c] + diffuse) * 0.5f;
            n[c] = 2.0f * n[c] - 1.0f;
        }
        n[2] = std::sqrt(std::abs(1.0f - n[0] * n[0] - n[1] * n[1]));
        // Into the world: x along the binormal (+z), y along the tangent
        // (+x), z along the normal (+y).
        std::array<f32, 3> world = {n[1], n[2], n[0]};
        const f32 len = std::sqrt(world[0] * world[0] + world[1] * world[1] + world[2] * world[2]);
        for (f32& c : world) c /= len;
        const std::array<f32, 3> sun = {-1.0f, 0.0f, 0.0f};
        const f32 dln = std::clamp(-world[0], 0.0f, 1.0f);
        std::array<f32, 3> reflection{};
        for (int c = 0; c < 3; ++c) reflection[c] = 2.0f * dln * world[c] - sun[c];
        const f32 rlen = std::sqrt(reflection[0] * reflection[0] + reflection[1] * reflection[1] +
                                   reflection[2] * reflection[2]);
        const f32 rv = (reflection[0] * v[0] + reflection[1] * v[1] + reflection[2] * v[2]) / rlen;
        const f32 glint = std::pow(std::clamp(rv, 0.0f, 1.0f), 8.0f);
        const f32 sheen_colour = diffuse * dln + glint;
        const Rgb expected_lit = over(white, Rgb{sheen_colour, sheen_colour, sheen_colour},
                                      std::clamp(2.0f * sheen_colour, 0.0f, 1.0f));
        const f32 ratio = full > 0 ? static_cast<f32>(small) / static_cast<f32>(full) : 0.0f;
        t.check(ratio > 0.72f && ratio < 0.78f && near(started, expected_started) &&
                    near(built, white) && near(lit, expected_lit),
                fmt::format("Test 4: an Aeon plate spans {} pixels at 0%, {} built ({:.3f}, 0.75 "
                            "expected); shows {} at 0% ({}), {} built (white), {} under a low sun "
                            "through a sheen ({})",
                            small, full, ratio, show(started), show(expected_started), show(built),
                            show(lit), show(expected_lit)));
    }

    // Test 5: CybranBuildPS: the albedo lit (white), at alpha 0.4 until 70%
    // built, then rising to 1; CybranBuildOverlayPS: (0.75 s.a, 0, 0) at
    // alpha s.r * s.a for the secondary s (0.4, 0, 0, 0.5), faded out over
    // the last 5%.
    {
        Build cybran;
        cybran.shader = "CybranBuild";
        cybran.albedo = "albedo_white.dds";
        cybran.secondary = "sec_cybran.dds";
        stand(cybran, 0.5f, 240.0f, 100.0f);
        stand(cybran, 0.85f, 260.0f, 100.0f);
        stand(cybran, 0.97f, 280.0f, 100.0f);
        const f32 sr = 102.0f / 255.0f;
        const f32 sa = 128.0f / 255.0f;
        const auto expected = [&](f32 f) {
            const f32 alpha = f >= 0.7f ? 0.4f + 0.6f * ((f - 0.7f) * 3.33f) : 0.4f;
            const f32 fade = f >= 0.95f ? 1.0f - (f - 0.95f) * 20.0f : 1.0f;
            return over(over(sky, white, alpha), Rgb{0.75f * sa, 0, 0}, sr * sa * fade);
        };
        const Rgb half = at(240.0f, 100.0f);
        const Rgb most = at(260.0f, 100.0f);
        const Rgb nearly = at(280.0f, 100.0f);
        t.check(near(half, expected(0.5f)) && near(most, expected(0.85f)) &&
                    near(nearly, expected(0.97f)),
                fmt::format("Test 5: Cybran plates show {} at 50% ({} expected), {} at 85% ({}), "
                            "{} at 97% ({})",
                            show(half), show(expected(0.5f)), show(most), show(expected(0.85f)),
                            show(nearly), show(expected(0.97f))));
    }

    // Test 6: SeraphimBuild. The mesh grows from a quarter (0.25 + 0.75 f);
    // the colour, UnitFalloffPS's (here the falloff lookup's texel, by N . V
    // across and the army's row down, for a black albedo), at alpha max(f,
    // 0.25); the secondary's red, times 0.03 (1 - (f - 0.9) 10), shifts the
    // albedo's reads along u: at 50%, a striped albedo's edge moves from the
    // plate's middle by 0.15 of its width.
    {
        Build seraphim;
        seraphim.shader = "SeraphimBuild";
        seraphim.lookup = "falloff_lookup.dds";
        stand(seraphim, 0.0f, 100.0f, 130.0f);
        stand(seraphim, 0.6f, 120.0f, 130.0f);
        Build whole = seraphim;
        whole.albedo = "albedo_white.dds";
        stand(whole, 0.0f, 140.0f, 130.0f);
        stand(whole, 1.0f, 160.0f, 130.0f);
        Build stripes = seraphim;
        stripes.albedo = "albedo_stripes.dds";
        stand(stripes, 0.5f, 180.0f, 130.0f);
        stripes.secondary = "sec_distort.dds";
        stand(stripes, 0.5f, 200.0f, 130.0f);
        if (army) army->set_color(0x13, 0x1C, 0xD3); // ArmyColors[3]: row 5 of 20
        const auto v = view_direction();
        const int column = std::min(15, static_cast<int>(16.0f * std::pow(1.0f - v[1], 0.6f)));
        const Rgb texel = {static_cast<f32>(column * 17) / 255.0f, 60.0f / 255.0f, 0.0f};
        const Rgb started = at(100.0f, 130.0f);
        const Rgb more = at(120.0f, 130.0f);
        const int small = plate_width(frame(140.0f, 130.0f, light()), sky);
        const int full = plate_width(frame(160.0f, 130.0f, light()), sky);
        // Along the middle row, the stripes' edge nearest the plate's
        // middle, from its middle, as a fraction of its width.
        const auto edge = [&](f32 x) {
            const ImageRGBA8 image = frame(x, 130.0f, light());
            const auto [left, right] = plate_span(image, sky);
            const u32 y = image.height / 2;
            const auto bright = [&](int x0) {
                return image.pixels[(static_cast<size_t>(y) * image.width +
                                     static_cast<size_t>(x0)) *
                                    4] > 150;
            };
            const f32 mid = static_cast<f32>(left + right) * 0.5f;
            const f32 width = static_cast<f32>(right - left + 1);
            f32 best = 1.0f;
            for (int x0 = left + 3; x0 + 1 <= right - 3; ++x0)
                if (bright(x0) != bright(x0 + 1)) {
                    const f32 at_edge = (static_cast<f32>(x0) + 0.5f - mid) / width;
                    if (std::abs(at_edge) < std::abs(best)) best = at_edge;
                }
            return best;
        };
        const f32 straight = edge(180.0f);
        const f32 shifted = edge(200.0f);
        const f32 moved = std::abs(shifted - straight);
        const f32 ratio = full > 0 ? static_cast<f32>(small) / static_cast<f32>(full) : 0.0f;
        t.check(near(started, over(sky, texel, 0.25f)) && near(more, over(sky, texel, 0.6f)) &&
                    ratio > 0.22f && ratio < 0.28f && std::abs(straight) < 0.04f && moved > 0.11f &&
                    moved < 0.19f,
                fmt::format("Test 6: Seraphim plates show {} at 0% ({} expected: the lookup's "
                            "column {}), {} at 60% ({}); span {} pixels at 0%, {} built ({:.3f}, "
                            "0.25 expected); a striped albedo's edge sits {:+.3f} of the plate "
                            "from its middle, distorted {:+.3f}",
                            show(started), show(over(sky, texel, 0.25f)), column, show(more),
                            show(over(sky, texel, 0.6f)), small, full, ratio, straight, shifted));
    }

    // Test 7: the team's colour, from 90% built: lerped in by the SpecTeam's
    // mask, times (f - 0.9) 10. With a black albedo under a full mask: none
    // at 85%, 0.6 of it at 96%.
    {
        const Rgb team = {220.0f / 255.0f, 30.0f / 255.0f, 30.0f / 255.0f};
        if (army) army->set_color(220, 30, 30);
        const char* shaders[] = {"UEFBuild", "AeonBuild", "CybranBuild"};
        f32 x = 220.0f;
        for (const char* shader : shaders) {
            Build b;
            b.shader = shader;
            b.specteam = "spec_team.dds";
            stand(b, 0.85f, x, 130.0f);
            stand(b, 0.96f, x + 20.0f, 130.0f);
            x += 40.0f;
        }
        to_phase(5);
        const auto v = view_direction();
        const f32 highlight = std::pow(v[1], 8.0f);
        const auto expected = [&](const char* shader, f32 f) {
            const Rgb albedo = scaled(team, f >= 0.9f ? (f - 0.9f) * 10.0f : 0.0f);
            const f32 fade = f >= 0.95f ? 1.0f - (f - 0.95f) * 20.0f : 1.0f;
            if (std::string(shader) == "UEFBuild") {
                const Rgb current = over(albedo, blue, 0.35f);
                return over(over(sky, over(current, albedo, f), f), black, 0.25f * fade);
            }
            if (std::string(shader) == "AeonBuild")
                return over(albedo, Rgb{highlight, highlight, highlight}, 2.0f * highlight * fade);
            const f32 alpha = 0.4f + 0.6f * ((f - 0.7f) * 3.33f);
            return over(sky, albedo, alpha);
        };
        bool ok = true;
        std::string seen;
        x = 220.0f;
        for (const char* shader : shaders) {
            for (const f32 f : {0.85f, 0.96f}) {
                const Rgb px = at(f < 0.9f ? x : x + 20.0f, 130.0f);
                ok = ok && near(px, expected(shader, f));
                seen += fmt::format(" {} {:.0f}%: {} ({});", shader, f * 100.0f, show(px),
                                    show(expected(shader, f)));
            }
            x += 40.0f;
        }
        t.check(ok, fmt::format("Test 7: the team's colour:{}", seen));
    }

    // Test 8: the overlays and the distortion move with the instance's age
    // (time - the tick it was made): between frames 100 ticks apart (the
    // pulse's phase the same), each technique's plate changes where a
    // checkered secondary crosses it; a finished unit's doesn't. The sun is
    // low in the west, for Aeon's sheen to light. And by age, not the clock:
    // a Cybran plate looked at as it's made, and another made in its place
    // 15 ticks later, look alike. (Its mask scrolls diagonally, a checker
    // 0.008 of a texture a tick: 32 ticks would bring the checker back.)
    {
        Build b;
        b.albedo = "albedo_stripes.dds";
        b.secondary = "sec_checker.dds";
        b.specteam = "sec_checker.dds";
        const char* shaders[] = {"UEFBuild", "AeonBuild", "CybranBuild", "SeraphimBuild", "Unit"};
        f32 x = 100.0f;
        for (const char* shader : shaders) {
            b.shader = shader;
            stand(b, shader == std::string("Unit") ? 1.0f : 0.5f, x, 160.0f);
            x += 20.0f;
        }
        const auto view = [&](f32 at_x) {
            map::ScmapEnvironment env;
            env.terrain_shader = "TTerrain";
            env.cubemaps.emplace_back("<default>", "/textures/environment/no_such_cube.dds");
            ground.set_lighting(light(-1.0f, 0.0f, 0.0f), std::move(env));
            shots.recapture();
            return shots.shoot(ground, at_x, 160.0f, 30.0f);
        };
        const auto differ = [](const Pixels& a, const Pixels& b_) {
            int n = 0;
            for (size_t k = 0; k < a.size() && k < b_.size(); ++k)
                for (int c = 0; c < 3; ++c)
                    if (std::abs(a[k][c] - b_[k][c]) > 12.0f / 255.0f) {
                        ++n;
                        break;
                    }
            return n;
        };
        b.shader = "CybranBuild";
        const char* same = kPlateBlueprints[next_bp++]; // a blueprint sets a unit's height
        stand(b, 0.5f, 200.0f, 160.0f, 0.0f, same);
        const Pixels first = view(200.0f);
        // Out of the way (destroyed, a unit could leave a wreck).
        const auto gone = ctx.lua_state.do_string(
            fmt::format("Warp(__osc_last_plate, Vector(600, {}, 600))\n", ground_y));
        if (!gone) spdlog::warn("Warp: {}", gone.error().message);
        for (int i = 0; i < 15; ++i) ctx.sim.tick();
        stand(b, 0.5f, 200.0f, 160.0f, 0.0f, same);
        const int apart = differ(first, view(200.0f));
        to_phase(1); // time 2: t 0.35
        std::vector<Pixels> before;
        before.reserve(5);
        for (int i = 0; i < 5; ++i) before.push_back(view(100.0f + 20.0f * static_cast<f32>(i)));
        for (int i = 0; i < 100; ++i) ctx.sim.tick();
        std::string seen;
        bool ok = true;
        for (int i = 0; i < 5; ++i) {
            const int changed = differ(view(100.0f + 20.0f * static_cast<f32>(i)), before[i]);
            ok = ok && (i < 4 ? changed > 50 : changed == 0);
            seen += fmt::format(" {} {};", shaders[i], changed);
        }
        t.check(ok && apart < 50, // a pixel's rounding apart, where 18,000 differ by the clock
                fmt::format("Test 8: pixels changed over 100 ticks:{} two Cybran plates made 17 "
                            "ticks apart, each looked at as it's made, differ on {}",
                            seen, apart));
    }

    // Test 9: shadows. A small plate hovers 2.5 over a unit's plate, the sun
    // high to one side (and no fill): its shadow falls beside it. UEFBuild
    // casts as the unit (FA's Depth technique); AeonBuild casts none (its
    // technique has no depth stage); SeraphimBuild's shadow is its mesh's,
    // scaled 0.25 + 0.75 f (SeraphimBuildDepth).
    {
        // The sun's side: square to the camera's view, so that the shadow
        // isn't behind the hovering plate.
        (void)frame(100.0f, 190.0f, light());
        const renderer::Camera& cam = r.camera();
        f32 ex = 0, ey = 0, ez = 0;
        cam.eye_position(ex, ey, ez);
        f32 dx = cam.target_x() - ex;
        f32 dz = cam.target_z() - ez;
        const f32 dl = std::sqrt(dx * dx + dz * dz);
        dx /= dl;
        dz /= dl;
        map::ScmapLighting sun = light(-dz * 0.7071f, 0.7071f, dx * 0.7071f);
        for (f32& c : sun.shadow_fill) c = 0.0f;
        for (f32& c : sun.sun_color) c = 1.0f;
        struct Hover {
            const char* shader;
            f32 f;
        };
        const Hover hovers[] = {{"Unit", 1.0f},
                                {"UEFBuild", 0.5f},
                                {"AeonBuild", 0.5f},
                                {"SeraphimBuild", 0.0f},
                                {"SeraphimBuild", 1.0f}};
        f32 x = 100.0f;
        for (const Hover& h : hovers) {
            Build under;
            under.shader = "Unit";
            under.albedo = "albedo_white.dds";
            stand(under, 1.0f, x, 190.0f);
            Build hover;
            hover.shader = h.shader;
            hover.albedo = "albedo_white.dds";
            hover.lookup = "falloff_lookup.dds"; // Seraphim's colour, not black
            hover.mesh = "plate_hover.scm";
            stand(hover, h.f, x, 190.0f, 2.5f);
            x += 20.0f;
        }
        std::vector<size_t> dark;
        x = 100.0f;
        for (size_t i = 0; i < std::size(hovers); ++i) {
            map::ScmapEnvironment env;
            env.terrain_shader = "TTerrain";
            env.cubemaps.emplace_back("<default>", "/textures/environment/no_such_cube.dds");
            ground.set_lighting(sun, std::move(env));
            shots.recapture();
            size_t n = 0;
            for (const auto& px : shots.shoot(ground, x, 190.0f, 30.0f))
                if (px[0] < 0.08f && px[1] < 0.08f && px[2] < 0.08f) ++n;
            dark.push_back(n);
            x += 20.0f;
        }
        t.check(dark[0] > 300 && dark[1] > dark[0] * 3 / 4 && dark[2] < 20 &&
                    dark[4] > dark[0] * 3 / 4 && dark[3] < dark[4] / 4,
                fmt::format("Test 9: shadowed pixels under a hovering plate: a unit's {}, "
                            "UEFBuild's {}, AeonBuild's {}, SeraphimBuild's {} at 0% and {} "
                            "built",
                            dark[0], dark[1], dark[2], dark[3], dark[4]));
    }

    spdlog::info("Build shader test: {}/{} passed", t.pass, t.pass + t.fail);
}

} // namespace osc::test
