// Tests for the deterministic sim RNG. Lockstep multiplayer relies on every
// client producing the same random stream from the same seed.

#include <catch2/catch_test_macros.hpp>

#include "sim/sim_random.hpp"

using osc::sim::SimRandom;

TEST_CASE("SimRandom is deterministic for a given seed", "[simrand]") {
    SimRandom a(12345), b(12345);
    for (int i = 0; i < 100; ++i) REQUIRE(a.next_u64() == b.next_u64());
}

TEST_CASE("SimRandom diverges for different seeds", "[simrand]") {
    SimRandom a(1), b(2);
    bool any_diff = false;
    for (int i = 0; i < 8; ++i) any_diff |= (a.next_u64() != b.next_u64());
    REQUIRE(any_diff);
}

TEST_CASE("SimRandom range stays in bounds and reseed resets the stream",
          "[simrand]") {
    SimRandom r(7);
    for (int i = 0; i < 1000; ++i) {
        osc::f32 v = r.range(-3.0f, 3.0f);
        REQUIRE(v >= -3.0f);
        REQUIRE(v < 3.0f);
    }
    SimRandom a(99), b(1);
    b.seed(99);
    REQUIRE(a.next_u64() == b.next_u64());
}
