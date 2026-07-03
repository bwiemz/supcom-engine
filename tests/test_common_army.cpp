// Common Army tests: allied armies pool their mass/energy each tick.

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

using Catch::Matchers::WithinAbs;
using osc::sim::Alliance;
using osc::sim::BrainState;
using osc::sim::SimState;

namespace {

struct LuaGuard {
    lua_State* L = lua_open();
    ~LuaGuard() { lua_close(L); }
};

double mass(SimState& sim, int army) {
    return sim.get_army(army)->economy().mass.stored;
}
double energy(SimState& sim, int army) {
    return sim.get_army(army)->economy().energy.stored;
}

} // namespace

TEST_CASE("Common Army off: allies keep their own reserves", "[commonarmy]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    sim.set_victory_condition("sandbox");
    sim.add_army("ARMY_1", "ARMY_1");
    sim.add_army("ARMY_2", "ARMY_2");
    sim.set_alliance(0, 1, Alliance::Ally);
    sim.get_army(0)->set_stored_resources(200.0, 200.0);
    sim.get_army(1)->set_stored_resources(0.0, 0.0);

    sim.tick();
    CHECK_THAT(mass(sim, 0), WithinAbs(200.0, 1e-6));
    CHECK_THAT(mass(sim, 1), WithinAbs(0.0, 1e-6));
}

TEST_CASE("Common Army pools allied mass and energy", "[commonarmy]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    sim.set_victory_condition("sandbox");
    sim.set_common_army(true);
    sim.add_army("ARMY_1", "ARMY_1");
    sim.add_army("ARMY_2", "ARMY_2");
    sim.set_alliance(0, 1, Alliance::Ally);
    sim.get_army(0)->set_stored_resources(200.0, 160.0);
    sim.get_army(1)->set_stored_resources(0.0, 0.0);

    sim.tick();
    // Equal storage caps (200 base each) → equal split.
    CHECK_THAT(mass(sim, 0), WithinAbs(100.0, 1e-6));
    CHECK_THAT(mass(sim, 1), WithinAbs(100.0, 1e-6));
    CHECK_THAT(energy(sim, 0), WithinAbs(80.0, 1e-6));
    CHECK_THAT(energy(sim, 1), WithinAbs(80.0, 1e-6));
}

TEST_CASE("Common Army does not pool across enemies", "[commonarmy]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    sim.set_victory_condition("sandbox");
    sim.set_common_army(true);
    sim.add_army("ARMY_1", "ARMY_1");
    sim.add_army("ARMY_2", "ARMY_2"); // enemy by default
    sim.get_army(0)->set_stored_resources(200.0, 0.0);
    sim.get_army(1)->set_stored_resources(0.0, 0.0);

    sim.tick();
    CHECK_THAT(mass(sim, 0), WithinAbs(200.0, 1e-6));
    CHECK_THAT(mass(sim, 1), WithinAbs(0.0, 1e-6));
}

TEST_CASE("Common Army excludes a defeated ally", "[commonarmy]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    sim.set_victory_condition("sandbox");
    sim.set_common_army(true);
    sim.add_army("ARMY_1", "ARMY_1");
    sim.add_army("ARMY_2", "ARMY_2");
    sim.set_alliance(0, 1, Alliance::Ally);
    sim.get_army(0)->set_stored_resources(200.0, 0.0);
    sim.get_army(1)->set_stored_resources(0.0, 0.0);
    sim.get_army(1)->set_state(BrainState::Defeat);

    sim.tick();
    // Only one active member → nothing to pool; reserves stay put.
    CHECK_THAT(mass(sim, 0), WithinAbs(200.0, 1e-6));
}

TEST_CASE("Common Army equalizes three allies to a common fill ratio",
          "[commonarmy]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    sim.set_victory_condition("sandbox");
    sim.set_common_army(true);
    sim.add_army("ARMY_1", "ARMY_1");
    sim.add_army("ARMY_2", "ARMY_2");
    sim.add_army("ARMY_3", "ARMY_3");
    sim.set_alliance(0, 1, Alliance::Ally);
    sim.set_alliance(0, 2, Alliance::Ally);
    sim.set_alliance(1, 2, Alliance::Ally);
    sim.get_army(0)->set_stored_resources(150.0, 0.0);
    sim.get_army(1)->set_stored_resources(0.0, 0.0);
    sim.get_army(2)->set_stored_resources(0.0, 0.0);

    sim.tick();
    // Pool 150 over 3 equal (200) caps → 50 each.
    CHECK_THAT(mass(sim, 0), WithinAbs(50.0, 1e-6));
    CHECK_THAT(mass(sim, 1), WithinAbs(50.0, 1e-6));
    CHECK_THAT(mass(sim, 2), WithinAbs(50.0, 1e-6));
}
