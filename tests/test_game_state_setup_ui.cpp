// SetupUI runs once the game state is the new one: what the UI's setup asks
// (SessionIsActive, read from the state) answers for the state it sets up.
// Called before, the front end after a game was set up as in a session, and
// FAF's world border looked for a map that was gone (WOPC OE-145). Moho's UI
// state, which GetCurrentUIState answers, is apart from the game state.

#include <catch2/catch_test_macros.hpp>

#include "core/game_state.hpp"
#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "sim/sim_state.hpp"
#include "ui/ui_control.hpp"

#include <string>

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

TEST_CASE("GetCurrentUIState answers Moho's UI state from boot to the front end",
          "[game_state][ui]") {
    // Moho's own UI state, not the game-state manager's (published only after
    // the boot's UI entry points ran, and "game" until then): "none" until
    // the first entry point starts, then the entry point's, set before it.
    osc::lua::LuaState lua;
    osc::sim::SimState sim(lua.raw(), nullptr);
    osc::ui::UIControlRegistry registry;
    osc::lua::register_moho_bindings(lua, sim);
    osc::lua::register_ui_bindings(lua, registry);
    osc::set_ui_state(osc::UIState::None); // the boot: userInit.lua's time
    REQUIRE(lua.do_string(R"(
        seen = {}
        modules = {['/lua/ui/uimain.lua'] = {
            StartFrontEndUI = function() table.insert(seen, GetCurrentUIState()) end,
        }}
        function import(name) return modules[name] end
        function StartGameUI() table.insert(seen, GetCurrentUIState()) end
        table.insert(seen, GetCurrentUIState())
        EngineStartFrontEndUI()
        table.insert(seen, GetCurrentUIState())
    )")
                .ok());
    osc::core::call_start_game_ui(lua.raw());
    CHECK(lua.do_string("assert(table.getn(seen) == 4, table.getn(seen))"
                        " assert(seen[1] == 'none', seen[1])"
                        " assert(seen[2] == 'frontend', seen[2])"
                        " assert(seen[3] == 'frontend', seen[3])"
                        " assert(seen[4] == 'game', seen[4])")
              .ok());
    CHECK(std::string(osc::ui_state_to_string(osc::UIState::Splash)) == "splash");
    CHECK(std::string(osc::ui_state_to_string(osc::UIState::Lobby)) == "lobby");
    osc::set_ui_state(osc::UIState::None);
}
