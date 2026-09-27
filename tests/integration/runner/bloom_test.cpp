// --bloom-test (M211e): FA's bloom, from the glow in the frame's alpha.
//
// FA blooms what glows, and what glows is written to the frame's alpha (a
// unit's SpecTeam blue plus 0.01; older terrain's specular). Its bloom
// copies the frame weighted by saturate((a - 0.02) * 2 + the map's bloom),
// blurs it twice over at half size (seven taps, each pass times 1.5), and
// adds it back. Plates of the test's own stand off its flat ground; frames
// with the bloom on and off are compared.

#include "integration_tests.hpp"
#include "plate_fixtures.hpp"
#include "render_probe.hpp"

#include "lua/lua_state.hpp"
#include "map/heightmap.hpp"
#include "map/terrain.hpp"
#include "sim/entity.hpp"
#include "sim/sim_state.hpp"
#include "vfs/directory_mount.hpp"
#include "vfs/virtual_file_system.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

namespace osc::test {

namespace {

constexpr u32 kSize = 64;
constexpr const char* kRoot = "/osc_bloom_test";

} // namespace

void test_bloom(TestContext& ctx) {
    spdlog::info("=== BLOOM TEST: FA's glow bloom (M211e) ===");
    Tally t;

    // The ground's height: SCMP_009's, under a structure in its corner.
    const auto made = ctx.lua_state.do_string(
        "__osc_bloom_probe = CreateUnitHPR('ueb0101', 'ARMY_1', 32, 0, 32, 0, 0, 0)\n");
    if (!made) spdlog::warn("CreateUnitHPR failed: {}", made.error().message);
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

    // Plates of the test's own: albedos dark, darker-grey and grey; SpecTeams
    // glowing fully, a fifth, and not at all (red, green and alpha 0).
    const auto dir = std::filesystem::temp_directory_path() / "osc_bloom_test";
    std::filesystem::create_directories(dir);
    write_plate_scm(dir / "plate.scm", 4.0f);
    const auto flat = [](u8 r, u8 g, u8 b, u8 a) {
        return [=](int, u32, u32) { return std::array<u8, 4>{r, g, b, a}; };
    };
    write_dds(dir / "plate_normals.dds", 1, flat(0, 128, 0, 128));
    write_dds(dir / "albedo_10.dds", 1, flat(10, 10, 10, 255));
    write_dds(dir / "albedo_50.dds", 1, flat(50, 50, 50, 255));
    write_dds(dir / "albedo_128.dds", 1, flat(128, 128, 128, 255));
    write_dds(dir / "glow_full.dds", 1, flat(0, 0, 255, 0));
    write_dds(dir / "glow_fifth.dds", 1, flat(0, 0, 51, 0));
    write_dds(dir / "glow_none.dds", 1, flat(0, 0, 0, 0));
    write_dds(dir / "secondary_dark.dds", 1, flat(10, 10, 10, 128));
    ctx.vfs.mount(kRoot, std::make_unique<vfs::DirectoryMount>(dir));
    const auto plate = [](const char* albedo, const char* glow, const char* shader = "Unit") {
        Plate p;
        p.albedo = albedo;
        p.specteam = glow;
        p.shader = shader;
        p.secondary = "secondary_dark.dds";
        return p;
    };
    // Off the test's ground, on its level, 30 apart.
    stand_plate(ctx, kRoot, "ueb2101", plate("albedo_10.dds", "glow_full.dds"), 100, 100, ground_y);
    stand_plate(ctx, kRoot, "ueb2301", plate("albedo_50.dds", "glow_fifth.dds"), 130, 100,
                ground_y);
    stand_plate(ctx, kRoot, "ueb5101", plate("albedo_128.dds", "glow_none.dds"), 160, 100,
                ground_y);
    // A build slice (AlphaFade, M211g), whole and just made.
    stand_plate(ctx, kRoot, "ueb0201", plate("albedo_10.dds", "glow_none.dds", "AlphaFade"), 250,
                100, ground_y);
    // Half built, with the build shaders (M211f).
    for (const auto& [bp, shader, x] : {std::tuple{"ueb3101", "SeraphimBuild", 190.0f},
                                        std::tuple{"ueb3201", "UEFBuild", 220.0f}}) {
        stand_plate(ctx, kRoot, bp, plate("albedo_10.dds", "glow_full.dds", shader), x, 100,
                    ground_y);
        const auto half_built =
            ctx.lua_state.do_string("__osc_last_plate:SetFractionComplete(0.5)\n");
        if (!half_built) spdlog::warn("SetFractionComplete: {}", half_built.error().message);
        ctx.sim.tick();
    }

    OffscreenShots shots(ctx);
    if (!shots.ok()) {
        t.check(false, "renderer init (no Vulkan?)");
        return;
    }
    renderer::Renderer& r = shots.renderer();
    r.camera().set_pitch(1.1f);

    // A light of `fill` (no sun, no ambience), the map's `bloom`, and TTerrain
    // or its XP.
    const auto frame = [&](f32 x, f32 z, f32 fill, f32 bloom, bool xp, bool on) {
        map::ScmapLighting l = white_fill();
        for (f32& c : l.shadow_fill) c = fill;
        l.bloom = bloom;
        map::ScmapEnvironment env;
        env.terrain_shader = xp ? "TTerrainXP" : "TTerrain";
        ground.set_lighting(l, std::move(env));
        r.set_bloom_enabled(on);
        shots.recapture();
        return shots.shoot_frame(ground, x, z, 30.0f);
    };
    // The middle's brightness with the bloom on over off.
    const auto lift = [&](f32 x, f32 fill, f32 bloom) {
        const auto off = middle(frame(x, 100.0f, fill, bloom, false, false));
        const auto on = middle(frame(x, 100.0f, fill, bloom, false, true));
        return off[0] > 0.0f ? on[0] / off[0] : -1.0f;
    };

    // Test 1: a glowing plate haloes; the rest of the sky doesn't brighten.
    // Pixels of the sky (the frame without bloom shows its colour there)
    // brightened by the bloom, around a plate that glows and one that
    // doesn't. The map's bloom is 0.
    {
        const auto halo = [&](f32 x, f32 fill) {
            const ImageRGBA8 off = frame(x, 100.0f, fill, 0.0f, false, false);
            const ImageRGBA8 on = frame(x, 100.0f, fill, 0.0f, false, true);
            size_t n = 0;
            for (size_t i = 0; i + 2 < off.pixels.size() && i + 2 < on.pixels.size(); i += 4) {
                const bool sky = std::abs(off.pixels[i] - 140) <= 3 &&
                                 std::abs(off.pixels[i + 1] - 158) <= 3 &&
                                 std::abs(off.pixels[i + 2] - 184) <= 3;
                if (sky && on.pixels[i] > off.pixels[i] + 8) ++n;
            }
            return n;
        };
        const size_t glowing = halo(100.0f, 1.0f);
        const size_t dull = halo(160.0f, 1.0f);
        t.check(glowing > 1000 && dull < 20,
                fmt::format("Test 1: around a glowing plate, {} pixels of sky brighten; around "
                            "one that doesn't glow, {}",
                            glowing, dull));
    }

    // Test 2: how much. In the dark, a plate shows its albedo times twice its
    // glow; the bloom adds that times the copy's weight, then the blur's
    // growth, 1.5 * 0.998975 per pass over four passes (5.04). Glowing fully,
    // the weight is 1: 6.04 times the frame without bloom. A fifth, with an
    // albedo five times brighter (the same colour): alpha 0.21, weight
    // (0.21 - 0.02) * 2 = 0.38, 2.93 times.
    {
        const f32 full = lift(100.0f, 0.0f, 0.0f);
        const f32 fifth = lift(130.0f, 0.0f, 0.0f);
        t.check(full > 5.6f && full < 6.5f && fifth > 2.75f && fifth < 3.1f,
                fmt::format("Test 2: the bloom brightens a fully glowing plate {:.2f} times "
                            "(6.04 expected), one glowing a fifth {:.2f} (2.93)",
                            full, fifth));
    }

    // Test 3: what doesn't glow (alpha 0.01, under MinimumGlow, 0.02) isn't
    // copied, unless the map's bloom lifts it: 0.05 gives it a weight of
    // 0.03, 1 + 5.04 * 0.03 = 1.15 times.
    {
        const f32 none = lift(160.0f, 1.0f, 0.0f);
        const f32 map_bloom = lift(160.0f, 1.0f, 0.05f);
        t.check(none > 0.98f && none < 1.02f && map_bloom > 1.1f && map_bloom < 1.2f,
                fmt::format("Test 3: a plate that doesn't glow is brightened {:.3f} times; by "
                            "a map's bloom of 0.05, {:.3f} (1.15)",
                            none, map_bloom));
    }

    // Test 4: no white-out. At retail's usual bloom (0.036), the sky and XP
    // terrain (alpha 0) aren't touched; TTerrain (alpha 0.01, weight 0.016)
    // is lifted about 8%.
    {
        const auto mean = [](const ImageRGBA8& image) {
            f64 sum = 0;
            for (size_t i = 0; i + 2 < image.pixels.size(); i += 4) sum += image.pixels[i + 1];
            return sum / (static_cast<f64>(image.pixels.size()) / 4.0 * 255.0);
        };
        const f64 sky_off = mean(frame(400.0f, 400.0f, 1.0f, 0.036f, true, false));
        const f64 sky_on = mean(frame(400.0f, 400.0f, 1.0f, 0.036f, true, true));
        const f64 xp_off = mean(frame(12.0f, 50.0f, 1.0f, 0.036f, true, false));
        const f64 xp_on = mean(frame(12.0f, 50.0f, 1.0f, 0.036f, true, true));
        const f64 old_off = mean(frame(12.0f, 50.0f, 1.0f, 0.036f, false, false));
        const f64 old_on = mean(frame(12.0f, 50.0f, 1.0f, 0.036f, false, true));
        t.check(std::abs(sky_on - sky_off) < 0.003 && std::abs(xp_on - xp_off) < 0.003 &&
                    old_on / old_off > 1.04 && old_on / old_off < 1.12,
                fmt::format("Test 4: with the bloom on, the sky moves {:+.4f}, XP terrain "
                            "{:+.4f}, TTerrain {:.3f} times",
                            sky_on - sky_off, xp_on - xp_off, old_on / old_off));
    }

    // Test 5: a unit under construction glows by what its build technique
    // writes to alpha (M211f), not by its SpecTeam: the first plate's
    // textures, half built. SeraphimBuild writes colour only: in the fill's
    // light, no glow. UEFBuild's overlay blends alpha by its own, as D3D9
    // blends it: at alpha a = max(2 * 0.5 * (1 - 0.5), 0.25) = 0.5 over the
    // sky's 0, a * a = 0.25; weight (0.25 - 0.02) * 2 = 0.46, and in the dark
    // 1 + 5.04 * 0.46 = 3.34 times (5.84 had it written a).
    {
        const f32 seraphim = lift(190.0f, 1.0f, 0.0f);
        const f32 uef = lift(220.0f, 0.0f, 0.0f);
        t.check(seraphim > 0.98f && seraphim < 1.02f && uef > 3.0f && uef < 3.7f,
                fmt::format("Test 5: the bloom brightens a glowing plate under construction "
                            "{:.3f} times drawn by SeraphimBuild, {:.2f} by UEFBuild (3.34)",
                            seraphim, uef));
    }

    // Test 6: UEF's build slices (AlphaFade, M211g) blend colour and alpha:
    // one just made, at alpha 1, glows fully whatever its SpecTeam, as the
    // first plate: 6.04 times (in the fill's light, its albedo lit).
    {
        const f32 slice = lift(250.0f, 1.0f, 0.0f);
        t.check(
            slice > 5.6f && slice < 6.5f,
            fmt::format("Test 6: the bloom brightens a build slice {:.2f} times (6.04)", slice));
    }

    r.set_bloom_enabled(false);
    spdlog::info("Bloom test: {}/{} passed", t.pass, t.pass + t.fail);
}

} // namespace osc::test
