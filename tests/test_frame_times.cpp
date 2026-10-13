#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/fixed_step.hpp"
#include "core/frame_times.hpp"

using Catch::Matchers::WithinAbs;
using osc::RollingFrameTimes;

TEST_CASE("the frame times' median is Moho's", "[frametimes]") {
    RollingFrameTimes times;
    CHECK(times.median() == 0.0);
    times.push(0.3);
    CHECK(times.median() == 0.3);
    times.push(0.1);
    CHECK(times.median() == 0.3); // of two, the upper
    times.push(0.2);
    CHECK(times.median() == 0.2);
}

TEST_CASE("the ring holds the last nine frame times", "[frametimes]") {
    RollingFrameTimes times;
    for (int i = 1; i <= 10; ++i) times.push(i);
    // 1 has gone: 2..10, whose middle is 6
    CHECK(times.median() == 6.0);
    for (int i = 0; i < 4; ++i) times.push(100.0);
    // 6..10 and four of 100: the middle is 10
    CHECK(times.median() == 10.0);
}

TEST_CASE("a ring full of no time drops the first long frames", "[frametimes]") {
    // As a game the engine loaded before its first frame starts: its first
    // renders are long, and Moho's median drops them among the loading
    // screen's frames.
    auto times = RollingFrameTimes::full_of(0.0);
    CHECK(times.median() == 0.0);
    times.push(0.126);
    times.push(0.157);
    CHECK(times.median() == 0.0);
    for (int i = 0; i < 3; ++i) times.push(0.004);
    // Five frames of time among nine: the middle is an ordinary one
    CHECK(times.median() == 0.004);
}

TEST_CASE("a hitch leaves the session's ticks behind the wall clock", "[frametimes]") {
    // Three seconds at 60 frames a second, one of them half a second long,
    // as Moho's session sees them: the median of the last frame times.
    constexpr double kFrame = 1.0 / 60.0;
    constexpr double kTick = 0.1;
    RollingFrameTimes times;
    double accumulator = 0.0;
    double wall = 0.0;
    int ticks = 0;
    while (wall < 3.0) {
        const double frame = (wall > 1.0 && wall < 1.0 + kFrame) ? 0.5 : kFrame;
        wall += frame;
        times.push(frame);
        ticks += osc::consume_fixed_steps(accumulator, times.median(), kTick, 8);
    }
    // The wall clock's ticks would be 30; the hitch's half second is lost.
    CHECK(ticks == 25);

    // Steady frames keep up.
    RollingFrameTimes steady;
    accumulator = 0.0;
    wall = 0.0;
    ticks = 0;
    for (int frame = 0; frame < 180; ++frame) {
        wall += kFrame;
        steady.push(kFrame);
        ticks += osc::consume_fixed_steps(accumulator, steady.median(), kTick, 8);
    }
    CHECK_THAT(wall, WithinAbs(3.0, 1e-9));
    CHECK(ticks >= 29);
}
