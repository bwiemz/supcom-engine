#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "sim/manipulator.hpp"
#include "sim/unit.hpp"

using namespace osc;
using namespace osc::sim;

TEST_CASE("Unit: begin_dying sets dying state", "[death]") {
    Unit u;
    u.set_entity_id(1);
    u.set_fraction_complete(1.0f);

    CHECK_FALSE(u.is_dying());
    CHECK_FALSE(u.is_wreckage());

    u.begin_dying(2.0f);

    CHECK(u.is_dying());
    CHECK_FALSE(u.is_wreckage());
    CHECK(u.death_timer() == 2.0f);
    CHECK(u.do_not_target());
}

TEST_CASE("Unit: tick_dying counts down timer", "[death]") {
    Unit u;
    u.begin_dying(1.0f);

    u.tick_dying(0.3f);
    CHECK(u.is_dying());
    CHECK_THAT(u.death_timer(), Catch::Matchers::WithinAbs(0.7f, 0.001f));

    u.tick_dying(0.5f);
    CHECK(u.is_dying());
    CHECK_THAT(u.death_timer(), Catch::Matchers::WithinAbs(0.2f, 0.001f));
}

TEST_CASE("Unit: dying transitions to wreckage when timer expires", "[death]") {
    Unit u;
    u.begin_dying(0.5f);

    u.tick_dying(0.6f);

    CHECK_FALSE(u.is_dying());
    CHECK(u.is_wreckage());
    CHECK(u.death_timer() == 0.0f);
}

TEST_CASE("Unit: dying clears command queue", "[death]") {
    Unit u;
    UnitCommand cmd;
    cmd.type = CommandType::Move;
    u.push_command(cmd, false);
    CHECK(u.command_queue().size() == 1);

    u.begin_dying(1.0f);
    CHECK(u.command_queue().empty());
}
