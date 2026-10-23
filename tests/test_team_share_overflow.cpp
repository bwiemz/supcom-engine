// Overflow sharing: what a full army would waste goes to its allies, as Moho's economy does.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
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
#include <string>

using Catch::Matchers::WithinAbs;
using osc::sim::Alliance;
using osc::sim::ArmyBrain;
using osc::sim::BrainState;
using osc::sim::SimState;
using osc::sim::Unit;

namespace {

struct LuaGuard {
    lua_State* L = lua_open();
    ~LuaGuard() { lua_close(L); }
};

struct Armies {
    LuaGuard g;
    SimState sim{g.L, nullptr};

    explicit Armies(int count) {
        sim.set_victory_condition("sandbox");
        for (int a = 0; a < count; ++a) {
            const std::string name = "ARMY_" + std::to_string(a + 1);
            sim.add_army(name, name);
            sim.get_army(a)->set_extra_storage(200.0, 200.0);
        }
        sim.get_army(0)->set_stored_resources(200.0, 0.0);
    }

    ArmyBrain& army(int a) { return *sim.get_army(a); }
    double mass(int a) { return army(a).economy().mass.stored; }

    // The first army's economy of the second tick holds 100 mass over its storage.
    void overflow() {
        auto u = std::make_unique<Unit>();
        u->set_army(0);
        u->economy().production_active = true;
        u->economy().production_mass = 1000.0;
        sim.entity_registry().register_entity(std::move(u));
        sim.tick();
        sim.tick();
    }
};

} // namespace

TEST_CASE("An army's overflow goes to an ally with room unless its sharing is off",
          "[shareoverflow]") {
    const bool sharing = GENERATE(true, false);
    CAPTURE(sharing);
    Armies w(2);
    w.sim.set_alliance(0, 1, Alliance::Ally);
    if (!sharing) {
        w.army(0).set_resource_sharing(false);
    }
    w.overflow();
    CHECK_THAT(w.mass(0), WithinAbs(200.0, 1e-6));
    CHECK_THAT(w.mass(1), WithinAbs(sharing ? 100.0 : 0.0, 1e-6));
    CHECK_THAT(w.army(1).get_economy_income("MASS"), WithinAbs(sharing ? 1000.0 : 0.0, 1e-6));
    CHECK_THAT(w.army(0).get_stat("Economy_Shared_Mass"), WithinAbs(sharing ? 100.0 : 0.0, 1e-6));
    CHECK_THAT(w.army(0).get_stat("Economy_AccumExcess_Mass"),
               WithinAbs(sharing ? 0.0 : 100.0, 1e-6));
}

TEST_CASE("An ally's cut is in the receiver's Economy_Trend, beside its own production",
          "[shareoverflow]") {
    const bool sharing = GENERATE(true, false);
    const double produced = GENERATE(0.0, 30.0);
    CAPTURE(sharing, produced);
    Armies w(2);
    w.sim.set_alliance(0, 1, Alliance::Ally);
    w.army(0).set_resource_sharing(sharing);
    auto u = std::make_unique<Unit>();
    u->set_army(1);
    u->economy().production_active = true;
    u->economy().production_mass = produced;
    w.sim.entity_registry().register_entity(std::move(u));
    w.overflow();
    CHECK_THAT(w.army(1).get_stat("Economy_Trend_Mass"),
               WithinAbs(produced + (sharing ? 1000.0 : 0.0), 1e-6));
}

TEST_CASE("Each ally is offered an equal cut of what is still unplaced, up to its room",
          "[shareoverflow]") {
    Armies w(3);
    w.sim.set_alliance(0, 1, Alliance::Ally);
    w.sim.set_alliance(0, 2, Alliance::Ally);
    w.army(1).set_stored_resources(190.0, 0.0);
    w.overflow();
    CHECK_THAT(w.mass(1), WithinAbs(200.0, 1e-6));
    CHECK_THAT(w.mass(2), WithinAbs(90.0, 1e-6));
}

TEST_CASE("An ally with room in neither resource is offered no cut", "[shareoverflow]") {
    const bool energy_room = GENERATE(true, false);
    CAPTURE(energy_room);
    Armies w(3);
    w.sim.set_alliance(0, 1, Alliance::Ally);
    w.sim.set_alliance(0, 2, Alliance::Ally);
    w.army(2).set_stored_resources(200.0, energy_room ? 0.0 : 200.0);
    w.overflow();
    CHECK_THAT(w.mass(1), WithinAbs(energy_room ? 50.0 : 100.0, 1e-6));
    CHECK_THAT(w.army(0).get_stat("Economy_AccumExcess_Mass"),
               WithinAbs(energy_room ? 50.0 : 0.0, 1e-6));
}

TEST_CASE("Only an army's own allies take its overflow, a civilian ally too", "[shareoverflow]") {
    const std::string third =
        GENERATE(std::string("enemy"), std::string("ally's ally"), std::string("civilian ally"));
    CAPTURE(third);
    Armies w(3);
    w.sim.set_alliance(0, 1, Alliance::Ally);
    w.army(1).set_stored_resources(190.0, 0.0);
    if (third == "ally's ally") {
        w.sim.set_alliance(1, 2, Alliance::Ally);
    } else if (third == "civilian ally") {
        w.army(2).set_civilian(true);
        w.sim.set_alliance(0, 2, Alliance::Ally);
    }
    w.overflow();
    CHECK_THAT(w.mass(1), WithinAbs(200.0, 1e-6));
    CHECK_THAT(w.mass(2), WithinAbs(third == "civilian ally" ? 90.0 : 0.0, 1e-6));
}

TEST_CASE("Defeat stops neither an army's share nor an ally's", "[shareoverflow]") {
    const int defeated = GENERATE(0, 1);
    CAPTURE(defeated);
    Armies w(2);
    w.sim.set_alliance(0, 1, Alliance::Ally);
    w.army(defeated).set_state(BrainState::Defeat);
    w.overflow();
    CHECK_THAT(w.mass(1), WithinAbs(100.0, 1e-6));
}

TEST_CASE("Allies keep their own reserves: only overflow moves", "[shareoverflow]") {
    Armies w(2);
    w.sim.set_alliance(0, 1, Alliance::Ally);
    w.sim.tick();
    w.sim.tick();
    CHECK_THAT(w.mass(0), WithinAbs(200.0, 1e-6));
    CHECK_THAT(w.mass(1), WithinAbs(0.0, 1e-6));
}
