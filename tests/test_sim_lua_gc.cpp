// The sim forces a full collection of its Lua state on Moho's schedule
// (Sim::AdvanceBeat: every 70 ticks). Its timing is part of the game --
// weak tables such as trash bags lose what it frees -- so it must fall on
// the same ticks everywhere.

#include <catch2/catch_test_macros.hpp>

#include "lua/lua_state.hpp"
#include "sim/sim_state.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

using osc::sim::SimState;

namespace {

// Tick until the sim has run `tick` ticks.
void tick_to(SimState& sim, osc::u32 tick) {
    while (sim.tick_count() < tick) sim.tick();
}

// Leave about 1 MB of garbage, and keep Lua's own collector (which runs
// when the heap doubles) out of the way, so only the sim's collects it.
void make_garbage(lua_State* L) {
    lua_setgcthreshold(L, 1 << 30);
    REQUIRE(lua_dostring(L, "local s = string.rep('x', 1024 * 1024)") == 0);
}

} // namespace

TEST_CASE("The sim collects its Lua garbage every 70 ticks, as Moho does", "[sim][lua]") {
    osc::lua::LuaState lua;
    lua_State* L = lua.raw();
    SimState sim(L, nullptr);
    STATIC_REQUIRE(SimState::LUA_GC_PERIOD_TICKS == 70);

    tick_to(sim, 49);
    make_garbage(L);
    const int with_garbage = lua_getgccount(L);
    sim.tick(); // tick 50: not a collection tick
    CHECK(lua_getgccount(L) >= with_garbage);

    tick_to(sim, 69);
    CHECK(lua_getgccount(L) >= with_garbage);
    sim.tick();                                    // tick 70
    CHECK(lua_getgccount(L) < with_garbage - 900); // the 1 MB string is gone

    tick_to(sim, 139);
    make_garbage(L);
    const int again = lua_getgccount(L);
    sim.tick(); // tick 140
    CHECK(lua_getgccount(L) < again - 900);
}
