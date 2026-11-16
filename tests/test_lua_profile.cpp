// The sim's Lua profile (--profile): the functions that run the most Lua
// instructions, counted every lua_profile::kInstructions.

#include <catch2/catch_test_macros.hpp>

#include "sim/lua_profile.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

#include <cstdint>
#include <cstring>
#include <string>

namespace {

int lua_profile_probe(lua_State*) {
    return 0;
}

/// The calls the profile has timed of the engine function called `name`.
uint64_t calls_of(const std::string& name) {
    for (const auto& c : osc::sim::lua_profile::engine_calls(100000)) {
        if (c.name == name) return c.calls;
    }
    return 0;
}

} // namespace

TEST_CASE("The Lua profile times the engine functions the scripts call", "[lua][profile]") {
    // Each C function a script calls, by the name it is called by; a Lua
    // function's calls aren't counted
    lua_State* L = lua_open();
    osc::sim::lua_profile::attach(L);
    lua_register(L, "LuaProfileProbe", lua_profile_probe);
    const uint64_t global_before = calls_of("LuaProfileProbe");
    const uint64_t field_before = calls_of("lua_profile_probe_field");
    const char* code = "local t = {lua_profile_probe_field = LuaProfileProbe}\n"
                       "function lua_profile_lua_only() return 1 end\n"
                       "for i = 1, 100 do\n"
                       "  LuaProfileProbe() t.lua_profile_probe_field() lua_profile_lua_only()\n"
                       "end\n";
    REQUIRE(luaL_loadbuffer(L, code, std::strlen(code), "=calls") == 0);
    REQUIRE(lua_pcall(L, 0, 0, 0) == 0);
    CHECK(calls_of("LuaProfileProbe") - global_before == 100);
    CHECK(calls_of("lua_profile_probe_field") - field_before == 100);
    CHECK(calls_of("lua_profile_lua_only") == 0);
    lua_close(L);
}

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
