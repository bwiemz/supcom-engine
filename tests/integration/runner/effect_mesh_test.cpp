// --effect-mesh-test (M211g): the build effects' meshes, Moho's shader
// names, and burnt trees.
//
// Retail's build effects are projectiles with meshes of their own: UEF's
// cube (UEFBuildCube) and slices (AlphaFade), Aeon's pool (AeonBuildPuddle).
// Moho resolves legacy ShaderNames (TMeshGlow is NormalMappedGlow) and gives
// every mesh but a unit's a white instance colour. Test 1 builds real units;
// the rest stand plates of the test's own off its ground, over the sky's
// clear colour, lit by a white fill (no sun colour, a black cube, SpecTeam
// red and green 0: a lit albedo shows as itself).

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
#include "sim/army_brain.hpp"
#include "sim/entity.hpp"
#include "sim/sim_state.hpp"
#include "sim/world_snapshot.hpp"
#include "vfs/directory_mount.hpp"
#include "vfs/virtual_file_system.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace osc::test {

namespace {

constexpr u32 kSize = 64;
constexpr const char* kRoot = "/osc_effect_mesh_test";
constexpr f32 kPitch = 1.1f;

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

} // namespace

void test_effect_meshes(TestContext& ctx) {
    spdlog::info("=== EFFECT MESH TEST: the build effects' meshes (M211g) ===");
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
        "__osc_effect_mesh_probe = CreateUnitHPR('ueb0101', 'ARMY_1', 32, 0, 32, 0, 0, 0)\n");
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
    const TempDir scratch("osc_effect_mesh_test");
    const auto& dir = scratch.path();
    write_plate_scm(dir / "plate.scm", 4.0f);
    const auto flat = [](u8 r_, u8 g_, u8 b_, u8 a_) {
        return [=](int, u32, u32) { return std::array<u8, 4>{r_, g_, b_, a_}; };
    };
    write_dds(dir / "plate_normals.dds", 1, flat(0, 128, 0, 128));
    write_dds(dir / "albedo_white.dds", 1, flat(255, 255, 255, 255));
    write_dds(dir / "albedo_cube.dds", 1, flat(100, 150, 200, 128));
    write_dds(dir / "albedo_slice.dds", 1, flat(200, 100, 50, 255));
    // White on the texture's left half, black on its right.
    write_dds(
        dir / "albedo_stripes.dds", 1,
        [](int, u32 column, u32) {
            const u8 v = column < 8 ? 255 : 0;
            return std::array<u8, 4>{v, v, v, 255};
        },
        16, 4);
    write_dds(dir / "spec_none.dds", 1, flat(0, 0, 0, 0));
    write_dds(dir / "sec_black.dds", 1, flat(0, 0, 0, 0));
    ctx.vfs.mount(kRoot, std::make_unique<vfs::DirectoryMount>(dir));

    // Prop blueprints the map has none of (so none are drawn but the
    // test's).
    std::vector<std::string> prop_bps;
    {
        const std::vector<std::string> world = sim::world_blueprints(ctx.sim);
        for (const auto* e : ctx.sim.blueprint_store()->get_all(blueprints::BlueprintType::Prop))
            if (prop_bps.size() < 2 && e->id.find("/env/") != std::string::npos &&
                std::find(world.begin(), world.end(), e->id) == world.end())
                prop_bps.push_back(e->id);
    }
    size_t next_prop = 0;

    size_t next_bp = 0;
    struct Look {
        const char* shader = "Unit";
        const char* albedo = "albedo_white.dds";
        const char* secondary = "sec_black.dds";
    };
    // A plate at (x, z), `f` built: a unit's, or a prop's.
    const auto stand = [&](const Look& look, f32 f, f32 x, f32 z, bool prop = false) {
        if (next_bp >= kPlateBlueprints.size()) {
            spdlog::warn("out of plate blueprints");
            return;
        }
        Plate p;
        p.shader = look.shader;
        p.albedo = look.albedo;
        p.specteam = "spec_none.dds";
        p.secondary = look.secondary;
        if (prop) {
            if (next_prop >= prop_bps.size()) {
                spdlog::warn("out of prop blueprints");
                return;
            }
            stand_plate(ctx, kRoot, prop_bps[next_prop++], p, x, z, ground_y, true);
        } else {
            stand_plate(ctx, kRoot, kPlateBlueprints[next_bp++], p, x, z, ground_y);
        }
        const auto set =
            ctx.lua_state.do_string(fmt::format("__osc_last_plate:SetFractionComplete({})\n", f));
        if (!set) spdlog::warn("SetFractionComplete: {}", set.error().message);
        ctx.sim.tick();
    };

    auto* army = ctx.sim.get_army(0);
    const auto frame = [&](f32 x, f32 z, const map::ScmapLighting& lighting = white_fill()) {
        map::ScmapEnvironment env;
        env.terrain_shader = "TTerrain";
        env.cubemaps.emplace_back("<default>", "/textures/environment/no_such_cube.dds");
        ground.set_lighting(lighting, std::move(env));
        shots.recapture();
        return shots.shoot_frame(ground, x, z, 30.0f);
    };
    const auto at = [&](f32 x, f32 z) { return middle(frame(x, z)); };

    r.camera().set_pitch(kPitch);
    const Rgb sky = at(400.0f, 400.0f);
    const Rgb white = {1, 1, 1};

    // Test 1: the build effects. A UEF engineer and an Aeon one build T1
    // power generators near ARMY_1's start: the UEF cube and its slices, and
    // the Aeon pool, are projectiles drawn by UEFBuildCube, AlphaFade and
    // AeonBuildPuddle, in white.
    {
        const auto started = ctx.lua_state.do_string(
            "local brain = GetArmyBrain('ARMY_1')\n"
            "brain:GiveResource('MASS', 5000)\n"
            "brain:GiveResource('ENERGY', 50000)\n"
            "local sx, sz = brain:GetArmyStartPos()\n"
            "local uef = CreateUnitHPR('uel0105', 'ARMY_1', sx - 10, 0, sz + 12, 0, 0, 0)\n"
            "IssueBuildMobile({uef}, Vector(sx - 10, 0, sz + 18), 'ueb1101', {})\n"
            "local aeon = CreateUnitHPR('ual0105', 'ARMY_1', sx + 10, 0, sz + 12, 0, 0, 0)\n"
            "IssueBuildMobile({aeon}, Vector(sx + 10, 0, sz + 18), 'uab1101', {})\n");
        if (!started) spdlog::warn("the engineers: {}", started.error().message);
        // Each effect's technique, and its colour in the frame's dump.
        struct Effect {
            const char* name;
            MeshTechnique technique;
            MeshTechnique seen = MeshTechnique::Unit;
            bool found = false;
        };
        Effect effects[] = {{"uefbuildeffect03", MeshTechnique::UEFBuildCube},
                            {"uefbuildeffect02", MeshTechnique::AlphaFade},
                            {"aeonbuildeffect01", MeshTechnique::AeonBuildPuddle}};
        f32 seen_x = 0.0f;
        f32 seen_z = 0.0f;
        int seen_n = 0;
        for (int i = 0; i < 300; ++i) {
            ctx.sim.tick();
            if (i % 10 != 9) continue;
            ctx.sim.entity_registry().for_each([&](const sim::Entity& e) {
                if (e.destroyed() || !e.is_projectile()) return;
                const std::string id = lower(e.blueprint_id());
                for (Effect& effect : effects)
                    if (id.find(effect.name) != std::string::npos) {
                        effect.found = true;
                        effect.seen = r.mesh_technique(e.blueprint_id(), ctx.L);
                        seen_x += e.position().x;
                        seen_z += e.position().z;
                        ++seen_n;
                    }
            });
            if (std::all_of(std::begin(effects), std::end(effects),
                            [](const Effect& e) { return e.found; }))
                break;
        }
        // The frame's instances, with the projectiles' colours: amid the
        // effects.
        const f32 n = static_cast<f32>(std::max(1, seen_n));
        (void)frame(seen_x / n, seen_z / n);
        std::ostringstream dump;
        r.dump_frame(dump);
        std::istringstream lines(dump.str());
        int effect_lines = 0;
        int white_lines = 0;
        for (std::string line; std::getline(lines, line);) {
            const std::string l = lower(line);
            if (l.find("buildeffect") == std::string::npos) continue;
            ++effect_lines;
            if (l.find("| 1.000 1.000 1.000") != std::string::npos) ++white_lines;
        }
        bool ok = effect_lines > 0 && white_lines == effect_lines;
        std::string seen;
        for (const Effect& effect : effects) {
            ok = ok && effect.found && effect.seen == effect.technique;
            seen +=
                fmt::format(" {} {} (technique {});", effect.name,
                            effect.found ? "made" : "never made", static_cast<u32>(effect.seen));
        }
        t.check(ok, fmt::format("Test 1: the build effects:{} {} of {} drawn in white", seen,
                                white_lines, effect_lines));
        const auto cleared = ctx.lua_state.do_string(
            "for _, u in GetArmyBrain('ARMY_1'):GetListOfUnits(categories.ALLUNITS, false) do\n"
            "    local id = u:GetUnitId()\n"
            "    if id == 'uel0105' or id == 'ual0105' or id == 'ueb1101' or id == 'uab1101' then\n"
            "        u:Destroy()\n"
            "    end\n"
            "end\n");
        if (!cleared) spdlog::warn("clearing up: {}", cleared.error().message);
        for (int i = 0; i < 30; ++i) ctx.sim.tick();
    }

    // Until the tick is `phase` in a period (the shader's time is the tick
    // plus the frame's interpolant, 1 here).
    const auto to_phase = [&](u32 period, u32 phase) {
        while (ctx.sim.tick_count() % period != phase) ctx.sim.tick();
    };

    // Test 2: UEFBuildCubePS: no light; built (f = 1), its albedo at alpha
    // max(f, 0.5) * albedo.a. At 0, it and the secondary (black) lerped to
    // blue by max(frac(0.05 time), 0.35), unclamped above (UEFBuild's pulse
    // stops at 0.7), at half its albedo's alpha.
    {
        Look cube;
        cube.shader = "UEFBuildCube";
        cube.albedo = "albedo_cube.dds";
        stand(cube, 1.0f, 100.0f, 100.0f);
        stand(cube, 0.0f, 120.0f, 100.0f);
        const Rgb albedo = {100.0f / 255.0f, 150.0f / 255.0f, 200.0f / 255.0f};
        const f32 a = 128.0f / 255.0f;
        to_phase(20, 17); // time 18: frac 0.9
        const Rgb built = at(100.0f, 100.0f);
        const Rgb pulsing = at(120.0f, 100.0f);
        to_phase(20, 2); // time 3: frac 0.15, held at 0.35
        const Rgb held = at(120.0f, 100.0f);
        const Rgb expected_built = over(sky, albedo, a);
        const auto expected_at = [&](f32 pulse) {
            return over(sky, over(albedo, Rgb{0, 0, 1}, pulse), 0.5f * a);
        };
        t.check(near(built, expected_built) && near(pulsing, expected_at(0.9f)) &&
                    near(held, expected_at(0.35f)),
                fmt::format("Test 2: UEF build cubes show {} built ({} expected), {} at 0% with "
                            "the pulse at 0.9 ({}), {} held at 0.35 ({})",
                            show(built), show(expected_built), show(pulsing),
                            show(expected_at(0.9f)), show(held), show(expected_at(0.35f))));
    }

    // Test 3: AlphaFadePS(2.0, 0.145): the albedo lit by the vertex's normal
    // (by the fill: itself), at alpha albedo.a * f * sat(1 - (age - 2) *
    // 0.145), tested over 0x23. Just made (age 1), its albedo; five ticks on,
    // at 0.42; ten on, gone. A tenth built, under the test.
    {
        Look slice;
        slice.shader = "AlphaFade";
        slice.albedo = "albedo_slice.dds";
        stand(slice, 1.0f, 140.0f, 100.0f);
        const Rgb albedo = {200.0f / 255.0f, 100.0f / 255.0f, 50.0f / 255.0f};
        const Rgb fresh = at(140.0f, 100.0f);
        for (int i = 0; i < 5; ++i) ctx.sim.tick();
        const Rgb fading = at(140.0f, 100.0f);
        for (int i = 0; i < 5; ++i) ctx.sim.tick();
        const Rgb gone = at(140.0f, 100.0f);
        stand(slice, 0.3f, 160.0f, 100.0f);
        const Rgb faint = at(160.0f, 100.0f);
        stand(slice, 0.1f, 180.0f, 100.0f);
        const Rgb tested = at(180.0f, 100.0f);
        const f32 at_six = 1.0f - (6.0f - 2.0f) * 0.145f;
        t.check(near(fresh, albedo) && near(fading, over(sky, albedo, at_six)) && near(gone, sky) &&
                    near(faint, over(sky, albedo, 0.3f)) && near(tested, sky),
                fmt::format("Test 3: an AlphaFade plate shows {} just made ({}), {} five ticks on "
                            "({}), {} ten on (the sky, {}); {} 30% built ({}), {} 10% (the sky)",
                            show(fresh), show(albedo), show(fading),
                            show(over(sky, albedo, at_six)), show(gone), show(sky), show(faint),
                            show(over(sky, albedo, 0.3f)), show(tested)));
    }

    // Test 4: NormalMappedGlow (TMeshGlow) and NormalMappedAlpha (TMeshAlpha)
    // are NormalMappedPS without the team's mask: a unit's albedo times its
    // army's colour, where Unit shows the albedo (its SpecTeam masks
    // nothing). NormalMappedAlpha is tested over 0x80 by f * albedo.a: a unit
    // 40% built isn't drawn.
    {
        const Rgb team = {220.0f / 255.0f, 30.0f / 255.0f, 30.0f / 255.0f};
        if (army) army->set_color(220, 30, 30);
        Look glow;
        glow.shader = "TMeshGlow";
        stand(glow, 1.0f, 100.0f, 130.0f);
        Look alpha;
        alpha.shader = "TMeshAlpha";
        stand(alpha, 1.0f, 120.0f, 130.0f);
        stand(alpha, 0.4f, 140.0f, 130.0f);
        Look unit;
        stand(unit, 1.0f, 160.0f, 130.0f);
        const Rgb glowing = at(100.0f, 130.0f);
        const Rgb tinted = at(120.0f, 130.0f);
        const Rgb cut = at(140.0f, 130.0f);
        const Rgb masked = at(160.0f, 130.0f);
        t.check(near(glowing, team) && near(tinted, team) && near(cut, sky) && near(masked, white),
                fmt::format("Test 4: TMeshGlow shows {}, TMeshAlpha {} (the army's {}), 40% built "
                            "{} (the sky); Unit {} (white)",
                            show(glowing), show(tinted), show(team), show(cut), show(masked)));
    }

    // Test 5: AeonBuildPuddlePS: every read scrolls by age * (-0.002,
    // 0.0042); lit as AeonBuildPS (by the fill: the albedo). A striped
    // albedo's edge moves 0.1 of the plate over 50 ticks; the same plate
    // drawn by Unit doesn't move.
    {
        Look pool;
        pool.shader = "AeonBuildPuddle";
        pool.albedo = "albedo_stripes.dds";
        stand(pool, 1.0f, 180.0f, 130.0f);
        Look still = pool;
        still.shader = "Unit";
        stand(still, 1.0f, 200.0f, 130.0f);
        // A structure made whole puts its mesh back as it's built (retail's
        // StopBeingBuiltEffects; Aeon's two seconds on), which makes a new
        // mesh instance, its age starting over: past that first.
        for (int i = 0; i < 30; ++i) ctx.sim.tick();
        // Along the middle row, the stripes' edge nearest the plate's middle,
        // from its middle, as a fraction of its width.
        const auto edge = [&](f32 x) {
            const ImageRGBA8 image = frame(x, 130.0f);
            const auto [left, right] = plate_span(image, sky);
            const u32 y = image.height / 2;
            const auto bright = [&](int x0) {
                return image.pixels[(static_cast<size_t>(y) * image.width +
                                     static_cast<size_t>(x0)) *
                                    4] > 128;
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
        const f32 pool_before = edge(180.0f);
        const f32 still_before = edge(200.0f);
        for (int i = 0; i < 50; ++i) ctx.sim.tick();
        const f32 pool_moved = std::abs(edge(180.0f) - pool_before);
        const f32 still_moved = std::abs(edge(200.0f) - still_before);
        t.check(pool_moved > 0.07f && pool_moved < 0.13f && still_moved < 0.01f,
                fmt::format("Test 5: over 50 ticks, an AeonBuildPuddle plate's stripes move "
                            "{:.3f} of the plate (0.1 expected); a Unit plate's {:.3f}",
                            pool_moved, still_moved));
    }

    // Test 6: burnt trees. BlackenedNormalMappedAlpha greys a prop's albedo
    // to dot(rgb, 0.1) (white: 0.3) and lights it as NormalMappedPS; it's
    // tested over 0x80 by f * albedo.a. It isn't the wreck path M211d sent
    // it down (black under a white fill).
    {
        Look burnt;
        burnt.shader = "BlackenedNormalMappedAlpha";
        stand(burnt, 1.0f, 220.0f, 130.0f, true);
        stand(burnt, 0.4f, 240.0f, 130.0f, true);
        const Rgb grey = at(220.0f, 130.0f);
        const Rgb cut = at(240.0f, 130.0f);
        const Rgb expected = {0.3f, 0.3f, 0.3f};
        t.check(near(grey, expected) && near(cut, sky),
                fmt::format("Test 6: a burnt tree shows {} ({} expected); 40% built, {} (the sky)",
                            show(grey), show(expected), show(cut)));
    }

    // Test 7: UEFBuildCubeLoFiPS (M211n), at Low: no pulse, its albedo and
    // the secondary lerped to blue by 0.65 whatever the time; built, its
    // albedo at alpha a, as at High.
    {
        renderer::Renderer::VideoOptions& video = r.video_options();
        const renderer::Renderer::VideoOptions as_was = video;
        video.graphics_fidelity = 0;
        Look cube;
        cube.shader = "UEFBuildCube";
        cube.albedo = "albedo_cube.dds";
        stand(cube, 1.0f, 100.0f, 160.0f);
        stand(cube, 0.0f, 120.0f, 160.0f);
        const Rgb albedo = {100.0f / 255.0f, 150.0f / 255.0f, 200.0f / 255.0f};
        const f32 a = 128.0f / 255.0f;
        to_phase(20, 17);
        const Rgb built = at(100.0f, 160.0f);
        const Rgb early = at(120.0f, 160.0f);
        to_phase(20, 2);
        const Rgb later = at(120.0f, 160.0f);
        video = as_was;
        const Rgb expected_built = over(sky, albedo, a);
        const Rgb expected = over(sky, over(albedo, Rgb{0, 0, 1}, 0.65f), 0.5f * a);
        t.check(near(built, expected_built) && near(early, expected) && near(later, expected),
                fmt::format("Test 7: at Low, UEF build cubes show {} built ({} expected), {} and "
                            "{} at 0% a pulse apart ({})",
                            show(built), show(expected_built), show(early), show(later),
                            show(expected)));
    }

    spdlog::info("Effect mesh test: {}/{} passed", t.pass, t.pass + t.fail);
}

} // namespace osc::test
