// --yaw-only-test: the Aeon Torrent (XAS0306) fires its missile racks. Their
// pitch is fixed at 55 degrees, and their blueprint's YawOnlyOnTarget makes
// them on target once their heading is (Moho's CAimManipulator), so they
// fire at a target at any range they reach. The engine had required the
// pitch too, and the racks never fired.

#include "integration_tests.hpp"
#include "intel_probe.hpp"
#include "render_probe.hpp"

#include "lua/lua_state.hpp"
#include "map/terrain.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <optional>
#include <set>

namespace osc::test {

void test_yaw_only(TestContext& ctx) {
    spdlog::info("=== YAW ONLY TEST: the Torrent's fixed-pitch racks fire ===");
    Tally t;
    const map::Terrain* terrain = ctx.sim.terrain();
    std::optional<Spot> sea;
    if (terrain && terrain->has_water()) {
        const auto w = static_cast<i32>(terrain->map_width());
        const auto h = static_cast<i32>(terrain->map_height());
        for (i32 z = 40; z < h - 40 && !sea; z += 8)
            for (i32 x = 40; x < w - 140 && !sea; x += 8)
                if (terrain->get_terrain_height(static_cast<f32>(x), static_cast<f32>(z)) <
                        terrain->water_elevation() - 8 &&
                    terrain->get_terrain_height(static_cast<f32>(x + 100), static_cast<f32>(z)) >
                        terrain->water_elevation() + 1) {
                    sea = Spot{static_cast<f32>(x), static_cast<f32>(z)};
                }
    }
    if (!sea) {
        t.check(false, "deep water with land 100 east on the map");
        return;
    }
    // The Torrent, and an enemy power generator ashore 100 to its side (its racks
    // reach 25-200, each over 165 degrees either side of its own centre).
    const u32 torrent = spawn_unit(ctx, "__osc_yo_torrent", "xas0306", "ARMY_1", *sea);
    spawn_unit(ctx, "__osc_yo_target", "ueb1101", "ARMY_2", {sea->x + 100, sea->z});
    run_lua(ctx, "__osc_yo_target:SetCanTakeDamage(false)");
    std::set<u32> missiles;
    for (int i = 0; i < 600; ++i) {
        ctx.sim.tick();
        ctx.sim.entity_registry().for_each([&](const sim::Entity& e) {
            if (e.is_projectile() && e.army() == 0) missiles.insert(e.entity_id());
        });
    }
    t.check(ctx.sim.entity_registry().find(torrent) != nullptr && missiles.size() >= 2,
            fmt::format("the Torrent fires its missile racks: {} missiles in 60 seconds",
                        missiles.size()));
    spdlog::info("=== YAW ONLY TEST: {} passed, {} failed ===", t.pass, t.fail);
}

} // namespace osc::test
