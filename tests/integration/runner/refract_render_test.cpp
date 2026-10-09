// --refract-render-test (M214d): FA's refracting particles.
//
// A particle of particle.fx's REFRACT blend (BlendMode 5) shows the frame
// behind it, displaced by 0.005 of the screen times its texture's red and
// green (2x - 1), blended by its texture's alpha times its ramp's
// (WorldRefractPS). Moho draws them apart and last, over a copy of the
// finished frame (RenderRefractingEffects), whatever their SortOrder. The
// backdrop is a plate of the test's own, red one side of a line and blue
// the other, the line down the frame's middle; the particles face the
// camera, so where they sit on the screen is exact.

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

#include <lua.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace osc::test {

namespace {

constexpr const char* kRoot = "/osc_refract_test";
/// The particles' lift over what they stand over, and their size.
constexpr f32 kLift = 4.0f;
constexpr f32 kSize = 3.0f;

using Rgb = std::array<f32, 3>;

Rgb at(const ImageRGBA8& img, int x, int y) {
    x = std::clamp(x, 0, static_cast<int>(img.width) - 1);
    y = std::clamp(y, 0, static_cast<int>(img.height) - 1);
    const u8* q = &img.pixels[(static_cast<size_t>(y) * img.width + static_cast<size_t>(x)) * 4];
    return {q[0] / 255.0f, q[1] / 255.0f, q[2] / 255.0f};
}

f32 apart(const Rgb& a, const Rgb& b) {
    return (std::abs(a[0] - b[0]) + std::abs(a[1] - b[1]) + std::abs(a[2] - b[2])) / 3.0f;
}

/// How a refracting particle's window of the frame (`with`) matches the
/// frame without it (`plain`): displaced by `shift` pixels to the right and
/// blended by `alpha`, and not displaced at all.
struct Match {
    f32 displaced = 1; ///< mean error against the model
    f32 unmoved = 0;   ///< mean difference from the frame without it
};

Match match(const ImageRGBA8& with, const ImageRGBA8& plain, std::array<f32, 2> centre, f32 shift,
            f32 alpha) {
    Match m{0, 0};
    const int cx = static_cast<int>(centre[0]);
    const int cy = static_cast<int>(centre[1]);
    const int s = static_cast<int>(std::lround(shift));
    int n = 0;
    for (int y = cy - 4; y <= cy + 4; ++y)
        for (int x = cx - 8; x <= cx + 8; ++x, ++n) {
            const Rgb w = at(with, x, y);
            const Rgb p = at(plain, x, y);
            const Rgb q = at(plain, x + s, y);
            const Rgb model = {alpha * q[0] + (1 - alpha) * p[0], alpha * q[1] + (1 - alpha) * p[1],
                               alpha * q[2] + (1 - alpha) * p[2]};
            m.displaced += apart(w, model);
            m.unmoved += apart(w, p);
        }
    m.displaced /= static_cast<f32>(n);
    m.unmoved /= static_cast<f32>(n);
    return m;
}

std::string curve(const char* name, f32 y) {
    return fmt::format("    {} = {{ Keys = {{ {{ x = 0, y = {}, z = 0 }} }} }},\n", name, y);
}

} // namespace

void test_refract_render(TestContext& ctx) {
    spdlog::info("=== REFRACT TEST: FA's refracting particles (M214d) ===");
    Tally t;
    map::Terrain& terrain = *ctx.sim.terrain();
    const auto spot = quiet_spot(ctx.sim);
    if (!spot || !terrain.has_water()) {
        t.check(false, "dry ground 60 from every unit, and water");
        return;
    }
    const f32 x0 = spot->x;
    const f32 z0 = spot->z;
    const f32 ground = terrain.get_terrain_height(x0, z0);

    const auto dir = std::filesystem::temp_directory_path() / "osc_refract_test";
    std::filesystem::create_directories(dir);
    write_plate_scm(dir / "plate.scm", 16.0f);
    write_dds(dir / "plate_normals.dds", 1,
              [](int, u32, u32) { return std::array<u8, 4>{0, 128, 0, 128}; });
    write_dds(dir / "spec_none.dds", 1, [](int, u32, u32) { return std::array<u8, 4>{}; });
    // Red, then blue, from the plate's middle on.
    write_dds(
        dir / "split.dds", 1,
        [](int, u32 column, u32) {
            return column < 32 ? std::array<u8, 4>{220, 30, 30, 255}
                               : std::array<u8, 4>{30, 30, 220, 255};
        },
        64, 4);
    // Red 1 and green a half: displaced 0.005 of the screen across, not down.
    write_dds(dir / "bend.dds", 1,
              [](int, u32, u32) { return std::array<u8, 4>{255, 128, 0, 255}; });
    write_dds(dir / "bend_half.dds", 1,
              [](int, u32, u32) { return std::array<u8, 4>{255, 128, 0, 128}; });
    write_dds(dir / "white.dds", 1,
              [](int, u32, u32) { return std::array<u8, 4>{255, 255, 255, 255}; });
    write_dds(dir / "white_half.dds", 1,
              [](int, u32, u32) { return std::array<u8, 4>{255, 255, 255, 128}; });
    const auto emitter_bp = [&](const char* file, const char* texture, const char* extra,
                                const char* ramp = "white.dds") {
        std::ofstream(dir / file) << fmt::format(
            "EmitterBlueprint {{\n"
            "    Lifetime = -1, Repeattime = 10, LODCutoff = 500, Blendmode = 5,\n"
            "    EmitIfVisible = false, InterpolateEmission = false, SnapToWaterline = false,\n"
            "    LocalVelocity = false,\n"
            "    Texture = '{0}/{1}', RampTexture = '{0}/{4}',\n"
            "{2}{3}}}\n",
            kRoot, texture, extra,
            curve("EmitRateCurve", 1) + curve("LifetimeCurve", 50) +
                curve("StartSizeCurve", kSize) + curve("EndSizeCurve", kSize),
            ramp);
    };
    emitter_bp("bend.bp", "bend.dds", "");
    emitter_bp("half.bp", "bend_half.dds", "", "white_half.dds");
    emitter_bp("under.bp", "bend.dds", "    SortOrder = -102,\n");
    ctx.vfs.mount(kRoot, std::make_unique<vfs::DirectoryMount>(dir));

    // The backdrop: a prop plate over the spot, the line along z at x0.
    std::string prop_bp;
    {
        const std::vector<std::string> world = sim::world_blueprints(ctx.sim);
        for (const auto* e : ctx.sim.blueprint_store()->get_all(blueprints::BlueprintType::Prop))
            if (e->id.find("/env/") != std::string::npos &&
                std::find(world.begin(), world.end(), e->id) == world.end()) {
                prop_bp = e->id;
                break;
            }
    }
    // Nothing else there: the spot's trees sway from frame to frame.
    run_lua(ctx, fmt::format("for _, p in GetReclaimablesInRect(Rect({}, {}, {}, {})) or {{}} do\n"
                             "  if IsProp(p) then p:Destroy() end\n"
                             "end\n",
                             x0 - 40.0f, z0 - 40.0f, x0 + 40.0f, z0 + 40.0f));
    ctx.sim.tick();
    Plate plate;
    plate.shader = "NormalMappedAlpha";
    plate.albedo = "split.dds";
    plate.specteam = "spec_none.dds";
    stand_plate(ctx, kRoot, prop_bp, plate, x0, z0, ground, true);
    run_lua(ctx, "__osc_rf_plate = __osc_last_plate\n");
    const f32 top = ground + 0.5f;

    OffscreenShots shots(ctx);
    if (!shots.ok()) {
        t.check(false, "renderer init (no Vulkan?)");
        return;
    }
    renderer::Renderer& r = shots.renderer();
    (void)shots.shoot(terrain, x0, z0, 45.0f);
    for (const char* tex :
         {"split.dds", "bend.dds", "bend_half.dds", "white.dds", "white_half.dds"})
        (void)r.texture_cache().get_blocking(fmt::format("{}/{}", kRoot, tex));
    const auto step = [&] {
        ctx.sim.tick();
        shots.recapture();
        shots.redraw();
    };
    for (int i = 0; i < 2; ++i) step();
    const ImageRGBA8 plain = shots.grab();
    const f32 shift = 0.005f * static_cast<f32>(plain.width);

    // Emitters over the plate's line, `kLift` up, at z0 + dz.
    const auto emit = [&](const char* global, const char* bp, f32 dz) {
        run_lua(ctx, fmt::format("local e = CreateEmitterAtEntity(__osc_rf_plate, 1, '{}/{}')\n"
                                 "e:OffsetEmitter(0, {}, {})\n"
                                 "{} = e\n",
                                 kRoot, bp, kLift, dz, global));
    };
    emit("__osc_rf_a", "bend.bp", -8.0f);
    emit("__osc_rf_b", "half.bp", 0.0f);
    emit("__osc_rf_c", "under.bp", 8.0f);
    for (int i = 0; i < 3; ++i) step();
    const ImageRGBA8 with = shots.grab();
    const auto centre = [&](f32 dz) {
        const auto s = screen_of(r, {x0, top + kLift, z0 + dz});
        return s ? *s : std::array<f32, 2>{0, 0};
    };

    // Test 1: the particle shows the frame behind it moved 0.005 of the
    // screen across: the plate's far colour spills over the line.
    {
        const Match m = match(with, plain, centre(-8.0f), shift, 1.0f);
        t.check(m.displaced < 0.01f && m.unmoved > 0.04f,
                fmt::format("Test 1: within a refracting particle, the frame {:.1f} pixels on: "
                            "off by {:.4f}; unmoved it differs {:.3f}",
                            shift, m.displaced, m.unmoved));
    }

    // Test 2: at its texture's alpha a half and its ramp's a half, a quarter
    // of the moved frame over the frame, for each of its particles there
    // (they don't move: each emitted lies over the last).
    {
        run_lua(ctx, "__osc_rf_id = __osc_rf_b._c_effect_id\n");
        lua_pushstring(ctx.L, "__osc_rf_id");
        lua_rawget(ctx.L, LUA_GLOBALSINDEX);
        const u32 id = lua_isnumber(ctx.L, -1) ? static_cast<u32>(lua_tonumber(ctx.L, -1)) : 0;
        lua_pop(ctx.L, 1);
        const auto& drawn = r.particle_system().drawn();
        const auto n = std::count_if(drawn.begin(), drawn.end(),
                                     [id](const auto& p) { return p.effect_id == id; });
        const f32 each = (128.0f / 255.0f) * (128.0f / 255.0f);
        const f32 alpha = 1.0f - std::pow(1.0f - each, static_cast<f32>(n));
        const Match m = match(with, plain, centre(0.0f), shift, alpha);
        t.check(n > 0 && m.displaced < 0.01f && m.unmoved > 0.02f,
                fmt::format("Test 2: {} particles at alpha a quarter each, {:.2f} moved: off by "
                            "{:.4f}; unmoved it differs {:.3f}",
                            n, alpha, m.displaced, m.unmoved));
    }

    // Test 3: a SortOrder below -101, which puts other particles under the
    // water, refracts the same: Moho draws them all apart, none in the pass
    // under the water.
    {
        const Match m = match(with, plain, centre(8.0f), shift, 1.0f);
        run_lua(ctx, "__osc_rf_id = __osc_rf_c._c_effect_id\n");
        lua_pushstring(ctx.L, "__osc_rf_id");
        lua_rawget(ctx.L, LUA_GLOBALSINDEX);
        const u32 id = lua_isnumber(ctx.L, -1) ? static_cast<u32>(lua_tonumber(ctx.L, -1)) : 0;
        lua_pop(ctx.L, 1);
        const auto& drawn = r.particle_system().drawn();
        const auto under = std::count_if(drawn.begin(), drawn.end(), [id](const auto& p) {
            return p.effect_id == id && p.under_water;
        });
        t.check(m.displaced < 0.01f && m.unmoved > 0.04f && under == 0,
                fmt::format("Test 3: at SortOrder -102, off by {:.4f}; unmoved it differs {:.3f}; "
                            "{} under the water",
                            m.displaced, m.unmoved, under));
    }

    // Test 4: drawn last, over the water: one standing over open water shows
    // the water's surface moved, the finished frame (its waves held still).
    {
        (void)shots.shoot(terrain, 32.0f, 32.0f, 70.0f);
        const renderer::WaterRenderer& water = r.water_renderer();
        std::optional<std::array<f32, 2>> deep;
        const std::vector<u8>& map = water.water_map();
        for (u32 hz = 16; hz + 16 < water.water_map_height() && !deep; ++hz)
            for (u32 hx = 16; hx + 16 < water.water_map_width() && !deep; ++hx) {
                bool ok = true;
                for (u32 z = hz - 4; z <= hz + 4 && ok; ++z)
                    for (u32 x = hx - 4; x <= hx + 4 && ok; ++x) {
                        const size_t i = (static_cast<size_t>(z) * water.water_map_width() + x) * 4;
                        ok = map[i + 2] == 0 && map[i + 1] > 150;
                    }
                if (ok)
                    deep = std::array<f32, 2>{static_cast<f32>(hx * 2), static_cast<f32>(hz * 2)};
            }
        Match m;
        if (deep) {
            const f32 w = terrain.water_elevation();
            map::ScmapWater still = terrain.water();
            const map::ScmapWater original = still;
            for (auto& layer : still.normal_movement) layer[0] = layer[1] = 0.0f;
            terrain.set_water(still, terrain.water_masks(), terrain.water_abyss_elevation());
            const Spot over{(*deep)[0], (*deep)[1]};
            (void)spawn_unit(ctx, "__osc_rf_sub", "uel0105", "ARMY_1", over, 0.0f);
            (void)shots.shoot(terrain, over.x, over.z, 45.0f);
            for (int i = 0; i < 2; ++i) step();
            const ImageRGBA8 still_plain = shots.grab();
            // kLift over the water, from wherever the unit is.
            run_lua(ctx,
                    fmt::format("local e = CreateEmitterAtEntity(__osc_rf_sub, 1, '{}/bend.bp')\n"
                                "e:OffsetEmitter(0, {} - __osc_rf_sub:GetPosition()[2], 0)\n",
                                kRoot, w + kLift));
            for (int i = 0; i < 3; ++i) step();
            const ImageRGBA8 still_with = shots.grab();
            const auto s = screen_of(r, {over.x, w + kLift, over.z});
            m = match(still_with, still_plain, s ? *s : std::array<f32, 2>{0, 0}, shift, 1.0f);
            terrain.set_water(original, terrain.water_masks(), terrain.water_abyss_elevation());
        }
        t.check(deep && m.displaced < 0.02f && m.displaced < 0.5f * m.unmoved,
                fmt::format("Test 4: over the water, off the moved water by {:.4f}; unmoved it "
                            "differs {:.3f}",
                            m.displaced, m.unmoved));
    }

    spdlog::info("Refract test: {}/{} passed", t.pass, t.pass + t.fail);
}

} // namespace osc::test
