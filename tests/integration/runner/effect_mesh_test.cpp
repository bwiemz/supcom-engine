// --effect-mesh-test (M211g): the build effects' meshes, Moho's shader
// names, burnt trees, and the explosions' effect techniques.
//
// Retail's build effects are projectiles with meshes of their own: UEF's
// cube (UEFBuildCube) and slices (AlphaFade), Aeon's pool (AeonBuildPuddle).
// So are the UEF nuke's dome (NukeUEF), the EMP warhead's cloud
// (OuterCloud) and sphere (NukeEMP), the Aeon nuke's head (NukeQuantum), the
// teleport's shockwave (Effect) and a plume (Cloud).
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
#include "renderer/unit_renderer.hpp"
#include "sim/army_brain.hpp"
#include "sim/entity.hpp"
#include "sim/script_class.hpp"
#include "sim/sim_state.hpp"
#include "sim/world_snapshot.hpp"
#include "vfs/directory_mount.hpp"
#include "vfs/virtual_file_system.hpp"

#include <lua.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
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
    {
        std::ifstream in(dir / "plate.scm", std::ios::binary);
        const std::vector<char> both((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
        u32 index_offset = 0;
        u32 index_count = 0;
        std::memcpy(&index_offset, both.data() + 28, 4);
        std::memcpy(&index_count, both.data() + 32, 4);
        for (const u32 side : {0u, 1u}) {
            std::vector<char> one(both.begin(), both.begin() + index_offset);
            for (u32 i = 0; i < index_count; ++i) {
                if ((i / 6) % 2 == side) {
                    const auto at = both.begin() + index_offset + 2 * i;
                    one.insert(one.end(), at, at + 2);
                }
            }
            one.insert(one.end(), both.begin() + index_offset + 2 * index_count, both.end());
            const u32 count = index_count / 2;
            std::memcpy(one.data() + 32, &count, 4);
            std::ofstream(dir / (side == 0 ? "plate_side0.scm" : "plate_side1.scm"),
                          std::ios::binary)
                .write(one.data(), static_cast<std::streamsize>(one.size()));
        }
    }
    const auto flat = [](u8 r_, u8 g_, u8 b_, u8 a_) {
        return [=](int, u32, u32) { return std::array<u8, 4>{r_, g_, b_, a_}; };
    };
    write_dds(dir / "plate_normals.dds", 1, flat(0, 128, 0, 128));
    write_dds(dir / "albedo_white.dds", 1, flat(255, 255, 255, 255));
    write_dds(dir / "albedo_cube.dds", 1, flat(100, 150, 200, 128));
    write_dds(dir / "albedo_slice.dds", 1, flat(200, 100, 50, 255));
    write_dds(dir / "albedo_faint.dds", 1, flat(200, 100, 50, 64));
    write_dds(dir / "albedo_green.dds", 1, flat(40, 200, 60, 255));
    write_dds(dir / "nuke_noise.dds", 1, flat(51, 204, 102, 255));
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
    write_dds(dir / "mask_full.dds", 1, flat(0, 255, 0, 255));
    write_dds(dir / "albedo_dim.dds", 1, flat(200, 100, 50, 30));
    write_dds(
        dir / "albedo_bands.dds", 1,
        [](int, u32, u32 row) {
            const u8 v = (row / 4) % 2 == 0 ? 255 : 0;
            return std::array<u8, 4>{v, v, v, 255};
        },
        4, 16);
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
        const char* normals = "plate_normals.dds";
        const char* mesh = "plate.scm";
    };
    // A plate at (x, z), `f` built, `lift` higher: a unit's, or a prop's.
    const auto stand = [&](const Look& look, f32 f, f32 x, f32 z, bool prop = false,
                           f32 lift = 0.0f) {
        if (next_bp >= kPlateBlueprints.size()) {
            spdlog::warn("out of plate blueprints");
            return;
        }
        Plate p;
        p.shader = look.shader;
        p.albedo = look.albedo;
        p.specteam = "spec_none.dds";
        p.secondary = look.secondary;
        p.normals = look.normals;
        p.mesh = look.mesh;
        if (prop) {
            if (next_prop >= prop_bps.size()) {
                spdlog::warn("out of prop blueprints");
                return;
            }
            stand_plate(ctx, kRoot, prop_bps[next_prop++], p, x, z, ground_y, true);
        } else {
            stand_plate(ctx, kRoot, kPlateBlueprints[next_bp++], p, x, z, ground_y, false, lift);
        }
        const auto set =
            ctx.lua_state.do_string(fmt::format("__osc_last_plate:SetFractionComplete({})\n", f));
        if (!set) spdlog::warn("SetFractionComplete: {}", set.error().message);
        ctx.sim.tick();
    };

    auto* army = ctx.sim.get_army(0);
    const auto frame = [&](f32 x, f32 z, const map::ScmapLighting& lighting = white_fill(),
                           f32 distance = 30.0f) {
        map::ScmapEnvironment env;
        env.terrain_shader = "TTerrain";
        env.cubemaps.emplace_back("<default>", "/textures/environment/no_such_cube.dds");
        ground.set_lighting(lighting, std::move(env));
        shots.recapture();
        return shots.shoot_frame(ground, x, z, distance);
    };
    const auto at = [&](f32 x, f32 z) { return middle(frame(x, z)); };
    const auto dispatch = [&](const std::string& fact) {
        std::ostringstream dump;
        r.dump_frame(dump);
        const bool found = dump.str().find(fact) != std::string::npos;
        t.check(found, "dispatch dump: " + fact);
    };

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
        t.check(dump.str().find("technique=") == std::string::npos,
                "dispatch recording is off by default for offscreen captures");
        r.set_record_dispatch(true);
        (void)frame(seen_x / n, seen_z / n);
        dispatch("technique=UEFBuild stage=pre-water pass=0 pipeline=fade "
                 "drawn=UEFBuild lane=2");
        dispatch("technique=UEFBuild stage=pre-water pass=1 pipeline=overlay "
                 "drawn=UEFBuild lane=2");
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
        dispatch("technique=UEFBuildCube stage=pre-water pass=0 pipeline=cube "
                 "drawn=UEFBuildCube lane=0");
        video = as_was;
        const Rgb expected_built = over(sky, albedo, a);
        const Rgb expected = over(sky, over(albedo, Rgb{0, 0, 1}, 0.65f), 0.5f * a);
        t.check(near(built, expected_built) && near(early, expected) && near(later, expected),
                fmt::format("Test 7: at Low, UEF build cubes show {} built ({} expected), {} and "
                            "{} at 0% a pulse apart ({})",
                            show(built), show(expected_built), show(early), show(later),
                            show(expected)));
    }

    // Tests 8-11 light the plates by a sun (0.5) 37 degrees from overhead,
    // an ambience of 0.25 and a fill of 0.2: Unit lights a plate 0.72, the
    // ambience alone 0.4 (0.25 + 0.75 * 0.2), and the unlit show their albedo.
    map::ScmapLighting lit = white_fill();
    lit.multiplier = 1.0f;
    lit.sun_direction[0] = 0.0f;
    lit.sun_direction[1] = 0.8f;
    lit.sun_direction[2] = 0.6f;
    for (int i = 0; i < 3; ++i) {
        lit.sun_color[i] = 0.5f;
        lit.sun_ambience[i] = 0.25f;
        lit.shadow_fill[i] = 0.2f;
    }
    const auto at_lit = [&](f32 x, f32 z) { return middle(frame(x, z, lit)); };
    const Rgb sky_lit = at_lit(400.0f, 400.0f);
    const auto rgb = [](u8 r_, u8 g_, u8 b_) { return Rgb{r_ / 255.0f, g_ / 255.0f, b_ / 255.0f}; };
    const Rgb slice = rgb(200, 100, 50);
    const Rgb green = rgb(40, 200, 60);
    const Rgb cube = rgb(100, 150, 200);
    const f32 cube_a = 128.0f / 255.0f;
    Look effect;
    effect.shader = "Effect";
    effect.albedo = "albedo_green.dds";

    // Test 8: NukeUEF, the UEF nuke's dome: NukeHeadPS(1, 0.018, 50, 0.015)
    // blended (colour only), its depth written, tested over 0x0F. With flat
    // textures its four reads are the noise (the normals) N and the albedo A:
    // rgb ((((N.g + N.b) / 2 + N.b) / 2 + N.r / 2) lerped halfway to A, plus
    // 0.3 N.b) * N.g + N.b; alpha A.a * N.g until 50 ticks old. From 20
    // ticks old it's 0 where N.a * N.b * N.g (0.32) >= A.a; from 50 its
    // alpha fades by 0.015 a tick. Each dome stands 2 over an Effect plate
    // (opaque, drawn after it): a dome that writes depth hides it, one tested
    // away doesn't.
    {
        Look dome;
        dome.shader = "NukeUEF";
        dome.normals = "nuke_noise.dds";
        dome.albedo = "albedo_slice.dds";
        stand(effect, 1.0f, 100.0f, 220.0f);
        stand(dome, 1.0f, 100.0f, 220.0f, false, 2.0f);
        Look faint = dome;
        faint.albedo = "albedo_faint.dds";
        stand(effect, 1.0f, 130.0f, 220.0f);
        stand(faint, 1.0f, 130.0f, 220.0f, false, 2.0f);
        const Rgb noise = rgb(51, 204, 102);
        const f32 grey = ((noise[1] + noise[2]) * 0.5f + noise[2]) * 0.5f + noise[0] * 0.5f;
        Rgb head{};
        for (int c = 0; c < 3; ++c)
            head[c] = std::clamp(((grey + slice[c]) * 0.5f + noise[2] * 0.3f) * noise[1] + noise[2],
                                 0.0f, 1.0f);
        const Rgb solid = over(sky_lit, head, noise[1]);
        const Rgb thin = over(sky_lit, head, 64.0f / 255.0f * noise[1]);
        const Rgb dome_fresh = at_lit(100.0f, 220.0f);
        const Rgb faint_fresh = at_lit(130.0f, 220.0f);
        dispatch("technique=NukeUEF stage=post-water pass=0 pipeline=fade "
                 "drawn=NukeUEF lane=2");
        for (int i = 0; i < 25; ++i) ctx.sim.tick();
        const Rgb dome_later = at_lit(100.0f, 220.0f);
        const Rgb faint_later = at_lit(130.0f, 220.0f);
        for (int i = 0; i < 44; ++i) ctx.sim.tick();
        const Rgb dome_fading = at_lit(100.0f, 220.0f); // 70 ticks old: at 0.7
        const Rgb fading = over(sky_lit, head, 0.7f * noise[1]);
        t.check(near(dome_fresh, solid) && near(faint_fresh, thin) && near(dome_later, solid) &&
                    near(faint_later, green) && near(dome_fading, fading),
                fmt::format("Test 8: NukeUEF domes show {} ({} expected) and {} faint ({}) "
                            "just made; 25 ticks on {} ({}) and {} (dissolved: the Effect "
                            "plate below, {}); 70 ticks old, {} ({})",
                            show(dome_fresh), show(solid), show(faint_fresh), show(thin),
                            show(dome_later), show(solid), show(faint_later), show(green),
                            show(dome_fading), show(fading)));
        // Across the clock's wrap: the times are kept mod 36000, so a dome
        // made 50 ticks before it is 70 ticks old 20 ticks after it.
        while (ctx.sim.tick_count() < 35949) ctx.sim.tick();
        stand(dome, 1.0f, 340.0f, 220.0f);
        const u32 born = ctx.sim.tick_count();
        (void)at_lit(340.0f, 220.0f);
        for (int i = 0; i < 69; ++i) ctx.sim.tick();
        const Rgb wrapped = at_lit(340.0f, 220.0f);
        t.check(near(wrapped, fading),
                fmt::format("Test 8: a NukeUEF dome made at tick {} shows {} 70 ticks old, past "
                            "the clock's wrap at 36000 ({})",
                            born, show(wrapped), show(fading)));
    }

    // Test 9: OuterCloud: AlphaFadeTexShiftScalePS(1, -0.008, 180, 0.025),
    // unlit: the albedo (scrolling up), at alpha albedo.a times the normals'
    // green (128), fading from 180 ticks old, blended (colour only) with no
    // depth written: an Effect plate drawn after it, below, covers it. It
    // tests strictly less: level with a Unit plate (drawn before), it's hidden.
    {
        Look cloud;
        cloud.shader = "OuterCloud";
        cloud.albedo = "albedo_slice.dds";
        Look unit;
        unit.albedo = "albedo_green.dds";
        stand(cloud, 1.0f, 160.0f, 220.0f);
        stand(effect, 1.0f, 190.0f, 220.0f);
        stand(cloud, 1.0f, 190.0f, 220.0f, false, 2.0f);
        stand(unit, 1.0f, 310.0f, 220.0f);
        stand(cloud, 1.0f, 310.0f, 220.0f);
        const Rgb alone = at_lit(160.0f, 220.0f);
        const Rgb above = at_lit(190.0f, 220.0f);
        const Rgb level = at_lit(310.0f, 220.0f);
        dispatch("technique=OuterCloud stage=post-water pass=0 pipeline=outer_cloud "
                 "drawn=OuterCloud lane=2");
        dispatch("technique=Unit stage=pre-water pass=0 pipeline=mesh drawn=Unit lane=2");
        const Rgb lit_unit = scaled(green, 0.72f);
        for (int i = 0; i < 199; ++i) ctx.sim.tick();
        const Rgb faded = at_lit(160.0f, 220.0f); // 200 ticks old: half faded
        const f32 a = 128.0f / 255.0f;
        const Rgb expected = over(sky_lit, slice, a);
        t.check(near(alone, expected) && near(above, green) && near(level, lit_unit) &&
                    near(faded, over(sky_lit, slice, 0.5f * a)),
                fmt::format("Test 9: an OuterCloud plate shows {} ({} expected); over an Effect "
                            "plate, {} (the Effect plate, {}); level with a Unit plate, {} (the "
                            "Unit plate, {}); 200 ticks old, {} ({})",
                            show(alone), show(expected), show(above), show(green), show(level),
                            show(lit_unit), show(faded), show(over(sky_lit, slice, 0.5f * a))));
    }

    // Test 10: Cloud: EffectFadePS(180, 0.025): EffectVS leaves the normal 0,
    // so the albedo is lit by the ambience alone (0.4), at alpha albedo.a
    // times material.y (the fraction complete) and fading from 180 ticks
    // old, blended (colour only).
    {
        Look cloud;
        cloud.shader = "Cloud";
        cloud.albedo = "albedo_cube.dds";
        stand(cloud, 1.0f, 220.0f, 220.0f);
        stand(cloud, 0.5f, 250.0f, 220.0f);
        const Rgb ambient = scaled(cube, 0.4f);
        const Rgb whole = at_lit(220.0f, 220.0f);
        const Rgb half = at_lit(250.0f, 220.0f);
        for (int i = 0; i < 200; ++i) ctx.sim.tick();
        const Rgb faded = at_lit(250.0f, 220.0f); // 200 ticks old: half faded
        t.check(near(whole, over(sky_lit, ambient, cube_a)) &&
                    near(half, over(sky_lit, ambient, 0.5f * cube_a)) &&
                    near(faded, over(sky_lit, ambient, 0.25f * cube_a)),
                fmt::format("Test 10: Cloud plates show {} ({} expected), {} half built ({}); "
                            "200 ticks on, {} ({})",
                            show(whole), show(over(sky_lit, ambient, cube_a)), show(half),
                            show(over(sky_lit, ambient, 0.5f * cube_a)), show(faded),
                            show(over(sky_lit, ambient, 0.25f * cube_a))));
    }

    // Test 11: Effect (TMeshAdd): EffectPS sets no blend or depth state, so
    // it's opaque: its albedo unlit, whatever its alpha (which is its glow).
    {
        Look shock = effect;
        shock.albedo = "albedo_cube.dds";
        stand(shock, 1.0f, 280.0f, 220.0f);
        const Rgb shown = at_lit(280.0f, 220.0f);
        t.check(near(shown, cube), fmt::format("Test 11: an Effect plate shows {} (its albedo, {})",
                                               show(shown), show(cube)));
    }

    // What the last frame drew at (x, z) by `technique`: its draw scale (its
    // model's first column), its mesh's width, and its width in the world.
    struct Drawn {
        f32 scale = 0.0f;
        f32 mesh_width = 0.0f;
        f32 width = 0.0f;
    };
    const auto drawn_at = [&](f32 x, f32 z, MeshTechnique technique) {
        Drawn d;
        const renderer::MeshInstance* all = r.unit_renderer().mesh_instances();
        for (const auto& g : r.unit_renderer().mesh_groups())
            for (u32 k = 0; all && g.mesh && g.mesh->technique == technique && k < g.instance_count;
                 ++k) {
                const f32* m = all[g.instance_offset + k].model;
                if (std::abs(m[12] - x) > 0.05f || std::abs(m[14] - z) > 0.05f) continue;
                d.scale = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
                d.mesh_width = g.mesh->bounds_max.x - g.mesh->bounds_min.x;
                d.width = d.mesh_width * d.scale;
            }
        return d;
    };
    const auto lua_number = [&](const std::string& expr) {
        const auto got = ctx.lua_state.do_string(fmt::format("__osc_number = {}\n", expr));
        if (!got) spdlog::warn("{}: {}", expr, got.error().message);
        lua_pushstring(ctx.L, "__osc_number");
        lua_rawget(ctx.L, LUA_GLOBALSINDEX);
        const auto n = static_cast<f32>(lua_tonumber(ctx.L, -1));
        lua_pop(ctx.L, 1);
        return n;
    };

    // Test 12: the UEF nuke in context. A UEF commander dies on the test's
    // ground; its DeathWeapon's controller makes the dome (UEFNukeEffect01).
    // OSC_EFFECT_MESH_TEST_PNG=<prefix>: the frames, to see.
    {
        const auto killed = ctx.lua_state.do_string(
            "__osc_nuke_acu = CreateUnitHPR('uel0001', 'ARMY_1', 32, 0, 40, 0, 0, 0)\n");
        if (!killed) spdlog::warn("the commander: {}", killed.error().message);
        // SetScale names the draw scale, whatever the blueprint's UniformScale:
        // the dome's (17) set to 8.5 is drawn 8.5 times its mesh, a debris's
        // (0.15) set to 0.3 is drawn 0.3 times its. Each stands off the
        // commander, which the frame draws too.
        const auto scaled_width = [&](const char* bp, f32 scale, f32 offset, f32 distance) {
            const auto made = ctx.lua_state.do_string(fmt::format(
                "__osc_scaled = __osc_nuke_acu:CreateProjectile('{}', {}, 0, 0, nil, nil, nil)\n"
                "__osc_scaled:SetScale({})\n",
                bp, offset, scale));
            if (!made) spdlog::warn("{}: {}", bp, made.error().message);
            // Read whole: a projectile's id is past a float's 24 bits.
            (void)ctx.lua_state.do_string("__osc_number = __osc_scaled:GetEntityId()");
            lua_pushstring(ctx.L, "__osc_number");
            lua_rawget(ctx.L, LUA_GLOBALSINDEX);
            const auto* p = ctx.sim.entity_registry().find(osc::sim::to_entity_id(ctx.L, -1));
            lua_pop(ctx.L, 1);
            const f32 x = p ? p->position().x : 0.0f;
            const f32 z = p ? p->position().z : 0.0f;
            (void)frame(x, z, map::ScmapLighting{}, distance);
            const Drawn d = drawn_at(x, z, r.mesh_technique(lower(bp), ctx.L));
            const auto gone = ctx.lua_state.do_string("__osc_scaled:Destroy()\n");
            if (!gone) spdlog::warn("{}'s end: {}", bp, gone.error().message);
            return d;
        };
        const Drawn dome = scaled_width("/effects/entities/UEFNukeEffect01/UEFNukeEffect01_proj.bp",
                                        8.5f, 40.0f, 160.0f);
        const Drawn debris =
            scaled_width("/effects/entities/DebrisMisc04/DebrisMisc04_proj.bp", 0.3f, 10.0f, 30.0f);
        t.check(dome.mesh_width > 0.0f && debris.mesh_width > 0.0f &&
                    std::abs(dome.width - 8.5f * dome.mesh_width) < 1e-3f * dome.width &&
                    std::abs(debris.width - 0.3f * debris.mesh_width) < 1e-3f * debris.width,
                fmt::format("Test 12: SetScale(8.5) draws the dome {:.3f} across ({:.3f}: its "
                            "mesh, {:.3f}, times 8.5); SetScale(0.3) a debris (UniformScale "
                            "0.15) {:.4f} ({:.4f}: its mesh, {:.4f}, times 0.3)",
                            dome.width, 8.5f * dome.mesh_width, dome.mesh_width, debris.width,
                            0.3f * debris.mesh_width, debris.mesh_width));
        for (int i = 0; i < 10; ++i) ctx.sim.tick();
        const auto dead =
            ctx.lua_state.do_string("Damage(nil, __osc_nuke_acu, 99999, nil, 'Normal')\n");
        if (!dead) spdlog::warn("its death: {}", dead.error().message);
        const char* dump = std::getenv("OSC_EFFECT_MESH_TEST_PNG");
        std::string dome_bp;
        u32 dome_draws = 0;
        f32 dome_x = 32.0f;
        f32 dome_z = 40.0f;
        // Its script grows it by 0.3 UniformScale a second for 4 seconds:
        // the width it's drawn at when made and 2 seconds on.
        int dome_born = 0;
        Drawn dome_made;
        Drawn dome_2s;
        for (int i = 1; i <= 60; ++i) {
            ctx.sim.tick();
            ctx.sim.entity_registry().for_each([&](const sim::Entity& e) {
                if (e.destroyed() || !e.is_projectile() ||
                    lower(e.blueprint_id()).find("uefnukeeffect01") == std::string::npos)
                    return;
                dome_bp = e.blueprint_id();
                dome_x = e.position().x;
                dome_z = e.position().z;
                if (dome_born == 0) dome_born = i;
            });
            if (dome_born > 0 && (i == dome_born || i == dome_born + 20)) {
                (void)frame(dome_x, dome_z, map::ScmapLighting{}, 160.0f);
                (i == dome_born ? dome_made : dome_2s) =
                    drawn_at(dome_x, dome_z, MeshTechnique::NukeUEF);
            }
            if (i != 15 && i != 50) continue;
            const ImageRGBA8 image = frame(dome_x, dome_z, map::ScmapLighting{}, 160.0f);
            dome_draws += r.mesh_draws(MeshTechnique::NukeUEF);
            if (dump) write_png(fmt::format("{}_nuke_t{}.png", dump, i), image);
        }
        // Close in on its side: its centre is out of view, its surface fills it.
        const ImageRGBA8 side = frame(dome_x + 25.0f, dome_z, map::ScmapLighting{}, 30.0f);
        const u32 side_draws = r.mesh_draws(MeshTechnique::NukeUEF);
        if (dump) write_png(fmt::format("{}_nuke_side.png", dump), side);
        const MeshTechnique seen =
            dome_bp.empty() ? MeshTechnique::Unit : r.mesh_technique(dome_bp, ctx.L);
        t.check(seen == MeshTechnique::NukeUEF && dome_draws > 0,
                fmt::format("Test 12: the UEF commander's death makes its nuke's dome ({}), "
                            "drawn by technique {} ({} NukeUEF draws in the frames)",
                            dome_bp.empty() ? "never made" : dome_bp, static_cast<u32>(seen),
                            dome_draws));
        const f32 dome_scale =
            dome_bp.empty()
                ? 0.0f
                : lua_number(fmt::format("__blueprints['{}'].Display.UniformScale", dome_bp));
        const f32 growth = (dome_2s.width - dome_made.width) / 2.0f;
        const f32 expected_growth = 0.3f * dome_scale * dome_made.mesh_width;
        t.check(std::abs(growth - expected_growth) < 0.01f * expected_growth,
                fmt::format("Test 12: the dome's drawn width grows {:.2f} units a second ({:.2f} "
                            "expected: its mesh, {:.3f} across, by 0.3 of its UniformScale, {})",
                            growth, expected_growth, dome_made.mesh_width, dome_scale));
        t.check(
            side_draws > 0,
            fmt::format("Test 12: seen from close by its side, the dome draws ({} NukeUEF draws)",
                        side_draws));
    }

    // Test 13: NukeEMP (Depth_Enable_Less_Write_None, POSTWATER + PREEFFECT).
    {
        Look emp;
        emp.shader = "NukeEMP";
        emp.albedo = "albedo_slice.dds";
        Look unit;
        unit.albedo = "albedo_green.dds";
        stand(emp, 1.0f, 100.0f, 250.0f);
        stand(effect, 1.0f, 130.0f, 250.0f);
        stand(emp, 1.0f, 130.0f, 250.0f, false, 2.0f);
        stand(unit, 1.0f, 160.0f, 250.0f);
        stand(emp, 1.0f, 160.0f, 250.0f);
        const Rgb alone = at_lit(100.0f, 250.0f);
        const Rgb above = at_lit(130.0f, 250.0f);
        const Rgb level = at_lit(160.0f, 250.0f);
        dispatch("technique=NukeEMP stage=post-water pass=0 pipeline=nuke_emp "
                 "drawn=NukeEMP lane=2");
        const Rgb expected = over(sky_lit, slice, 128.0f / 255.0f);
        const Rgb lit_unit = scaled(green, 0.72f);
        t.check(near(alone, expected) && near(above, green) && near(level, lit_unit),
                fmt::format("Test 13: a NukeEMP plate shows {} ({} expected); over an Effect "
                            "plate, {} (the Effect plate, {}); level with a Unit plate, {} (the "
                            "Unit plate, {})",
                            show(alone), show(expected), show(above), show(green), show(level),
                            show(lit_unit)));
    }

    // Test 14: NukeQuantum (ColorWriteEnable 0x0D, POSTWATER + POSTEFFECT).
    {
        Look quantum;
        quantum.shader = "NukeQuantum";
        Look unit;
        unit.albedo = "albedo_green.dds";
        stand(unit, 1.0f, 190.0f, 250.0f);
        stand(quantum, 1.0f, 190.0f, 250.0f, false, 2.0f);
        stand(quantum, 1.0f, 220.0f, 250.0f);
        const u32 born = ctx.sim.tick_count();
        const Rgb above = at_lit(190.0f, 250.0f);
        dispatch("technique=NukeQuantum stage=post-effect pass=0 pipeline=nuke_quantum "
                 "drawn=NukeQuantum lane=2");
        const f32 a = 128.0f / 255.0f;
        const Rgb lit_unit = scaled(green, 0.72f);
        const auto red_blue = [](const Rgb& below, f32 alpha) {
            return Rgb{below[0] + (1.0f - below[0]) * alpha, below[1],
                       below[2] + (1.0f - below[2]) * alpha};
        };
        while (ctx.sim.tick_count() < born + 169) {
            ctx.sim.tick();
        }
        const Rgb faded = at_lit(220.0f, 250.0f);
        t.check(near(above, red_blue(lit_unit, a)) && near(faded, red_blue(sky_lit, 0.5f * a)),
                fmt::format("Test 14: a NukeQuantum plate over a Unit plate shows {} ({} "
                            "expected: the Unit plate's green); 170 ticks old, {} ({})",
                            show(above), show(red_blue(lit_unit, a)), show(faded),
                            show(red_blue(sky_lit, 0.5f * a))));
    }

    const auto red_crossings = [&](f32 x, f32 z, int cut) {
        const ImageRGBA8 image = frame(x, z, lit);
        const u32 y = image.height / 2;
        const auto at = [&](u32 x0, u32 c) {
            return static_cast<int>(
                image.pixels[(static_cast<size_t>(y) * image.width + x0) * 4 + c]);
        };
        const auto off_sky = [&](u32 x0) {
            for (u32 c = 0; c < 3; ++c) {
                if (std::abs(at(x0, c) - static_cast<int>(std::lround(sky_lit[c] * 255.0f))) > 12) {
                    return true;
                }
            }
            return false;
        };
        u32 left = 0;
        while (left < image.width && !off_sky(left)) {
            ++left;
        }
        u32 right = image.width;
        while (right > left && !off_sky(right - 1)) {
            --right;
        }
        const f32 width = static_cast<f32>(right - left);
        std::vector<f32> found;
        for (u32 x0 = left; x0 + 1 < right; ++x0) {
            const f32 e = static_cast<f32>(x0 + 1 - left) / width;
            if (e > 0.05f && e < 0.95f && (at(x0, 0) > cut) != (at(x0 + 1, 0) > cut)) {
                found.push_back(e);
            }
        }
        return found;
    };
    const auto nearest_middle = [](const std::vector<f32>& found) {
        f32 best = 1.0f;
        for (const f32 e : found) {
            if (std::abs(e - 0.5f) < std::abs(best - 0.5f)) {
                best = e;
            }
        }
        return best;
    };

    // Test 15: AlphaFadeTexShiftScalePS's scroll and scale, on plates turned so
    // that their v runs across the frame.
    {
        const auto band = [](const std::vector<f32>& found) {
            if (found.size() < 2) {
                return 0.0f;
            }
            return (found.back() - found.front()) / static_cast<f32>(found.size() - 1);
        };
        for (const char* shader : {"NukeEMP", "NukeQuantum"}) {
            Look banded;
            banded.shader = shader;
            banded.albedo = "albedo_bands.dds";
            stand(banded, 1.0f, std::string(shader) == "NukeEMP" ? 250.0f : 280.0f, 280.0f);
            const auto turned =
                ctx.lua_state.do_string("__osc_last_plate:SetOrientation({0, 0.70710678, 0, "
                                        "0.70710678}, true)\n");
            if (!turned) {
                spdlog::warn("turning the plate: {}", turned.error().message);
            }
        }
        ctx.sim.tick();
        const std::vector<f32> emp_before = red_crossings(250.0f, 280.0f, 128);
        const std::vector<f32> quantum_before = red_crossings(280.0f, 280.0f, 128);
        for (int i = 0; i < 10; ++i) {
            ctx.sim.tick();
        }
        const std::vector<f32> emp_after = red_crossings(250.0f, 280.0f, 128);
        const std::vector<f32> quantum_after = red_crossings(280.0f, 280.0f, 128);
        const f32 emp_moved = std::abs(nearest_middle(emp_after) - nearest_middle(emp_before));
        const f32 quantum_moved =
            std::abs(nearest_middle(quantum_after) - nearest_middle(quantum_before));
        const f32 emp_band = band(emp_after);
        const f32 quantum_band = band(quantum_after);
        t.check(emp_moved > 0.06f && emp_moved < 0.10f && quantum_moved > 0.10f &&
                    quantum_moved < 0.14f && std::abs(emp_band - 0.25f) < 0.04f &&
                    std::abs(quantum_band - 0.5f) < 0.06f,
                fmt::format("Test 15: over 10 ticks a NukeEMP plate's bands move {:.3f} of the "
                            "plate (0.08 expected), {:.3f} wide (0.25); a NukeQuantum plate's "
                            "{:.3f} (0.12), {:.3f} wide (0.5)",
                            emp_moved, emp_band, quantum_moved, quantum_band));
    }

    // Test 16: the nukes write their alpha, the frame's glow; OuterCloud doesn't.
    {
        map::ScmapLighting no_map_bloom = lit;
        no_map_bloom.bloom = 0.0f;
        const auto halo = [&](f32 x) {
            const ImageRGBA8 off = frame(x, 310.0f, no_map_bloom);
            r.set_bloom_enabled(true);
            const ImageRGBA8 on = frame(x, 310.0f, no_map_bloom);
            r.set_bloom_enabled(false);
            size_t n = 0;
            for (size_t i = 0; i + 2 < off.pixels.size() && i + 2 < on.pixels.size(); i += 4) {
                bool sky_pixel = true;
                for (int c = 0; c < 3; ++c) {
                    sky_pixel = sky_pixel && std::abs(off.pixels[i + static_cast<size_t>(c)] -
                                                      std::lround(sky_lit[c] * 255.0f)) <= 3;
                }
                if (sky_pixel && on.pixels[i] > off.pixels[i] + 8) {
                    ++n;
                }
            }
            return n;
        };
        std::array<size_t, 3> haloes{};
        const std::array<const char*, 3> shaders = {"NukeEMP", "NukeQuantum", "OuterCloud"};
        for (size_t i = 0; i < shaders.size(); ++i) {
            Look glowing;
            glowing.shader = shaders[i];
            glowing.normals = "mask_full.dds";
            stand(glowing, 1.0f, 100.0f + 60.0f * static_cast<f32>(i), 310.0f);
        }
        for (size_t i = 0; i < shaders.size(); ++i) {
            haloes[i] = halo(100.0f + 60.0f * static_cast<f32>(i));
        }
        t.check(haloes[0] > 1000 && haloes[1] > 1000 && haloes[2] < 20,
                fmt::format("Test 16: with the bloom on, {} pixels of sky brighten about a "
                            "NukeEMP plate, {} about a NukeQuantum one, {} about an OuterCloud "
                            "one",
                            haloes[0], haloes[1], haloes[2]));
    }

    // Test 17: an Aeon and a Cybran commander's DeathWeapons.
    {
        struct Death {
            const char* commander;
            const char* effect;
            MeshTechnique technique;
            f32 x;
            f32 z;
        };
        const std::array<Death, 2> deaths = {
            Death{"ual0001", "quantumwarheadeffect01", MeshTechnique::NukeQuantum, 200.0f, 400.0f},
            Death{"url0001", "empfluxwarheadeffect03", MeshTechnique::NukeEMP, 320.0f, 400.0f}};
        const char* dump = std::getenv("OSC_EFFECT_MESH_TEST_PNG");
        for (const Death& death : deaths) {
            const auto killed = ctx.lua_state.do_string(
                fmt::format("local acu = CreateUnitHPR('{}', 'ARMY_1', {}, 0, {}, 0, 0, 0)\n"
                            "Damage(nil, acu, 99999, nil, 'Normal')\n",
                            death.commander, death.x, death.z));
            if (!killed) {
                spdlog::warn("{}: {}", death.commander, killed.error().message);
            }
            std::string effect_bp;
            f32 effect_x = death.x;
            f32 effect_z = death.z;
            u32 draws = 0;
            int made_at = 0;
            for (int i = 1; i <= 120; ++i) {
                ctx.sim.tick();
                ctx.sim.entity_registry().for_each([&](const sim::Entity& e) {
                    if (e.destroyed() || !e.is_projectile() ||
                        lower(e.blueprint_id()).find(death.effect) == std::string::npos) {
                        return;
                    }
                    effect_bp = e.blueprint_id();
                    effect_x = e.position().x;
                    effect_z = e.position().z;
                });
                if (!effect_bp.empty() && made_at == 0) {
                    made_at = i;
                }
                if (made_at == 0 || (i != made_at + 10 && i != made_at + 50)) {
                    continue;
                }
                const ImageRGBA8 image = frame(effect_x, effect_z, map::ScmapLighting{}, 60.0f);
                draws += r.mesh_draws(death.technique);
                if (dump) {
                    write_png(fmt::format("{}_{}_t{}.png", dump, death.commander, i - made_at),
                              image);
                }
            }
            const MeshTechnique seen =
                effect_bp.empty() ? MeshTechnique::Unit : r.mesh_technique(effect_bp, ctx.L);
            t.check(seen == death.technique && draws > 0,
                    fmt::format("Test 17: the {} commander's death makes {} ({}), drawn by "
                                "technique {} ({} {} draws in the frames)",
                                death.commander, death.effect,
                                effect_bp.empty() ? "never made" : effect_bp,
                                static_cast<u32>(seen), draws,
                                renderer::mesh_technique_name(death.technique)));
        }
    }

    const auto middle_crossing_shift = [&](const std::vector<f32>& before,
                                           const std::vector<f32>& after) {
        const f32 from = nearest_middle(before);
        f32 best = 1.0f;
        for (const f32 e : after) {
            best = std::min(best, std::abs(e - from));
        }
        return best;
    };

    // Test 18: PhalanxEffect (TwoTexShiftScalePS, AlphaBlend_One_InvSrcAlpha_Write_RGBA,
    // Depth_Enable_Less_Write_None, POSTWATER + PREEFFECT).
    {
        Look sphere;
        sphere.shader = "PhalanxEffect";
        sphere.albedo = "albedo_slice.dds";
        Look unit;
        unit.albedo = "albedo_green.dds";
        stand(unit, 1.0f, 100.0f, 480.0f);
        stand(sphere, 1.0f, 100.0f, 480.0f, false, 2.0f);
        stand(effect, 1.0f, 130.0f, 480.0f);
        stand(sphere, 1.0f, 130.0f, 480.0f, false, 2.0f);
        Look striped = sphere;
        striped.albedo = "albedo_stripes.dds";
        stand(striped, 1.0f, 160.0f, 480.0f);
        const Rgb above = at_lit(100.0f, 480.0f);
        const Rgb over_effect = at_lit(130.0f, 480.0f);
        dispatch("technique=PhalanxEffect stage=post-water pass=0 pipeline=phalanx "
                 "drawn=PhalanxEffect lane=2");
        const Rgb normal = rgb(0, 128, 0);
        const Rgb lit_unit = scaled(green, 0.72f);
        Rgb expected{};
        for (int c = 0; c < 3; ++c) {
            expected[c] = (slice[c] + normal[c]) * 0.2f + 0.8f * lit_unit[c];
        }
        const std::vector<f32> before = red_crossings(160.0f, 480.0f, 138);
        for (int i = 0; i < 2; ++i) {
            ctx.sim.tick();
        }
        const f32 moved = middle_crossing_shift(before, red_crossings(160.0f, 480.0f, 138));
        t.check(near(above, expected) && near(over_effect, green) && moved > 0.08f && moved < 0.12f,
                fmt::format("Test 18: a PhalanxEffect plate over a Unit plate shows {} ({} "
                            "expected); over an Effect plate, {} (the Effect plate, {}); its "
                            "stripes move {:.3f} of the plate in 2 ticks (0.1)",
                            show(above), show(expected), show(over_effect), show(green), moved));
    }

    // Test 19: UEFQuantumGate (TexSubAlphaMaskStaticLoFiPS, Rasterizer_Cull_None).
    {
        Look field;
        field.shader = "UEFQuantumGate";
        Look banded = field;
        banded.albedo = "albedo_bands.dds";
        stand(banded, 1.0f, 130.0f, 510.0f);
        const auto turned = ctx.lua_state.do_string(
            "__osc_last_plate:SetOrientation({0, 0.70710678, 0, 0.70710678}, true)\n");
        if (!turned) {
            spdlog::warn("turning the plate: {}", turned.error().message);
        }
        std::array<Rgb, 4> sides{};
        for (u32 i = 0; i < 4; ++i) {
            Look one = i < 2 ? Look{} : field;
            one.mesh = i % 2 == 0 ? "plate_side0.scm" : "plate_side1.scm";
            stand(one, 1.0f, 160.0f + 30.0f * static_cast<f32>(i), 510.0f);
        }
        ctx.sim.tick();
        for (u32 i = 0; i < 4; ++i) {
            sides[i] = at_lit(160.0f + 30.0f * static_cast<f32>(i), 510.0f);
        }
        dispatch("technique=UEFQuantumGate stage=post-water pass=0 pipeline=quantum_gate "
                 "drawn=UEFQuantumGate lane=2");
        const std::vector<f32> before = red_crossings(130.0f, 510.0f, 128);
        for (int i = 0; i < 3; ++i) {
            ctx.sim.tick();
        }
        const f32 moved = middle_crossing_shift(before, red_crossings(130.0f, 510.0f, 128));
        const Rgb expected = over(sky_lit, Rgb{1.0f, 1.0f - 128.0f / 255.0f, 1.0f}, 0.4f);
        const bool unit_one_side = near(sides[0], sky_lit) != near(sides[1], sky_lit);
        t.check(unit_one_side && near(sides[2], expected) && near(sides[3], expected) &&
                    moved > 0.06f && moved < 0.09f,
                fmt::format("Test 19: wound one way and the other, a Unit plate shows {} and {} "
                            "(one the sky, {}), a UEFQuantumGate plate {} and {} ({} expected); "
                            "its bands move {:.3f} of the plate in 3 ticks (0.075)",
                            show(sides[0]), show(sides[1]), show(sky_lit), show(sides[2]),
                            show(sides[3]), show(expected), moved));
    }

    // Test 20: URB4201's zapper sphere and UEB0304's field.
    {
        struct Host {
            const char* unit;
            MeshTechnique technique;
            f32 x;
        };
        for (const Host& host : {Host{"urb4201", MeshTechnique::PhalanxEffect, 150.0f},
                                 Host{"ueb0304", MeshTechnique::UEFQuantumGate, 260.0f}}) {
            const auto made = ctx.lua_state.do_string(fmt::format(
                "CreateUnitHPR('{}', 'ARMY_1', {}, 0, 560, 0, 0, 0)\n", host.unit, host.x));
            if (!made) {
                spdlog::warn("{}: {}", host.unit, made.error().message);
            }
            for (int i = 0; i < 20; ++i) {
                ctx.sim.tick();
            }
            (void)frame(host.x, 560.0f, map::ScmapLighting{}, 60.0f);
            const u32 draws = r.mesh_draws(host.technique);
            t.check(draws > 0,
                    fmt::format("Test 20: a {} carries a mesh drawn by {} ({} draws)", host.unit,
                                renderer::mesh_technique_name(host.technique), draws));
        }
    }

    // Test 21: CommandFeedback3 (Depth_Disable, POSTWATER + PREEFFECT).
    {
        Look ping;
        ping.shader = "CommandFeedback3";
        ping.albedo = "albedo_cube.dds";
        Look unit;
        unit.albedo = "albedo_green.dds";
        stand(unit, 1.0f, 100.0f, 600.0f, false, 2.0f);
        stand(ping, 1.0f, 100.0f, 600.0f);
        Look dim = ping;
        dim.albedo = "albedo_dim.dds";
        stand(dim, 1.0f, 130.0f, 600.0f);
        const Rgb under = at_lit(100.0f, 600.0f);
        const Rgb faint = at_lit(130.0f, 600.0f);
        dispatch("technique=CommandFeedback3 stage=post-water pass=0 pipeline=feedback "
                 "drawn=CommandFeedback3 lane=2");
        for (int i = 0; i < 50; ++i) {
            ctx.sim.tick();
        }
        const Rgb later = at_lit(100.0f, 600.0f);
        const Rgb expected = over(scaled(green, 0.72f), cube, cube_a);
        const Rgb expected_faint = over(sky_lit, slice, 30.0f / 255.0f);
        t.check(near(under, expected) && near(later, expected) && near(faint, expected_faint),
                fmt::format("Test 21: a CommandFeedback3 plate under a Unit plate shows {} ({} "
                            "expected), 50 ticks on {}; at alpha 30, {} ({})",
                            show(under), show(expected), show(later), show(faint),
                            show(expected_faint)));
    }

    // Test 22: CommandFeedback4 (Depth_Enable_LessEqual_Write_None, DepthBias = -0.0001).
    {
        Look beacon;
        beacon.shader = "CommandFeedback4";
        beacon.albedo = "albedo_cube.dds";
        Look unit;
        unit.albedo = "albedo_green.dds";
        stand(unit, 1.0f, 160.0f, 600.0f, false, 2.0f);
        stand(beacon, 1.0f, 160.0f, 600.0f);
        stand(unit, 1.0f, 190.0f, 600.0f, false, 0.1f);
        stand(beacon, 1.0f, 190.0f, 600.0f);
        const Rgb deep = at_lit(160.0f, 600.0f);
        const Rgb shallow = at_lit(190.0f, 600.0f);
        dispatch("technique=CommandFeedback4 stage=post-water pass=0 pipeline=cube "
                 "drawn=CommandFeedback4 lane=2");
        const Rgb lit_unit = scaled(green, 0.72f);
        const Rgb expected = over(lit_unit, cube, cube_a);
        t.check(near(deep, lit_unit) && near(shallow, expected),
                fmt::format("Test 22: a CommandFeedback4 plate 2 under a Unit plate shows {} (the "
                            "Unit plate, {}); 0.1 under one, {} ({})",
                            show(deep), show(lit_unit), show(shallow), show(expected)));
    }

    // Test 23: retail's ping (SimPing.SpawnPing), out of ARMY_1's sight, and a UEF ferry beacon.
    {
        const auto pinged = ctx.lua_state.do_string(
            fmt::format("import('/lua/SimPing.lua').SpawnPing({{Owner = 0, Location = {{250, {}, "
                        "600}}, Mesh = 'alert_marker', Lifetime = 60, Type = 'alert'}})\n"
                        "CreateUnitHPR('ueb5102', 'ARMY_1', 300, 0, 600, 0, 0, 0)\n",
                        ground_y));
        if (!pinged) {
            spdlog::warn("the ping: {}", pinged.error().message);
        }
        for (int i = 0; i < 5; ++i) {
            ctx.sim.tick();
        }
        (void)frame(250.0f, 600.0f, lit, 40.0f);
        const u32 ping_draws = r.mesh_draws(MeshTechnique::CommandFeedback3);
        (void)frame(300.0f, 600.0f, lit, 40.0f);
        const u32 beacon_draws = r.mesh_draws(MeshTechnique::CommandFeedback4);
        t.check(ping_draws > 0 && beacon_draws > 0,
                fmt::format("Test 23: a ping draws by CommandFeedback3 ({} draws), a UEF ferry "
                            "beacon by CommandFeedback4 ({})",
                            ping_draws, beacon_draws));
    }

    // Test 24: Flat (TMeshNoLighting: FlatPS, alpha-tested over 0x7F). A unit's instance
    // colour is its army's (Test 4's).
    {
        Look flat;
        flat.shader = "TMeshNoLighting";
        stand(flat, 1.0f, 100.0f, 640.0f);
        Look faint = flat;
        faint.albedo = "albedo_faint.dds";
        stand(faint, 1.0f, 130.0f, 640.0f);
        Look slice_flat = flat;
        slice_flat.albedo = "albedo_slice.dds";
        stand(slice_flat, 0.4f, 160.0f, 640.0f);
        const Rgb shown = at_lit(100.0f, 640.0f);
        const Rgb faint_shown = at_lit(130.0f, 640.0f);
        const Rgb cut = at_lit(160.0f, 640.0f);
        dispatch("technique=Flat stage=pre-water pass=0 pipeline=mesh drawn=Flat lane=2");
        const Rgb team = rgb(220, 30, 30);
        t.check(near(shown, team) && near(faint_shown, sky_lit) && near(cut, sky_lit),
                fmt::format("Test 24: under the sun a Flat plate shows {} (its albedo times its "
                            "army's colour, {}); at alpha 64, {}, and 40% built, {} (the sky, {})",
                            show(shown), show(team), show(faint_shown), show(cut), show(sky_lit)));
    }

    // The dump sees actual shadow draws, with terrain in view.
    {
        stand(Look{}, 1.0f, 32.0f, 32.0f);
        Look cube;
        cube.shader = "UEFBuildCube";
        stand(cube, 1.0f, 40.0f, 32.0f);
        (void)frame(36.0f, 32.0f);
        dispatch("technique=Unit stage=shadow pass=0 pipeline=shadow drawn=Unit lane=2");
        Look shield;
        shield.shader = "SeraphimPersonalShield";
        stand(shield, 1.0f, 48.0f, 32.0f);
        r.video_options().graphics_fidelity = 1;
        (void)frame(48.0f, 32.0f);
        dispatch("pass=0 pipeline=mesh drawn=Seraphim lane=0");
        dispatch("pass=1 pipeline=shield:BlendDepthWrite drawn=PhaseShield lane=0");
        r.video_options().graphics_fidelity = 2;
    }

    r.set_record_dispatch(false);
    (void)frame(36.0f, 32.0f);
    std::ostringstream unrecorded;
    r.dump_frame(unrecorded);
    t.check(unrecorded.str().find("technique=") == std::string::npos,
            "disabling dispatch recording leaves no records in the next frame");

    // Equal SortOrder ties follow the first entity, for both GPU address orders.
    {
        Look cloud;
        cloud.shader = "Cloud";
        const std::string first_bp = fmt::format("{}/{}_plate", kRoot, kPlateBlueprints[next_bp]);
        stand(cloud, 1.0f, 440.0f, 440.0f);
        t.check(
            static_cast<bool>(ctx.lua_state.do_string("__osc_order_first = __osc_last_plate\n")),
            "entity order: keep the first entity");
        cloud.albedo = "albedo_cube.dds";
        const std::string second_bp = fmt::format("{}/{}_plate", kRoot, kPlateBlueprints[next_bp]);
        stand(cloud, 1.0f, 448.0f, 440.0f);
        t.check(
            static_cast<bool>(ctx.lua_state.do_string("__osc_order_second = __osc_last_plate\n")),
            "entity order: keep the second entity");
        (void)frame(444.0f, 440.0f);
        std::array<const renderer::GPUMesh*, 2> meshes{};
        for (const auto& group : r.unit_renderer().mesh_groups()) {
            const auto& instance = r.unit_renderer().mesh_instances()[group.instance_offset];
            if (instance.model[12] == 440.0f) meshes[0] = group.mesh;
            if (instance.model[12] == 448.0f) meshes[1] = group.mesh;
        }
        t.check(meshes[0] && meshes[1], "entity order: two distinct translucent mesh groups");
        if (meshes[0] && meshes[1]) {
            t.check(meshes[0] != meshes[1] && meshes[0]->sort_order == meshes[1]->sort_order,
                    "entity order: distinct GPU meshes have equal SortOrder");
            const bool first_lower = std::less<const renderer::GPUMesh*>{}(meshes[0], meshes[1]);
            for (bool reverse : {false, true}) {
                const auto changed = ctx.lua_state.do_string(
                    fmt::format("__osc_order_first:SetMesh('{}', true)\n"
                                "__osc_order_second:SetMesh('{}', true)\n",
                                reverse ? second_bp : first_bp, reverse ? first_bp : second_bp));
                t.check(static_cast<bool>(changed), "entity order: assign the mesh pair");
                ctx.sim.tick();
                (void)frame(444.0f, 440.0f);
                std::vector<f32> positions;
                for (const auto& group : r.unit_renderer().mesh_groups()) {
                    if (group.mesh != meshes[0] && group.mesh != meshes[1]) continue;
                    positions.push_back(
                        r.unit_renderer().mesh_instances()[group.instance_offset].model[12]);
                }
                t.check(positions == std::vector<f32>{440.0f, 448.0f},
                        fmt::format("entity order: equal SortOrder draws first entity first "
                                    "({} GPU address first)",
                                    first_lower != reverse ? "lower" : "higher") +
                            fmt::format("; {} groups, x = {} then {}", positions.size(),
                                        positions.empty() ? -1.0f : positions[0],
                                        positions.size() < 2 ? -1.0f : positions[1]));
            }
        }
    }

    spdlog::info("Effect mesh test: {}/{} passed", t.pass, t.pass + t.fail);
}

} // namespace osc::test
