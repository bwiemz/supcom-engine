// Tests for the deterministic sim RNG. Lockstep multiplayer relies on every
// client producing the same random stream from the same seed.

#include <catch2/catch_test_macros.hpp>

#include "sim/sim_random.hpp"

#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "lua/sim_bindings.hpp"
#include "sim/sim_state.hpp"

extern "C" {
#include <lua.h>
}

#include <cstdint>
#include <string>

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

// --- Distributions (M196): our own integer arithmetic, so every platform
// draws the same values (std:: distributions differ between standard
// libraries even from the same engine and seed). ------------------------------

TEST_CASE("SimRandom doubles are in [0, 1) and reproducible", "[simrand]") {
    SimRandom r(2024);
    for (int i = 0; i < 10000; ++i) {
        const double v = r.next_double();
        REQUIRE(v >= 0.0);
        REQUIRE(v < 1.0);
    }
    SimRandom a(5), b(5);
    for (int i = 0; i < 100; ++i) REQUIRE(a.next_double() == b.next_double());
}

TEST_CASE("SimRandom integers cover an inclusive range", "[simrand]") {
    SimRandom r(77);
    bool seen[7] = {};
    for (int i = 0; i < 2000; ++i) {
        const auto v = r.next_int(1, 6);
        REQUIRE(v >= 1);
        REQUIRE(v <= 6);
        seen[v] = true;
    }
    for (int face = 1; face <= 6; ++face) CHECK(seen[face]);
    CHECK(r.next_int(3, 3) == 3);
    // The widest range doesn't overflow.
    const auto wide = r.next_int(INT64_MIN, INT64_MAX);
    (void)wide;
    for (int i = 0; i < 100; ++i) {
        const auto v = r.next_int(-5, 5);
        REQUIRE(v >= -5);
        REQUIRE(v <= 5);
    }
}

TEST_CASE("SimRandom's stream is pinned", "[simrand]") {
    // Changing these values changes every game and every replay: do it only
    // on purpose.
    SimRandom r(42);
    CHECK(r.next_u64() == 0xBDD732262FEB6E95ull);
    SimRandom d(42);
    CHECK(d.next_double() == 0.7415648787718233);
    SimRandom n(42);
    CHECK(n.next_int(1, 100) == 14);
}

// --- Scripts' randomness (M196) ----------------------------------------------

namespace {

// A sim Lua state wired as a session wires it.
struct SimLua {
    osc::lua::LuaState lua;
    osc::sim::SimState sim{lua.raw(), nullptr};
    explicit SimLua(osc::u64 seed) {
        osc::lua::register_moho_bindings(lua, sim);
        osc::lua::register_sim_bindings(lua, sim);
        sim.set_seed(seed);
    }
    double number(const char* code) {
        auto r = lua.do_string(code);
        INFO((r.ok() ? std::string() : r.error().message));
        REQUIRE(r.ok());
        const double v = lua_tonumber(lua.raw(), -1);
        lua_pop(lua.raw(), 1);
        return v;
    }
};

} // namespace

TEST_CASE("Sim scripts' Random draws from the session's seeded stream", "[simrand][lua]") {
    SimLua a(7), b(7);
    for (int i = 0; i < 20; ++i) {
        const double x = a.number("return Random()");
        REQUIRE(x >= 0.0);
        REQUIRE(x < 1.0);
        REQUIRE(x == b.number("return Random()"));
    }
    SimRandom expect(7);
    SimLua c(7);
    CHECK(c.number("return Random()") == expect.next_double());
    CHECK(c.number("return Random(1, 6)") == static_cast<double>(expect.next_int(1, 6)));

    for (int i = 0; i < 200; ++i) {
        const double one = a.number("return Random(5)"); // math.random's 1..n
        REQUIRE(one >= 1);
        REQUIRE(one <= 5);
        const double two = a.number("return Random(-3, 3)");
        REQUIRE(two >= -3);
        REQUIRE(two <= 3);
    }
    CHECK(a.number("return Random(4, 4)") == 4);
}

TEST_CASE("Sim math.random is the session's stream; the UI's is its own", "[simrand][lua]") {
    SimLua sim(11);
    SimRandom expect(11);
    CHECK(sim.number("return math.random()") == expect.next_double());
    CHECK(sim.number("return math.random(10)") == static_cast<double>(expect.next_int(1, 10)));
    CHECK(sim.number("return math.random(20, 30)") == static_cast<double>(expect.next_int(20, 30)));

    // math.randomseed reseeds it, as a script may to replay a sequence.
    sim.number("math.randomseed(123) return 0");
    SimRandom reseeded(123);
    CHECK(sim.number("return math.random()") == reseeded.next_double());

    // The UI state's math.random never moves the sim's stream.
    const osc::u64 before = sim.sim.random().state();
    osc::lua::LuaState ui;
    REQUIRE(ui.do_string("local x = math.random() local y = math.random(5)").ok());
    CHECK(sim.sim.random().state() == before);
}
