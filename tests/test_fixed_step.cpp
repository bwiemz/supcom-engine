#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/fixed_step.hpp"

using Catch::Matchers::WithinAbs;
using osc::consume_fixed_steps;

TEST_CASE("fixed steps accumulate across frames", "[fixedstep]") {
    double acc = 0.0;
    CHECK(consume_fixed_steps(acc, 0.06, 0.1, 8) == 0); // 60 ms: not yet
    CHECK(consume_fixed_steps(acc, 0.06, 0.1, 8) == 1); // 120 ms total
    CHECK_THAT(acc, WithinAbs(0.02, 1e-9));
}

TEST_CASE("a backlog beyond the cap is dropped, not carried", "[fixedstep]") {
    double acc = 0.0;
    // 10x speed on a 250 ms frame = 2.5 s of sim time = 25 ticks due.
    CHECK(consume_fixed_steps(acc, 2.5, 0.1, 8) == 8);
    CHECK(acc < 0.1); // the other 17 ticks are gone: the game slows down
    // The next ordinary frame is ordinary again.
    CHECK(consume_fixed_steps(acc, 0.1, 0.1, 8) <= 2);
}

TEST_CASE("negative frame time is ignored", "[fixedstep]") {
    double acc = 0.05;
    CHECK(consume_fixed_steps(acc, -1.0, 0.1, 8) == 0);
    CHECK_THAT(acc, WithinAbs(0.05, 1e-12));
}
