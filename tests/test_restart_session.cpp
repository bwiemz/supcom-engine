// RestartSession (#811): Moho's cfunc_RestartSessionL requests the
// CreateSession frame action, which starts the session again from its launch
// info. The user state's binding had been a stand-in that did nothing, so the
// game menu's Restart Game, the score dialog's restart and the debug key did
// nothing either. The binding asks; the window launches the game in play
// again (take_restart_request); data.restart_flow plays it through.

#include <catch2/catch_test_macros.hpp>

#include "app/support.hpp"
#include "core/game_state.hpp"
#include "core/ui_registry_keys.hpp"
#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "sim/sim_state.hpp"
#include "ui/ui_control.hpp"

extern "C" {
#include <lua.h>
}

#include <optional>
#include <string>

namespace {

/// The UI's state of a game: the bindings, over `sim`.
struct UiOfGame {
    osc::lua::LuaState ui;
    osc::sim::SimState sim{ui.raw(), nullptr};
    osc::ui::UIControlRegistry registry;
    osc::GameStateManager game_state; // playing: a world session
    UiOfGame() {
        osc::lua::register_moho_bindings(ui, sim);
        osc::lua::register_ui_bindings(ui, registry);
        game_state.transition_to(osc::GameState::GAME, nullptr);
        lua_State* L = ui.raw();
        lua_pushstring(L, "__osc_game_state_mgr");
        lua_pushlightuserdata(L, &game_state);
        lua_rawset(L, LUA_REGISTRYINDEX);
    }
    /// Run `code`; its error, or "" when it ran.
    std::string run(const std::string& code) {
        auto r = ui.do_string(code);
        return r.ok() ? std::string() : r.error().message;
    }
    /// The registry's `name`, as a string ("nil", "true", "/maps/...").
    std::string key(const char* name) {
        lua_State* L = ui.raw();
        lua_pushstring(L, name);
        lua_rawget(L, LUA_REGISTRYINDEX);
        const std::string v = lua_isnil(L, -1) ? "nil"
                              : lua_type(L, -1) == LUA_TBOOLEAN
                                  ? (lua_toboolean(L, -1) ? "true" : "false")
                              : lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1)
                                                               : "?";
        lua_pop(L, 1);
        return v;
    }
    /// The world session gone, as at the front end.
    void end_session() {
        lua_State* L = ui.raw();
        lua_pushstring(L, "osc_sim_state");
        lua_pushnil(L);
        lua_rawset(L, LUA_REGISTRYINDEX);
    }
};

} // namespace

TEST_CASE("RestartSession asks for the game in play to start again, as Moho's", "[session][lua]") {
    UiOfGame g;
    // Moho's argument-count error, before anything
    CHECK(g.run("RestartSession(1)").find("expected 0 args, but got 1") != std::string::npos);
    CHECK(g.key(osc::core::kRestartRequestKey) == "nil");

    // In a session with its launch: asked, for the window to take
    CHECK(g.run("assert(SessionCanRestart() == true)").empty());
    REQUIRE(g.run("RestartSession()").empty());
    CHECK(g.key(osc::core::kRestartRequestKey) == "true");

    // No session: nothing to restart, and SessionCanRestart raises as Moho's
    lua_State* L = g.ui.raw();
    lua_pushstring(L, osc::core::kRestartRequestKey);
    lua_pushnil(L);
    lua_rawset(L, LUA_REGISTRYINDEX);
    g.end_session();
    REQUIRE(g.run("RestartSession()").empty());
    CHECK(g.key(osc::core::kRestartRequestKey) == "nil");
    CHECK(g.run("SessionCanRestart()").find("no active session.") != std::string::npos);
    CHECK(g.run("SessionCanRestart(1)").find("expected 0 args, but got 1") != std::string::npos);
}

TEST_CASE("A restart launches the game in play again, from its launch", "[session][lua]") {
    UiOfGame g;
    lua_State* L = g.ui.raw();
    const auto ask = [&] {
        lua_pushstring(L, osc::core::kRestartRequestKey);
        lua_pushboolean(L, 1);
        lua_rawset(L, LUA_REGISTRYINDEX);
    };
    osc::app::SessionLaunch launch;
    launch.scenario = "/maps/SCMP_009/SCMP_009_scenario.lua";
    launch.save_file = "/saves/Player/mid-game.SCFASave";

    // Nothing asked: nothing launched
    CHECK_FALSE(osc::app::take_restart_request(L, launch));
    CHECK(g.key("__osc_launch_requested") == "nil");

    // Asked with no launch to repeat: the request goes, and nothing launches
    ask();
    CHECK_FALSE(osc::app::take_restart_request(L, std::nullopt));
    CHECK(g.key(osc::core::kRestartRequestKey) == "nil");
    CHECK(g.key("__osc_launch_requested") == "nil");

    // Asked in a loaded game: its save loads again, as LoadSavedGame asks
    lua_pushstring(L, "__osc_launch_replay");
    lua_pushstring(L, "/replays/Player/stale.SCFAReplay"); // an earlier launch's
    lua_rawset(L, LUA_REGISTRYINDEX);
    ask();
    CHECK(osc::app::take_restart_request(L, launch));
    CHECK(g.key(osc::core::kRestartRequestKey) == "nil");
    CHECK(g.key("__osc_launch_requested") == "true");
    CHECK(g.key("__osc_launch_scenario") == launch.scenario);
    CHECK(g.key("__osc_launch_save") == launch.save_file);
    CHECK(g.key("__osc_launch_replay") == "nil");

    // A new game: its scenario alone (its setup goes to the reload)
    launch.save_file.clear();
    launch.setup = osc::sim::GameSetup{};
    ask();
    CHECK(osc::app::take_restart_request(L, launch));
    CHECK(g.key("__osc_launch_scenario") == launch.scenario);
    CHECK(g.key("__osc_launch_save") == "nil");
    CHECK(g.key("__osc_launch_replay") == "nil");
}
