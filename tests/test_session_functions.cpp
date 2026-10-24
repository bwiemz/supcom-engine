// The session's functions in the user state, as Moho's binders check their
// calls: the argument count first (kLuaExpectedArgsWarning, with each
// function's own usage), as faf-re's Sim.cpp and SessionStartup.cpp show.

#include <catch2/catch_test_macros.hpp>

#include "app/app_internal.hpp"
#include "core/game_state.hpp"
#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "sim/sim_state.hpp"
#include "ui/ui_control.hpp"

extern "C" {
#include <lua.h>
}

#include <string>
#include <utility>
#include <vector>

namespace {

/// A game's user state: the UI's bindings over a sim, in a session (the game
/// state manager in its registry, playing).
struct UserState {
    osc::lua::LuaState lua;
    osc::sim::SimState sim{lua.raw(), nullptr};
    osc::ui::UIControlRegistry registry;
    osc::GameStateManager mgr;

    UserState() {
        osc::lua::register_moho_bindings(lua, sim);
        osc::lua::register_ui_bindings(lua, registry);
        osc::app::register_session_ui_globals(lua);
        lua_State* L = lua.raw();
        lua_pushstring(L, "__osc_game_state_mgr");
        lua_pushlightuserdata(L, &mgr);
        lua_rawset(L, LUA_REGISTRYINDEX);
        mgr.transition_to(osc::GameState::GAME, nullptr);
    }

    /// The error `code` raises, or "" when it ran.
    std::string error_of(const std::string& code) {
        auto r = lua.do_string(code);
        return r.ok() ? std::string() : r.error().message;
    }
};

} // namespace

TEST_CASE("Zero-argument session queries raise Moho's count error when given an argument",
          "[session][args]") {
    UserState s;
    // Each with the usage Moho registers for it (the retail exe's strings).
    const std::vector<std::pair<std::string, std::string>> queries = {
        {"GameTick", "Get the current game time in ticks. The game time is the simulation time, "
                     "that stops when the game is paused."},
        {"GetGameTimeSeconds", "float GetGameTimeSeconds() - returns game time in seconds"},
        {"GetFocusArmy", "GetFocusArmy()"},
        {"IsObserver", "IsObserver()"},
        {"GetSimRate", "number GetSimRate()"},
        {"GetGameSpeed", "Return the current game speed"},
        {"GetCurrentUIState", "state GetCurrentUIState() - returns 'splash', 'frontend' or 'game' "
                              "depending on the current state of the ui"},
        {"SessionIsActive", "Return true iff there is a session currently running"},
        {"SessionIsReplay", "Return true iff the active session is a replay session."},
        {"SessionIsMultiplayer", "Return true iff the active session is a multiplayer session."},
        {"SessionGetLocalCommandSource",
         "Return the local command source.  Returns 0 if the local client can't issue commands."},
        {"SessionGetCommandSourceNames", "Return a table of  command sources."},
    };
    for (const auto& [name, usage] : queries) {
        INFO(name);
        CHECK(s.error_of(name + "()").empty());
        CHECK(s.error_of(name + "(1)").ends_with(usage + "\n  expected 0 args, but got 1"));
        CHECK(s.error_of(name + "(nil, 2)").ends_with(usage + "\n  expected 0 args, but got 2"));
    }
}
