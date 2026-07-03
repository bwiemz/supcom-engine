// TeamShareOverflow tests: resources a full teammate would waste flow to allies.

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
using osc::sim::Alliance;
using osc::sim::SimState;
using osc::sim::Unit;

namespace {

struct LuaGuard {
    lua_State* L = lua_open();
    ~LuaGuard() { lua_close(L); }
};

double mass(SimState& sim, int army) {
    return sim.get_army(army)->economy().mass.stored;
}

// A mass producer big enough to overflow full storage in one tick.
void add_producer(SimState& sim, int army, double m) {
    auto u = std::make_unique<Unit>();
    u->set_army(army);
    u->economy().production_active = true;
    u->economy().production_mass = m;
    sim.entity_registry().register_entity(std::move(u));
}

} // namespace

TEST_CASE("TeamShareOverflow routes wasted overflow to an ally", "[shareoverflow]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    sim.set_victory_condition("sandbox");
    sim.set_team_share_overflow(true);
    sim.add_army("ARMY_1", "ARMY_1");
    sim.add_army("ARMY_2", "ARMY_2");
    sim.set_alliance(0, 1, Alliance::Ally);
    sim.get_army(0)->set_stored_resources(200.0, 0.0); // full
    sim.get_army(1)->set_stored_resources(0.0, 0.0);   // empty, has room
    add_producer(sim, 0, 1000.0); // income 1000 → +100/tick, overflows by 100

    sim.tick();
    CHECK_THAT(mass(sim, 0), WithinAbs(200.0, 1e-6)); // donor stays full
    CHECK_THAT(mass(sim, 1), WithinAbs(100.0, 1e-6)); // ally gains the overflow
}

TEST_CASE("Overflow is not shared when the option is off", "[shareoverflow]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    sim.set_victory_condition("sandbox");
    sim.add_army("ARMY_1", "ARMY_1");
    sim.add_army("ARMY_2", "ARMY_2");
    sim.set_alliance(0, 1, Alliance::Ally);
    sim.get_army(0)->set_stored_resources(200.0, 0.0);
    sim.get_army(1)->set_stored_resources(0.0, 0.0);
    add_producer(sim, 0, 1000.0);

    sim.tick();
    CHECK_THAT(mass(sim, 1), WithinAbs(0.0, 1e-6)); // wasted, not shared
}

TEST_CASE("Overflow is not shared with enemies", "[shareoverflow]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    sim.set_victory_condition("sandbox");
    sim.set_team_share_overflow(true);
    sim.add_army("ARMY_1", "ARMY_1");
    sim.add_army("ARMY_2", "ARMY_2"); // enemy by default
    sim.get_army(0)->set_stored_resources(200.0, 0.0);
    sim.get_army(1)->set_stored_resources(0.0, 0.0);
    add_producer(sim, 0, 1000.0);

    sim.tick();
    CHECK_THAT(mass(sim, 1), WithinAbs(0.0, 1e-6));
}

TEST_CASE("Common Army takes precedence over overflow sharing", "[shareoverflow]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    sim.set_victory_condition("sandbox");
    sim.set_common_army(true);
    sim.set_team_share_overflow(true);
    sim.add_army("ARMY_1", "ARMY_1");
    sim.add_army("ARMY_2", "ARMY_2");
    sim.set_alliance(0, 1, Alliance::Ally);
    sim.get_army(0)->set_stored_resources(200.0, 0.0);
    sim.get_army(1)->set_stored_resources(0.0, 0.0);

    sim.tick();
    // Full pool (100/100), not the overflow path.
    CHECK_THAT(mass(sim, 0), WithinAbs(100.0, 1e-6));
    CHECK_THAT(mass(sim, 1), WithinAbs(100.0, 1e-6));
}

TEST_CASE("Overflow distribution is capped by available room", "[shareoverflow]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    sim.set_victory_condition("sandbox");
    sim.set_team_share_overflow(true);
    sim.add_army("ARMY_1", "ARMY_1");
    sim.add_army("ARMY_2", "ARMY_2");
    sim.set_alliance(0, 1, Alliance::Ally);
    sim.get_army(0)->set_stored_resources(200.0, 0.0); // full donor
    sim.get_army(1)->set_stored_resources(180.0, 0.0); // only 20 room
    add_producer(sim, 0, 1000.0); // overflow 100, but ally can take only 20

    sim.tick();
    CHECK_THAT(mass(sim, 1), WithinAbs(200.0, 1e-6)); // filled to cap, rest lost
}
