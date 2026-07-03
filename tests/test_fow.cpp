// Fog-of-war option tests: FogOfWar=none reveals the whole map.

#include <catch2/catch_test_macros.hpp>

#include "map/visibility_grid.hpp"
#include "sim/sim_state.hpp"

using osc::map::VisFlag;
using osc::map::VisibilityGrid;
using osc::sim::FogMode;

TEST_CASE("parse_fog_mode maps FA keys", "[fow]") {
    CHECK(osc::sim::parse_fog_mode("explored") == FogMode::Explored);
    CHECK(osc::sim::parse_fog_mode("none") == FogMode::None);
    CHECK(osc::sim::parse_fog_mode("NONE") == FogMode::None);
    CHECK(osc::sim::parse_fog_mode("something") == FogMode::Explored);
}

TEST_CASE("VisibilityGrid::reveal_all lights the whole map for an army", "[fow]") {
    VisibilityGrid grid(256, 256); // 16x16 cells
    const osc::u32 army = 0;

    // Nothing is visible before revealing.
    CHECK_FALSE(grid.has_vision(8.0f, 8.0f, army));
    CHECK_FALSE(grid.has_omni(200.0f, 40.0f, army));

    grid.reveal_all(army);

    for (osc::u32 gz = 0; gz < grid.grid_height(); ++gz) {
        for (osc::u32 gx = 0; gx < grid.grid_width(); ++gx) {
            VisFlag f = grid.get(gx, gz, army);
            CHECK(osc::map::has_flag(f, VisFlag::Vision));
            CHECK(osc::map::has_flag(f, VisFlag::Omni));
            CHECK(osc::map::has_flag(f, VisFlag::EverSeen));
        }
    }
    CHECK(grid.has_vision(200.0f, 40.0f, army));
    CHECK(grid.has_omni(8.0f, 8.0f, army));

    // Other armies are unaffected.
    CHECK_FALSE(grid.has_vision(8.0f, 8.0f, 1));
}

TEST_CASE("SimState defaults to explored fog and honors FogOfWar=none", "[fow]") {
    osc::sim::SimState sim(nullptr, nullptr);
    CHECK(sim.fog_mode() == FogMode::Explored);
    sim.set_fog_of_war("none");
    CHECK(sim.fog_mode() == FogMode::None);
    sim.set_fog_of_war("explored");
    CHECK(sim.fog_mode() == FogMode::Explored);
}
