// The benchmark's figures (M223): tick_stats's percentiles and slowest tick.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "app/bench.hpp"

#include <vector>

using osc::app::tick_stats;

TEST_CASE("A benchmark's tick figures, by nearest rank", "[bench]") {
    std::vector<double> ms;
    for (int i = 1; i <= 100; ++i) ms.push_back(i); // 1..100 ms
    // Out of order, the slowest one at tick 7
    std::swap(ms[6], ms[99]);
    const auto s = tick_stats(ms);
    CHECK(s.ticks == 100);
    CHECK(s.total_ms == Catch::Approx(5050));
    CHECK(s.mean_ms == Catch::Approx(50.5));
    CHECK(s.p50_ms == 50);
    CHECK(s.p90_ms == 90);
    CHECK(s.p99_ms == 99);
    CHECK(s.max_ms == 100);
    CHECK(s.max_tick == 7);
}

TEST_CASE("A benchmark of one tick, or none", "[bench]") {
    const auto one = tick_stats({4.0});
    CHECK(one.ticks == 1);
    CHECK(one.p50_ms == 4.0);
    CHECK(one.p99_ms == 4.0);
    CHECK(one.max_tick == 1);
    const auto none = tick_stats({});
    CHECK(none.ticks == 0);
    CHECK(none.total_ms == 0);
    CHECK(none.max_tick == 0);
}
