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

TEST_CASE("Lua profile source names survive replacing the script state",
          "[lua][profile][lifetime]") {
    const char* code = "local n = 0 for i = 1, 10000 do n = n + i end";
    for (int index = 0; index < 32; ++index) {
        lua_State* L = lua_open();
        REQUIRE(L);
        osc::sim::lua_profile::attach(L);
        const std::string source = "=replacement_" + std::to_string(100 + index);
        REQUIRE(luaL_loadbuffer(L, code, std::strlen(code), source.c_str()) == 0);
        REQUIRE(lua_pcall(L, 0, 0, 0) == 0);
        lua_close(L);
    }
    const auto counts = osc::sim::lua_profile::top(10000);
    for (int index = 0; index < 32; ++index) {
        const std::string source = "replacement_" + std::to_string(100 + index) + ":0";
        bool found = false;
        for (const auto& [where, n] : counts) {
            if (where == source) found = n > 0;
        }
        INFO(source);
        CHECK(found);
    }
}
