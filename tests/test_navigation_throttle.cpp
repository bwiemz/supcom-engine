// The pathfinder serves at most Pathfinder::MAX_REQUESTS_PER_TICK requests per
// tick. Orders issued beyond that budget must wait for a later tick -- they
// used to be dropped (Move) or to spin the sim forever (Patrol).

#include <catch2/catch_test_macros.hpp>

#include "map/heightmap.hpp"
#include "map/pathfinder.hpp"
#include "map/pathfinding_grid.hpp"
#include "map/terrain.hpp"
#include "sim/army_brain.hpp"
#include "sim/manipulator.hpp"
#include "sim/shield.hpp"
#include "sim/navigator.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/unit_command.hpp"

extern "C" {
#include <lua.h>
}

#include <memory>
#include <vector>

using osc::sim::CommandType;
using osc::sim::SimState;
using osc::sim::Unit;
using osc::sim::UnitCommand;

namespace {

struct LuaGuard {
    lua_State* L = lua_open();
    ~LuaGuard() { lua_close(L); }
};

constexpr osc::u32 kMapSize = 128;

/// Flat, dry 128x128 map with a pathfinding grid.
void make_flat_world(SimState& sim) {
    std::vector<osc::u16> heights((kMapSize + 1) * (kMapSize + 1), 1000);
    osc::map::Heightmap hm(kMapSize, kMapSize, 1.0f / 128.0f, std::move(heights));
    sim.set_terrain(std::make_unique<osc::map::Terrain>(std::move(hm), 0.0f, false));
    sim.build_pathfinding_grid();
}

Unit* spawn_land_unit(SimState& sim, osc::f32 x, osc::f32 z) {
    auto u = std::make_unique<Unit>();
    u->set_army(0);
    u->set_max_speed(5.0f);
    u->set_position({x, 0.0f, z});
    auto* raw = u.get();
    sim.entity_registry().register_entity(std::move(u));
    return raw;
}

UnitCommand order(CommandType type, osc::f32 x, osc::f32 z) {
    UnitCommand c;
    c.type = type;
    c.target_pos = {x, 0.0f, z};
    return c;
}

} // namespace

TEST_CASE("Move orders beyond the per-tick path budget are delayed not dropped",
          "[nav][m183]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    make_flat_world(sim);
    REQUIRE(sim.pathfinder() != nullptr);

    // More movers than one tick's path budget, all ordered in the same tick.
    const int movers = osc::map::Pathfinder::MAX_REQUESTS_PER_TICK + 4;
    std::vector<Unit*> units;
    for (int i = 0; i < movers; ++i) {
        auto* u = spawn_land_unit(sim, 10.0f, 10.0f + 4.0f * static_cast<osc::f32>(i));
        u->push_command(order(CommandType::Move, 110.0f, 10.0f + 4.0f * i), true);
        units.push_back(u);
    }

    for (int t = 0; t < 5; ++t) sim.tick();

    // Every unit made progress: none had its order silently discarded.
    for (int i = 0; i < movers; ++i) {
        INFO("unit " << i);
        CHECK(units[i]->position().x > 11.0f); // started at x = 10
    }
}

TEST_CASE("Patrol with an exhausted path budget does not hang the tick",
          "[nav][m183]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    make_flat_world(sim);

    // Patrol cycles its waypoint to the back of the queue on arrival. When
    // throttled it used to treat "no path yet" as arrival and loop forever.
    const int patrollers = osc::map::Pathfinder::MAX_REQUESTS_PER_TICK + 4;
    std::vector<Unit*> units;
    for (int i = 0; i < patrollers; ++i) {
        auto* u = spawn_land_unit(sim, 10.0f, 10.0f + 4.0f * static_cast<osc::f32>(i));
        u->push_command(order(CommandType::Patrol, 100.0f, 10.0f + 4.0f * i), true);
        units.push_back(u);
    }

    for (int t = 0; t < 5; ++t) sim.tick(); // must return

    for (int i = 0; i < patrollers; ++i) {
        INFO("unit " << i);
        CHECK(units[i]->position().x > 11.0f); // started at x = 10
    }
}

namespace {

/// 128x128 map split in two by a cliff wall along x = 60..64.
void make_walled_world(SimState& sim) {
    std::vector<osc::u16> heights((kMapSize + 1) * (kMapSize + 1), 1000);
    for (osc::u32 z = 0; z <= kMapSize; ++z) {
        for (osc::u32 x = 60; x <= 64; ++x) {
            heights[z * (kMapSize + 1) + x] = 60000; // ~470 units tall
        }
    }
    osc::map::Heightmap hm(kMapSize, kMapSize, 1.0f / 128.0f, std::move(heights));
    sim.set_terrain(std::make_unique<osc::map::Terrain>(std::move(hm), 0.0f, false));
    sim.build_pathfinding_grid();
}

} // namespace

TEST_CASE("an unreachable goal yields a partial path to the closest point",
          "[nav][m183]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    make_walled_world(sim);
    const auto* pf = sim.pathfinder();
    REQUIRE(pf != nullptr);
    pf->reset_request_count();

    auto result = pf->find_path(20.0f, 64.0f, 100.0f, 64.0f, "Land");
    REQUIRE(result.found);
    CHECK(result.partial);
    REQUIRE_FALSE(result.waypoints.empty());
    // The path stops on the near side of the wall, as close to it as it gets.
    for (const auto& wp : result.waypoints) CHECK(wp.x < 60.0f);
    CHECK(result.waypoints.back().x > 50.0f);

    // A reachable goal is not partial.
    auto ok = pf->find_path(20.0f, 64.0f, 40.0f, 30.0f, "Land");
    REQUIRE(ok.found);
    CHECK_FALSE(ok.partial);
}

TEST_CASE("a unit ordered across a cliff stops at it instead of clipping through",
          "[nav][m183]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    make_walled_world(sim);

    auto* u = spawn_land_unit(sim, 40.0f, 64.0f);
    u->push_command(order(CommandType::Move, 100.0f, 64.0f), true);
    for (int t = 0; t < 200; ++t) sim.tick();

    CHECK(u->position().x < 60.0f);  // never entered or crossed the wall
    CHECK(u->position().x > 50.0f);  // but got as close as it could
    CHECK(u->command_queue().empty()); // and the order finished
}

TEST_CASE("repeated requests for an unreachable goal do not re-run A* every tick",
          "[nav][m183]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    make_walled_world(sim);
    const auto* pf = sim.pathfinder();

    // A unit already standing at the closest reachable point to a goal behind
    // the wall (the end of a partial path): nothing better exists, so the
    // request fails.
    const osc::sim::Vector3 goal{100.0f, 0.0f, 64.0f};
    pf->reset_request_count();
    auto partial = pf->find_path(20.0f, 64.0f, goal.x, goal.z, "Land");
    REQUIRE(partial.partial);
    const osc::sim::Vector3 here = partial.waypoints.back();

    osc::sim::Navigator nav;
    pf->reset_request_count();
    nav.set_goal(goal, pf, here, "Land");
    REQUIRE_FALSE(nav.busy());
    REQUIRE(pf->requests_this_tick() == 1);

    // Chase-style handlers re-request whenever !is_moving(), i.e. every tick.
    for (int i = 0; i < 10; ++i) nav.set_goal(goal, pf, here, "Land");
    CHECK(pf->requests_this_tick() == 1); // no new searches

    // A different goal, or a changed position, is a new question.
    nav.set_goal({20.0f, 0.0f, 64.0f}, pf, here, "Land");
    CHECK(pf->requests_this_tick() == 2);
}

TEST_CASE("a failed request is retried eventually (terrain can open up)",
          "[nav][m183]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    make_walled_world(sim);
    const auto* pf = sim.pathfinder();
    const osc::sim::Vector3 goal{100.0f, 0.0f, 64.0f};
    pf->reset_request_count();
    const osc::sim::Vector3 here =
        pf->find_path(20.0f, 64.0f, goal.x, goal.z, "Land").waypoints.back();

    osc::sim::Navigator nav;
    pf->reset_request_count();
    nav.set_goal(goal, pf, here, "Land");
    REQUIRE_FALSE(nav.busy());
    for (int i = 0; i < osc::sim::Navigator::FAILED_PATH_RETRY_CALLS; ++i) {
        pf->reset_request_count();
        nav.set_goal(goal, pf, here, "Land");
    }
    CHECK(pf->requests_this_tick() == 1); // the retry happened
}

TEST_CASE("reachability is answered from connectivity, without the path budget",
          "[nav][reach]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    make_walled_world(sim);
    const auto* pf = sim.pathfinder();

    // Spend the whole per-tick movement budget first: queries must not care.
    pf->reset_request_count();
    for (int i = 0; i < osc::map::Pathfinder::MAX_REQUESTS_PER_TICK; ++i)
        pf->increment_request_count();

    CHECK(pf->reachable(20.0f, 64.0f, 40.0f, 30.0f, "Land"));   // same side
    CHECK_FALSE(pf->reachable(20.0f, 64.0f, 100.0f, 64.0f, "Land")); // across
    CHECK(pf->reachable(100.0f, 20.0f, 110.0f, 100.0f, "Land")); // far side
    CHECK(pf->reachable(20.0f, 64.0f, 100.0f, 64.0f, "Air"));    // air: always

    // Across the wall, the best point is the near side of it, level with the goal.
    const auto across = pf->reachability(20.0f, 64.0f, 100.0f, 64.0f, "Land");
    CHECK(across.best_x > 50.0f);
    CHECK(across.best_x < 60.0f);
    CHECK(across.best_z > 60.0f);
    CHECK(across.best_z < 68.0f);
    // The queries spent none of the budget.
    CHECK(pf->requests_this_tick() == osc::map::Pathfinder::MAX_REQUESTS_PER_TICK);
}

TEST_CASE("reachability follows obstacles that open and close", "[nav][reach]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    make_flat_world(sim);
    auto* grid = sim.pathfinding_grid();
    const auto* pf = sim.pathfinder();

    REQUIRE(pf->reachable(10.0f, 64.0f, 110.0f, 64.0f, "Land"));
    // A line of buildings across the whole map splits it...
    grid->mark_obstacle(64.0f, 64.0f, 4.0f, 130.0f);
    CHECK_FALSE(pf->reachable(10.0f, 64.0f, 110.0f, 64.0f, "Land"));
    // ...until they are gone.
    grid->clear_obstacle(64.0f, 64.0f, 4.0f, 130.0f);
    CHECK(pf->reachable(10.0f, 64.0f, 110.0f, 64.0f, "Land"));
}
