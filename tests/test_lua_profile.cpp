// The sim's Lua profile (--profile): the functions that run the most Lua
// instructions, counted every lua_profile::kInstructions.

#include <catch2/catch_test_macros.hpp>

#include "sim/lua_profile.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

#include <cstring>
#include <string>

TEST_CASE("The Lua profile counts the functions that run the most", "[lua][profile]") {
    lua_State* L = lua_open();
    const uint64_t before = osc::sim::lua_profile::total();
    osc::sim::lua_profile::attach(L);
    const char* code = "local x = 0\n"
                       "function spin(n)\n"
                       "  for i = 1, n do x = x + i end\n"
                       "end\n"
                       "spin(200000)\n";
    REQUIRE(luaL_loadbuffer(L, code, std::strlen(code), "=busy") == 0);
    REQUIRE(lua_pcall(L, 0, 0, 0) == 0);

    // 200,000 turns of a loop of a few instructions: hundreds of samples,
    // nearly all in spin
    const uint64_t taken = osc::sim::lua_profile::total() - before;
    CHECK(taken > 200);
    bool found = false;
    for (const auto& [where, n] : osc::sim::lua_profile::top(5)) {
        if (where == "busy:2 (spin)") {
            found = true;
            CHECK(n * 10 > taken * 9);
        }
    }
    CHECK(found);
    lua_close(L);
}
