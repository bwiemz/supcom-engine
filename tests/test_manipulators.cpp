#include <catch2/catch_test_macros.hpp>

#include "sim/manipulator.hpp"

TEST_CASE("A slider at a negative speed is at its goal at once, at none it stays", "[manip]") {
    osc::sim::SlideManipulator slider;
    slider.set_goal(0, -2, 0);
    slider.tick(0.1f);
    CHECK_FALSE(slider.is_at_goal());

    slider.set_speed(-1);
    slider.tick(0.1f);
    CHECK(slider.is_at_goal());
    CHECK(slider.current().y == -2.0f);
}
