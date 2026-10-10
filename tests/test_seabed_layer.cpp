// A unit's layer over water, as Moho's CUnitMotion::UpdateCurrentLayer sets
// it (faf-re) each tick it moves: amphibious and land units walk the seabed
// under the water (on the terrain), hover and floating units ride the water
// (at its surface), each from where the ground is under the water's surface
// plus the blueprint's LayerChangeOffsetHeight.

#include <catch2/catch_test_macros.hpp>

#include "map/heightmap.hpp"
#include "map/terrain.hpp"
#include "sim/manipulator.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/unit_command.hpp"
#include "sim/weapon.hpp"

extern "C" {
#include <lua.h>
}

#include <cmath>
#include <memory>
#include <string>
#include <vector>

using osc::f32;
using osc::sim::CommandType;
using osc::sim::SimState;
using osc::sim::Unit;
using osc::sim::UnitCommand;
using osc::sim::Vector3;

namespace {

struct LuaGuard {
    lua_State* L = lua_open();
    ~LuaGuard() { lua_close(L); }
};

constexpr f32 kWater = 5.0f;
constexpr f32 kLand = 1000.0f / 128.0f; // 7.8

/// 128 x 128: land at 7.8 east of x = 64, a lake bed at 0 west of x = 40,
/// a ramp between, and the water at 5 (so it begins at x = 55.4).
void lake(SimState& sim) {
    std::vector<osc::u16> heights(129 * 129, 1000);
    for (size_t z = 0; z <= 128; ++z)
        for (size_t x = 0; x < 64; ++x)
            heights[z * 129 + x] = x < 40 ? 0 : static_cast<osc::u16>((x - 40) * 1000 / 24);
    osc::map::Heightmap hm(128, 128, 1.0f / 128.0f, std::move(heights));
    sim.set_terrain(std::make_unique<osc::map::Terrain>(std::move(hm), kWater, true));
    sim.build_pathfinding_grid();
    sim.add_army("ARMY_1", "ARMY_1");
    sim.set_fog_of_war("none");
    sim.set_victory_condition("sandbox");
}

Unit* mover(SimState& sim, const std::string& motion_type, f32 x, f32 z) {
    auto u = std::make_unique<Unit>();
    u->set_army(0);
    u->set_max_speed(5.0f);
    u->set_motion_type(motion_type);
    u->set_position({x, kLand, z});
    u->set_max_health(100.0f);
    u->set_health(100.0f);
    Unit::Drive drive;
    drive.max_accel = 50.0f;
    u->set_drive(drive);
    auto* raw = u.get();
    sim.entity_registry().register_entity(std::move(u));
    return raw;
}

UnitCommand move_to(Vector3 at) {
    UnitCommand c;
    c.type = CommandType::Move;
    c.target_pos = at;
    return c;
}

void ticks(SimState& sim, int n) {
    for (int t = 0; t < n; ++t) sim.tick();
}

} // namespace

TEST_CASE("An amphibious unit walks the seabed under the water, and comes back onto land",
          "[seabed]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    lake(sim);
    Unit* u = mover(sim, "RULEUMT_Amphibious", 70.0f, 64.0f);
    u->push_command(move_to({30.0f, 0.0f, 64.0f}), true);
    ticks(sim, 120);
    REQUIRE(std::abs(u->position().x - 30.0f) < 1.0f);
    CHECK(u->layer() == "Seabed");
    CHECK(u->position().y < 0.5f); // on the bed, not the water at 5

    u->push_command(move_to({75.0f, 0.0f, 64.0f}), true);
    ticks(sim, 120);
    REQUIRE(std::abs(u->position().x - 75.0f) < 1.0f);
    CHECK(u->layer() == "Land");
    CHECK(std::abs(u->position().y - kLand) < 0.01f);
}

TEST_CASE("Hover and floating units ride the water's surface on the Water layer", "[seabed]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    lake(sim);
    Unit* hover = mover(sim, "RULEUMT_Hover", 70.0f, 40.0f);
    Unit* floater = mover(sim, "RULEUMT_AmphibiousFloating", 70.0f, 90.0f);
    hover->push_command(move_to({30.0f, 0.0f, 40.0f}), true);
    floater->push_command(move_to({30.0f, 0.0f, 90.0f}), true);
    ticks(sim, 120);
    for (const Unit* u : {hover, floater}) {
        REQUIRE(std::abs(u->position().x - 30.0f) < 1.0f);
        CHECK(u->layer() == "Water");
        CHECK(std::abs(u->position().y - kWater) < 0.01f);
    }

    hover->push_command(move_to({75.0f, 0.0f, 40.0f}), true);
    ticks(sim, 120);
    CHECK(hover->layer() == "Land");
}

TEST_CASE("A unit standing still keeps its layer", "[seabed]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    lake(sim);
    // Put down in the lake (a script's Warp), still: Moho looks again only
    // once it moves.
    Unit* u = mover(sim, "RULEUMT_Amphibious", 30.0f, 64.0f);
    ticks(sim, 5);
    CHECK(u->layer() == "Land");
    u->push_command(move_to({28.0f, 0.0f, 64.0f}), true);
    ticks(sim, 5);
    CHECK(u->layer() == "Seabed");
}

TEST_CASE("LayerChangeOffsetHeight moves where the water begins for a land footprint", "[seabed]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    lake(sim);
    // A deep wader: under water only where the ground is 6 below its surface,
    // never in this lake (5 deep).
    Unit* deep = mover(sim, "RULEUMT_Amphibious", 70.0f, 30.0f);
    deep->set_layer_change_offset(-6.0f);
    deep->push_command(move_to({30.0f, 0.0f, 30.0f}), true);
    // The default, -0.1: under water as soon as the ground is.
    Unit* plain = mover(sim, "RULEUMT_Amphibious", 70.0f, 100.0f);
    plain->push_command(move_to({30.0f, 0.0f, 100.0f}), true);
    ticks(sim, 120);
    CHECK(deep->layer() == "Land");
    CHECK(plain->layer() == "Seabed");
}

TEST_CASE("Where a unit stands: the bed for those that walk it, else the surface", "[seabed]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    lake(sim);
    const auto* terrain = sim.terrain();
    Unit* amph = mover(sim, "RULEUMT_Amphibious", 10.0f, 10.0f);
    Unit* land = mover(sim, "RULEUMT_Land", 10.0f, 20.0f);
    Unit* hover = mover(sim, "RULEUMT_Hover", 10.0f, 30.0f);
    Unit* ship = mover(sim, "RULEUMT_Water", 10.0f, 40.0f);
    ship->set_layer("Water");
    CHECK(amph->ground_y(terrain, 30.0f, 30.0f) == 0.0f);
    CHECK(land->ground_y(terrain, 30.0f, 30.0f) == 0.0f);
    CHECK(hover->ground_y(terrain, 30.0f, 30.0f) == kWater);
    CHECK(ship->ground_y(terrain, 30.0f, 30.0f) == kWater);
    // On land, all the same.
    CHECK(amph->ground_y(terrain, 100.0f, 30.0f) == hover->ground_y(terrain, 100.0f, 30.0f));
}
