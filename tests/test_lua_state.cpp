#include <catch2/catch_test_macros.hpp>
#include "lua/lua_state.hpp"

extern "C" {
#include <lua.h>
}

using namespace osc::lua;

TEST_CASE("LuaState creation and basic execution", "[lua]") {
    LuaState state;
    REQUIRE(state.raw() != nullptr);

    auto result = state.do_string("x = 1 + 2");
    REQUIRE(result.ok());
}

TEST_CASE("LuaState register and call C function", "[lua]") {
    LuaState state;

    static int called = 0;
    state.register_function("test_fn", [](lua_State* L) -> int {
        called++;
        lua_pushnumber(L, 42);
        return 1;
    });

    called = 0;
    auto result = state.do_string("result = test_fn()");
    REQUIRE(result.ok());
    CHECK(called == 1);
}

TEST_CASE("LuaState != operator (LuaPlus patch)", "[lua]") {
    LuaState state;

    auto result = state.do_string(R"(
        x = 5
        if x != 3 then
            result = true
        else
            result = false
        end
    )");
    REQUIRE(result.ok());

    lua_getglobal(state.raw(), "result");
    CHECK(lua_toboolean(state.raw(), -1) == 1);
    lua_pop(state.raw(), 1);
}

TEST_CASE("LuaState continue statement (LuaPlus patch)", "[lua]") {
    LuaState state;

    auto result = state.do_string(R"(
        sum = 0
        for i = 1, 10 do
            if i == 5 then continue end
            sum = sum + i
        end
    )");
    REQUIRE(result.ok());

    lua_getglobal(state.raw(), "sum");
    // Sum of 1..10 minus 5 = 55 - 5 = 50
    CHECK(lua_tonumber(state.raw(), -1) == 50);
    lua_pop(state.raw(), 1);
}

TEST_CASE("LuaState hex literals (LuaPlus patch)", "[lua]") {
    LuaState state;

    auto result = state.do_string("x = 0xFF");
    REQUIRE(result.ok());

    lua_getglobal(state.raw(), "x");
    CHECK(lua_tonumber(state.raw(), -1) == 255);
    lua_pop(state.raw(), 1);
}

TEST_CASE("LuaState per-type metatables (LuaPlus patch)", "[lua]") {
    LuaState state;

    // getmetatable(nil) should return a table
    auto result = state.do_string(R"(
        local mt = getmetatable(nil)
        mt_type = type(mt)
    )");
    REQUIRE(result.ok());

    lua_getglobal(state.raw(), "mt_type");
    CHECK(std::string(lua_tostring(state.raw(), -1)) == "table");
    lua_pop(state.raw(), 1);

    // Full metacleanup pattern from config.lua
    result = state.do_string(R"(
        local function metacleanup(obj)
            local name = type(obj)
            local mmt = {
                __newindex = function(_, key, _)
                    error(("Attempt to set attribute '%s' on %s"):format(tostring(key), name), 2)
                end,
            }
            setmetatable(getmetatable(obj), mmt)
        end
        metacleanup(nil)
        metacleanup(false)
        metacleanup(0)
        metacleanup('')
        cleanup_ok = true
    )");
    REQUIRE(result.ok());

    lua_getglobal(state.raw(), "cleanup_ok");
    CHECK(lua_toboolean(state.raw(), -1) == 1);
    lua_pop(state.raw(), 1);
}

TEST_CASE("LuaState table iteration without pairs()", "[lua]") {
    LuaState state;

    auto result = state.do_string(R"(
        t = {a = 1, b = 2, c = 3}
        sum = 0
        for k, v in t do
            sum = sum + v
        end
    )");
    REQUIRE(result.ok());

    lua_getglobal(state.raw(), "sum");
    CHECK(lua_tonumber(state.raw(), -1) == 6);
    lua_pop(state.raw(), 1);
}
