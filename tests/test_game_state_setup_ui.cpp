// SetupUI runs once the game state is the new one: what the UI's setup asks
// (SessionIsActive, read from the state) answers for the state it sets up.
// Called before, the front end after a game was set up as in a session, and
// FAF's world border looked for a map that was gone (WOPC OE-145).

#include <catch2/catch_test_macros.hpp>

#include "core/game_state.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace {

/// State() -> the manager's state, as a number (the upvalue is the manager)
int state_of(lua_State* L) {
    const auto* mgr =
        static_cast<const osc::GameStateManager*>(lua_touserdata(L, lua_upvalueindex(1)));
    lua_pushnumber(L, static_cast<lua_Number>(mgr->current()));
    return 1;
}

double seen_state(lua_State* L) {
    lua_pushstring(L, "seen");
    lua_rawget(L, LUA_GLOBALSINDEX);
    const double seen = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : -1;
    lua_pop(L, 1);
    return seen;
}

} // namespace

TEST_CASE("SetupUI sees the state it sets up", "[game_state]") {
    lua_State* L = lua_open();
    osc::GameStateManager mgr;
    lua_pushstring(L, "State");
    lua_pushlightuserdata(L, &mgr);
    lua_pushcclosure(L, state_of, 1);
    lua_rawset(L, LUA_GLOBALSINDEX);
    REQUIRE(luaL_loadbuffer(L, "function SetupUI() seen = State() end", 37, "test") == 0);
    REQUIRE(lua_pcall(L, 0, 0, 0) == 0);

    mgr.transition_to(osc::GameState::GAME, L);
    CHECK(seen_state(L) == static_cast<double>(osc::GameState::GAME));
    mgr.transition_to(osc::GameState::FRONT_END, L);
    CHECK(seen_state(L) == static_cast<double>(osc::GameState::FRONT_END));
    lua_close(L);
}
