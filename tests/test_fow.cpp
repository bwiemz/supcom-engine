// Fog-of-war option tests: FogOfWar=none reveals the whole map.

#include <catch2/catch_test_macros.hpp>

#include "map/intel_grid.hpp"
#include "sim/sim_state.hpp"

using osc::map::IntelGrids;
using osc::map::IntelLayer;
using osc::sim::FogMode;

TEST_CASE("parse_fog_mode maps FA keys", "[fow]") {
    CHECK(osc::sim::parse_fog_mode("explored") == FogMode::Explored);
    CHECK(osc::sim::parse_fog_mode("none") == FogMode::None);
    CHECK(osc::sim::parse_fog_mode("NONE") == FogMode::None);
    CHECK(osc::sim::parse_fog_mode("something") == FogMode::Explored);
}

TEST_CASE("Without fog of war an army keeps no sight grids, as Moho's", "[fow]") {
    const IntelGrids fogged(256, 256, 2, true);
    CHECK(fogged.grid(0, IntelLayer::Vision).width() == 128);
    CHECK(fogged.grid(1, IntelLayer::Water).width() == 64);
    const IntelGrids clear(256, 256, 2, false);
    CHECK(clear.grid(0, IntelLayer::Vision).empty());
    CHECK(clear.grid(1, IntelLayer::Water).empty());
    // Radar, sonar, omni and the counter grids still are
    CHECK(clear.grid(0, IntelLayer::Radar).width() == 64);
    CHECK(clear.grid(1, IntelLayer::VisionCounter).width() == 64);
}

TEST_CASE("SimState defaults to explored fog and honors FogOfWar=none", "[fow]") {
    osc::sim::SimState sim(nullptr, nullptr);
    CHECK(sim.fog_mode() == FogMode::Explored);
    sim.set_fog_of_war("none");
    CHECK(sim.fog_mode() == FogMode::None);
    sim.set_fog_of_war("explored");
    CHECK(sim.fog_mode() == FogMode::Explored);
}
