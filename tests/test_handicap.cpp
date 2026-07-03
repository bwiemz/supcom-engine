// Handicap tests: a lobby handicap reduces an army's resource income.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "sim/army_brain.hpp"
#include "sim/manipulator.hpp"
#include "sim/shield.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"

extern "C" {
#include <lua.h>
}

#include <memory>

using Catch::Matchers::WithinAbs;
using osc::sim::ArmyBrain;
using osc::sim::SimState;
using osc::sim::Unit;

namespace {

struct LuaGuard {
    lua_State* L = lua_open();
    ~LuaGuard() { lua_close(L); }
};

// A producer contributing fixed mass/energy income.
osc::u32 spawn_producer(SimState& sim, int army, double m, double e) {
    auto u = std::make_unique<Unit>();
    u->set_army(army);
    u->economy().production_active = true;
    u->economy().production_mass = m;
    u->economy().production_energy = e;
    return sim.entity_registry().register_entity(std::move(u));
}

} // namespace

TEST_CASE("set_handicap clamps to [0, 0.95]", "[handicap]") {
    ArmyBrain b;
    CHECK(b.handicap() == 0.0);
    b.set_handicap(-1.0);
    CHECK(b.handicap() == 0.0);
    b.set_handicap(0.25);
    CHECK(b.handicap() == 0.25);
    b.set_handicap(2.0);
    CHECK_THAT(b.handicap(), WithinAbs(0.95, 1e-9));
}

TEST_CASE("No handicap leaves income unchanged", "[handicap]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    sim.set_victory_condition("sandbox");
    sim.add_army("ARMY_1", "ARMY_1");
    spawn_producer(sim, 0, 100.0, 200.0);

    sim.tick();
    CHECK_THAT(sim.get_army(0)->get_economy_income("MASS"), WithinAbs(100.0, 1e-6));
    CHECK_THAT(sim.get_army(0)->get_economy_income("ENERGY"), WithinAbs(200.0, 1e-6));
}

TEST_CASE("Handicap scales an army's income down", "[handicap]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    sim.set_victory_condition("sandbox");
    sim.add_army("ARMY_1", "ARMY_1");
    sim.get_army(0)->set_handicap(0.25); // 25% weaker
    spawn_producer(sim, 0, 100.0, 200.0);

    sim.tick();
    CHECK_THAT(sim.get_army(0)->get_economy_income("MASS"), WithinAbs(75.0, 1e-6));
    CHECK_THAT(sim.get_army(0)->get_economy_income("ENERGY"), WithinAbs(150.0, 1e-6));
}

TEST_CASE("Handicap only affects the handicapped army", "[handicap]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    sim.set_victory_condition("sandbox");
    sim.add_army("ARMY_1", "ARMY_1");
    sim.add_army("ARMY_2", "ARMY_2");
    sim.get_army(0)->set_handicap(0.5);
    spawn_producer(sim, 0, 100.0, 0.0);
    spawn_producer(sim, 1, 100.0, 0.0);

    sim.tick();
    CHECK_THAT(sim.get_army(0)->get_economy_income("MASS"), WithinAbs(50.0, 1e-6));
    CHECK_THAT(sim.get_army(1)->get_economy_income("MASS"), WithinAbs(100.0, 1e-6));
}
