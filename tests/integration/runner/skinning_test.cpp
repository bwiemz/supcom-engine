// --skinning-test (M211h): meshes follow their bones as FA's do.
//
// FA's vertex shaders skin a vertex by its first bone alone
// (ComputeWorldMatrix(anim.y + boneIndex[0], ...)), and Moho parks a hidden
// bone's geometry out of sight (HardwareMeshBatch). A plate of the test's
// own has two bones, a root and a child its vertices name first, [1, 0, 0,
// 0], as half of retail's vertices name theirs; it stands off the test's
// ground, over the sky's clear colour, under a white fill. Test 3 uses a
// real commander.

#include "integration_tests.hpp"
#include "plate_fixtures.hpp"
#include "render_probe.hpp"

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

#include <array>
#include <cmath>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace osc::test {

namespace {

constexpr u32 kSize = 64;
constexpr const char* kRoot = "/osc_skinning_test";

} // namespace

void test_skinning(TestContext& ctx) {
    spdlog::info("=== SKINNING TEST: rigid skinning and hidden bones (M211h) ===");
    Tally t;

    OffscreenShots shots(ctx);
    if (!shots.ok()) {
        t.check(false, "renderer init (no Vulkan?)");
        return;
    }
    renderer::Renderer& r = shots.renderer();

    // The test's ground: flat, at a structure's height, in the map's corner.
    const auto probe = ctx.lua_state.do_string(
        "__osc_skinning_probe = CreateUnitHPR('ueb0101', 'ARMY_1', 32, 0, 32, 0, 0, 0)\n");
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

    const auto dir = std::filesystem::temp_directory_path() / "osc_skinning_test";
    std::filesystem::create_directories(dir);
    write_plate_scm(dir / "plate_bones.scm", 4.0f, 1, true);
    const auto flat = [](u8 r_, u8 g_, u8 b_, u8 a_) {
        return [=](int, u32, u32) { return std::array<u8, 4>{r_, g_, b_, a_}; };
    };
    write_dds(dir / "plate_normals.dds", 1, flat(0, 128, 0, 128));
    write_dds(dir / "plate_albedo.dds", 1, flat(255, 255, 255, 255));
    write_dds(dir / "plate_specteam.dds", 1, flat(0, 0, 0, 0));
    ctx.vfs.mount(kRoot, std::make_unique<vfs::DirectoryMount>(dir));

    Plate plate;
    plate.mesh = "plate_bones.scm";
    size_t next_bp = 0;
    const auto stand = [&](f32 x, f32 z, const char* name) {
        stand_plate(ctx, kRoot, kPlateBlueprints[next_bp++], plate, x, z, ground_y);
        const auto named = ctx.lua_state.do_string(fmt::format("{} = __osc_last_plate\n", name));
        if (!named) spdlog::warn("naming the plate: {}", named.error().message);
    };
    const auto frame = [&](f32 x, f32 z) {
        map::ScmapEnvironment env;
        env.terrain_shader = "TTerrain";
        env.cubemaps.emplace_back("<default>", "/textures/environment/no_such_cube.dds");
        ground.set_lighting(white_fill(), std::move(env));
        shots.recapture();
        return shots.shoot_frame(ground, x, z, 30.0f);
    };
    const auto lua = [&](const std::string& code) {
        const auto done = ctx.lua_state.do_string(code);
        if (!done) spdlog::warn("{}: {}", code, done.error().message);
    };

    r.camera().set_pitch(1.1f);
    const Rgb sky = middle(frame(400.0f, 400.0f));

    stand(100.0f, 100.0f, "__osc_turned");
    stand(130.0f, 100.0f, "__osc_hidden");
    stand(160.0f, 100.0f, "__osc_rooted");
    // Settled: a structure spawned whole puts its mesh back (retail's
    // StopBeingBuiltEffects, Aeon's two seconds on).
    for (int i = 0; i < 30; ++i) ctx.sim.tick();

    // Test 1: rigid skinning. A rotator turns the child bone 45° about y: the
    // plate, its vertices the child's, turns with it, a square to a diamond.
    // In its widest row the diamond is √2 as wide as the square was in that
    // row (one row, one depth: perspective cancels). Blended a quarter each
    // with the root, it would turn about 11° and shrink.
    {
        const ImageRGBA8 square = frame(100.0f, 100.0f);
        lua("__osc_turner = CreateRotator(__osc_turned, 1, 'y', 45, 100000)\n");
        for (int i = 0; i < 3; ++i) ctx.sim.tick();
        const ImageRGBA8 diamond = frame(100.0f, 100.0f);
        int widest_row = -1;
        int widest = 0;
        for (int row = static_cast<int>(diamond.height) / 4;
             row < static_cast<int>(diamond.height) * 3 / 4; ++row) {
            const int w = plate_width(diamond, sky, row);
            if (w > widest) {
                widest = w;
                widest_row = row;
            }
        }
        const int across = widest_row >= 0 ? plate_width(square, sky, widest_row) : 0;
        const f32 ratio = across > 0 ? static_cast<f32>(widest) / static_cast<f32>(across) : 0;
        t.check(ratio > 1.37f && ratio < 1.46f,
                fmt::format("Test 1: turned 45° by its bone, a plate is {} pixels wide at its "
                            "widest, {} square in that row ({:.3f}, 1.414 expected)",
                            widest, across, ratio));
    }

    // Test 2: hidden bones. The plate's child hidden, it's gone (the sky
    // where it stood); shown, back. Its root hidden with its children, gone
    // too.
    {
        const Rgb shown = middle(frame(130.0f, 100.0f));
        lua("__osc_hidden:HideBone(1, false)\n");
        ctx.sim.tick();
        const Rgb hidden = middle(frame(130.0f, 100.0f));
        lua("__osc_hidden:ShowBone(1, false)\n");
        ctx.sim.tick();
        const Rgb again = middle(frame(130.0f, 100.0f));
        lua("__osc_rooted:HideBone(0, true)\n");
        ctx.sim.tick();
        const Rgb rooted = middle(frame(160.0f, 100.0f));
        const Rgb white = {1, 1, 1};
        t.check(near(shown, white) && near(hidden, sky) && near(again, white) && near(rooted, sky),
                fmt::format("Test 2: a plate shows {}; its bone hidden, {} (the sky, {}); shown "
                            "again, {}; its root hidden with its children, {}",
                            show(shown), show(hidden), show(sky), show(again), show(rooted)));
    }

    // Test 3: a real commander. A UEF ACU hides its upgrades' bones as it's
    // made (Right_Upgrade, Left_Upgrade, Back_Upgrade_B01, with their
    // children): its snapshot marks them, and it draws differently with
    // them shown.
    {
        lua("__osc_acu = CreateUnitHPR('uel0001', 'ARMY_1', 190, 0, 100, 0, 0, 0)\n");
        for (int i = 0; i < 30; ++i) ctx.sim.tick();
        sim::WorldHistory history;
        history.capture(ctx.sim);
        u64 hidden = 0;
        for (const sim::EntityRecord& e : history.cur().entities)
            if (e.is_unit && e.blueprint_id == "uel0001") hidden = e.hidden_bones;
        int marked = 0;
        for (u64 bits = hidden; bits != 0; bits &= bits - 1) ++marked;
        r.camera().set_pitch(0.9f);
        const auto look = [&] {
            map::ScmapEnvironment env;
            env.terrain_shader = "TTerrain";
            env.cubemaps.emplace_back("<default>", "/textures/environment/no_such_cube.dds");
            ground.set_lighting(white_fill(), std::move(env));
            shots.recapture();
            return shots.shoot(ground, 190.0f, 100.0f, 12.0f);
        };
        const Pixels with_hidden = look();
        lua("__osc_acu:ShowBone('Right_Upgrade', true)\n"
            "__osc_acu:ShowBone('Left_Upgrade', true)\n"
            "__osc_acu:ShowBone('Back_Upgrade_B01', true)\n");
        ctx.sim.tick();
        const Pixels with_shown = look();
        int changed = 0;
        for (size_t k = 0; k < with_hidden.size() && k < with_shown.size(); ++k)
            for (int c = 0; c < 3; ++c)
                if (std::abs(with_hidden[k][c] - with_shown[k][c]) > 12.0f / 255.0f) {
                    ++changed;
                    break;
                }
        t.check(marked >= 3 && changed > 200,
                fmt::format("Test 3: a UEF commander's snapshot marks {} hidden bones; showing "
                            "its upgrades changes {} pixels",
                            marked, changed));
    }

    spdlog::info("Skinning test: {}/{} passed", t.pass, t.pass + t.fail);
}

} // namespace osc::test
