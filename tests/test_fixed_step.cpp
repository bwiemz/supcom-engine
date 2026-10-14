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

TEST_CASE("the first frame after a paused beat runs one tick, and its time restarts",
          "[fixedstep]") {
    double acc = 0.07;
    bool one_tick = true;
    CHECK(osc::session_ticks(acc, one_tick, 0.25, 0.1, 8) == 1);
    CHECK_FALSE(one_tick);
    CHECK(acc == 0.0);
    CHECK(osc::session_ticks(acc, one_tick, 0.05, 0.1, 8) == 0);
    CHECK_THAT(acc, WithinAbs(0.05, 1e-12));
}

TEST_CASE("a slow lockstep frame's time past its rounds is lost, not replayed", "[fixedstep]") {
    double acc = 0.0;
    bool one_tick = false;
    CHECK(osc::session_ticks(acc, one_tick, 1.0, 0.1, 4) == 4);
    CHECK(acc < 0.1);
    CHECK(osc::session_ticks(acc, one_tick, 1.0 / 60.0, 0.1, 4) == 1);
}

TEST_CASE("a lockstep frame after a paused beat runs one round, then the clock's", "[fixedstep]") {
    double acc = 0.05;
    bool one_tick = true;
    CHECK(osc::session_ticks(acc, one_tick, 0.35, 0.1, 4) == 1);
    CHECK_FALSE(one_tick);
    CHECK(osc::session_ticks(acc, one_tick, 1.0 / 60.0, 0.1, 4) == 0);
    CHECK(osc::session_ticks(acc, one_tick, 0.1, 0.1, 4) == 1);
}
