// A lobby message's Lua data (M218a): what SendData sends arrives as it
// was, and what can't travel doesn't break what can.

#include <catch2/catch_test_macros.hpp>

#include "lua/lobby_wire.hpp"
#include "lua/lua_state.hpp"

#include <string>
#include <vector>

extern "C" {
#include <lua.h>
}

namespace {

/// Encode the global `name`, decode it into `copy`, and run `check` (Lua).
bool round_trip(osc::lua::LuaState& state, const std::string& setup, const std::string& check) {
    lua_State* L = state.raw();
    if (!state.do_string(setup)) return false;
    lua_getglobal(L, "value");
    const std::vector<osc::u8> bytes = osc::lua::encode_lobby_value(L, -1);
    lua_pop(L, 1);
    const bool ok = osc::lua::push_lobby_value(L, bytes);
    lua_setglobal(L, "copy");
    auto r = state.do_string(check);
    UNSCOPED_INFO((r.ok() ? std::string() : r.error().message));
    return ok && r.ok();
}

} // namespace

TEST_CASE("Lobby data arrives as it was sent (M218a)", "[lobby][wire]") {
    osc::lua::LuaState state;
    // Retail lobby.lua's messages: a type, nested player options, numbers,
    // booleans, a list
    CHECK(round_trip(state, R"(
        value = {Type = 'SetPlayerOption', Slot = 3, Key = 'Faction', Value = 2.5,
                 Ready = false, Options = {Team = 1, Colors = {1, 2, 3}},
                 [7] = 'seven', [true] = 'yes', Big = 123456789012, Name = 'a\0b'})",
                     R"(
        assert(copy.Type == 'SetPlayerOption' and copy.Slot == 3 and copy.Value == 2.5)
        assert(copy.Ready == false and copy.Options.Team == 1)
        assert(copy.Options.Colors[3] == 3 and table.getn(copy.Options.Colors) == 3)
        assert(copy[7] == 'seven' and copy[true] == 'yes')
        assert(copy.Big == 123456789012, 'numbers exact')
        assert(copy.Name == 'a\0b' and string.len(copy.Name) == 3, 'strings whole')
    )"));
    // Not a table: a bare string or number
    CHECK(round_trip(state, "value = 'just text'", "assert(copy == 'just text')"));
    CHECK(round_trip(state, "value = -0.125", "assert(copy == -0.125)"));
}

TEST_CASE("What can't travel doesn't: functions, cycles, depth (M218a)", "[lobby][wire]") {
    osc::lua::LuaState state;
    CHECK(round_trip(state, R"(
        value = {Keep = 1, Fn = function() end, Self = nil}
        value.Self = value
        value.Deep = {}
        local t = value.Deep
        for i = 1, 40 do t.Next = {Level = i} t = t.Next end
    )",
                     R"(
        assert(copy.Keep == 1)
        assert(copy.Fn == nil, 'a function travels as nil: its pair dropped')
        assert(copy.Self == nil, 'a cycle stops')
        local depth, t = 0, copy.Deep
        while t and t.Next do depth = depth + 1 t = t.Next end
        assert(depth > 20 and depth < 40, 'cut at the depth limit: ' .. depth)
    )"));
}

TEST_CASE("Malformed lobby data decodes to nil (M218a)", "[lobby][wire]") {
    osc::lua::LuaState state;
    lua_State* L = state.raw();
    const int top = lua_gettop(L);
    const std::vector<std::vector<osc::u8>> bad = {
        {},                    // nothing
        {9},                   // no such tag
        {1, 10, 0, 0, 0, 'a'}, // a string shorter than it says
        {4, 1, 1, 0, 0, 0},    // a table cut short
        {0, 1, 2},             // a number cut short
        {3, 1, 3},             // trailing bytes
    };
    for (const auto& b : bad) {
        CHECK_FALSE(osc::lua::push_lobby_value(L, b));
        CHECK(lua_isnil(L, -1));
        lua_pop(L, 1);
    }
    CHECK(lua_gettop(L) == top); // nothing left behind
}
