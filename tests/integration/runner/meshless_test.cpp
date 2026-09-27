// --meshless-test: entities without a mesh draw as FA draws them.
//
// FA draws nothing for a projectile without a mesh: most carry only effects
// (a NullShell, like the ACU's warp-in, which lives 32 seconds). The engine
// drew each as a cube in its army's colour -- the blue box over every ACU
// at the start of a game. A unit without a mesh, a gap in the data or the
// engine, still stands in as a cube.

#include "integration_tests.hpp"
#include "render_probe.hpp"

#include "lua/lua_state.hpp"
#include "map/terrain.hpp"
#include "sim/entity.hpp"
#include "sim/manipulator.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"

#include <spdlog/spdlog.h>

#include <cmath>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace osc::test {

namespace {

struct Cube {
    f32 x = 0, z = 0;
};

/// The cubes the last frame drew (the frame dump's "cube x y z ..." lines).
std::vector<Cube> cubes_drawn(renderer::Renderer& renderer, size_t& meshes) {
    std::ostringstream out;
    renderer.dump_frame(out);
    std::istringstream in(out.str());
    std::vector<Cube> cubes;
    meshes = 0;
    bool in_units = false;
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line[0] == '[') {
            in_units = line == "[units]";
            continue;
        }
        if (!in_units) continue;
        if (line.rfind("mesh ", 0) == 0) ++meshes;
        if (line.rfind("cube ", 0) == 0) {
            Cube c;
            f32 y = 0;
            std::istringstream fields(line.substr(5));
            fields >> c.x >> y >> c.z;
            cubes.push_back(c);
        }
    }
    return cubes;
}

bool cube_at(const std::vector<Cube>& cubes, f32 x, f32 z) {
    for (const Cube& c : cubes) {
        if (std::abs(c.x - x) < 0.5f && std::abs(c.z - z) < 0.5f) return true;
    }
    return false;
}

} // namespace

void test_meshless(TestContext& ctx) {
    spdlog::info("=== MESHLESS TEST: entities without a mesh ===");
    Tally t;
    const u32 acu_id = army_acu_id(ctx.sim, 0);
    auto* acu = ctx.sim.entity_registry().find(acu_id);
    if (!acu) {
        t.check(false, "ARMY_1's ACU");
        return;
    }
    const f32 ax = acu->position().x;
    const f32 az = acu->position().z;

    // The ACU's warp-in effect, as retail's commander script makes it.
    const auto made = ctx.lua_state.do_string(
        "local acu = GetEntityById(__osc_test_acu_id(1))\n"
        "acu:CreateProjectile('/effects/entities/UnitTeleport01/UnitTeleport01_proj.bp', "
        "0, 1.35, 0, nil, nil, nil)\n");
    if (!made) spdlog::warn("CreateProjectile failed: {}", made.error().message);

    // A unit whose blueprint isn't there, and so has no mesh, beside it.
    const f32 ux = ax + 6.0f;
    const f32 uz = az;
    {
        auto unit = std::make_unique<sim::Unit>();
        unit->set_blueprint_id("osc_test_no_such_unit");
        unit->set_army(0);
        unit->set_position({ux, ctx.sim.terrain()->get_terrain_height(ux, uz), uz});
        unit->set_max_health(100.0f);
        unit->set_health(100.0f);
        ctx.sim.entity_registry().register_entity(std::move(unit));
    }
    ctx.sim.tick();

    const sim::Entity* warp = nullptr;
    ctx.sim.entity_registry().for_each([&](const sim::Entity& e) {
        if (e.is_projectile() && !e.destroyed() &&
            e.blueprint_id().find("unitteleport01") != std::string::npos) {
            warp = &e;
        }
    });
    t.check(warp != nullptr, "Test 1: the warp-in effect carrier exists");

    OffscreenShots shots(ctx);
    if (!shots.ok()) {
        t.check(false, "Tests 2-3: renderer init (no Vulkan?)");
        return;
    }
    (void)shots.shoot(*ctx.sim.terrain(), ax, az, 30.0f);
    size_t meshes = 0;
    const auto cubes = cubes_drawn(shots.renderer(), meshes);

    // Test 2: the effect carrier draws nothing (the ACU's mesh draws).
    if (warp) {
        const f32 wx = warp->position().x;
        const f32 wz = warp->position().z;
        t.check(!cube_at(cubes, wx, wz) && meshes > 0,
                fmt::format("Test 2: nothing drawn for the warp-in at ({:.1f}, {:.1f}); {} "
                            "cubes, {} meshes in the frame",
                            wx, wz, cubes.size(), meshes));
    }

    // Test 3: a unit without a mesh still stands in as a cube.
    t.check(cube_at(cubes, ux, uz),
            fmt::format("Test 3: a cube for the unit without a mesh at ({:.1f}, {:.1f})", ux, uz));

    spdlog::info("Meshless test: {}/{} passed", t.pass, t.pass + t.fail);
}

} // namespace osc::test
