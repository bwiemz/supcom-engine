#include <catch2/catch_test_macros.hpp>

#include "sim/manipulator.hpp"
#include "sim/unit.hpp"

using namespace osc;
using namespace osc::sim;

// A killed unit is dead until its script's death thread destroys it (M201b):
// no timer ends it, and it never turns into a wreck itself -- retail's
// CreateWreckageProp makes the wreck, a prop.

TEST_CASE("Unit: begin_dying makes the unit dead", "[death]") {
    Unit u;
    u.set_entity_id(1);
    u.set_fraction_complete(1.0f);
    CHECK_FALSE(u.is_dying());

    u.begin_dying();

    CHECK(u.is_dying());
    CHECK(u.do_not_target());
    CHECK_FALSE(u.is_crashing()); // on the ground: it doesn't fall
}

TEST_CASE("Unit: a dead unit stays dead until destroyed", "[death]") {
    Unit u;
    u.begin_dying();
    for (int i = 0; i < 600; ++i) u.tick_dying(0.1f, nullptr);
    CHECK(u.is_dying());
    CHECK_FALSE(u.is_wreckage());
}

TEST_CASE("Unit: dying clears orders and the economy", "[death]") {
    Unit u;
    UnitCommand cmd;
    cmd.type = CommandType::Move;
    u.push_command(cmd, false);
    CHECK(u.command_queue().size() == 1);
    u.economy().production_energy = 5;
    u.economy().production_active = true;

    u.begin_dying();

    CHECK(u.command_queue().empty());
    CHECK(u.economy().production_energy == 0);
    CHECK_FALSE(u.economy().production_active);
}
