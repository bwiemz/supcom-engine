// The session's functions in the user state, as Moho's binders check their
// calls: the argument count first (kLuaExpectedArgsWarning, with each
// function's own usage), then, for those that read the session, that one is
// active (WLD_GetActiveSession), as faf-re's Sim.cpp and SessionStartup.cpp
// show.

#include <catch2/catch_test_macros.hpp>

#include "app/app_internal.hpp"
#include "core/game_state.hpp"
#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "lua/sim_bindings.hpp"
#include "lua/user_bindings.hpp"
#include "sim/sim_state.hpp"
#include "ui/ui_control.hpp"

extern "C" {
#include <lua.h>
}

#include <string>
#include <utility>
#include <vector>

namespace {

/// A game's user state: the UI's bindings over a sim, as App::init_ui_state
/// registers them, in a session (the game state manager in its registry,
/// playing).
struct UserState {
    osc::lua::LuaState lua;
    osc::sim::SimState sim{lua.raw(), nullptr};
    osc::ui::UIControlRegistry registry;
    osc::GameStateManager mgr;

    UserState() {
        osc::lua::register_moho_bindings(lua, sim);
        osc::lua::register_ui_bindings(lua, registry);
        osc::lua::register_user_bindings(lua);
        osc::app::register_session_ui_globals(lua);
        osc::lua::register_category_bindings(lua);
        lua_State* L = lua.raw();
        lua_pushstring(L, "__osc_game_state_mgr");
        lua_pushlightuserdata(L, &mgr);
        lua_rawset(L, LUA_REGISTRYINDEX);
        mgr.transition_to(osc::GameState::GAME, nullptr);
    }

    /// No sim for the UI, as at the front end or while a game loads.
    void drop_sim() {
        lua_State* L = lua.raw();
        lua_pushstring(L, "osc_sim_state");
        lua_pushlightuserdata(L, nullptr);
        lua_rawset(L, LUA_REGISTRYINDEX);
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

namespace {

/// The user functions Moho refuses with no session, a call each takes, and
/// the error each raises then (faf-re Sim.cpp's kNoActiveSessionText,
/// kNoSessionStartedText and the functions' own texts).
constexpr std::pair<const char*, const char*> kSessionGated[] = {
    {"GameTick()", "No active session"},
    {"GameTime()", "No active session"},
    {"GetGameTimeSeconds()", "No active session"},
    {"GetGameTime()", "No session started."},
    {"GetSimRate()", "No session started."},
    {"FormatTime(61)", "No session started."},
    {"IsObserver()", "No session started."},
    {"GetArmiesTable()", "No session started."},
    {"SetFocusArmy(-1)", "No session started."},
    {"GetMouseWorldPos()", "No session started."},
    {"SessionGetScenarioInfo()", "no active session."},
    {"SessionIsReplay()", "no active session."},
    {"DecreaseBuildCountInQueue(1, 1)", "No active session!"},
    {"IncreaseBuildCountInQueue(1, 1)", "No active session!"},
    {"ParseEntityCategory('ALLUNITS')", "ParseEntityCategory: no session loaded"},
    {"EntityCategoryContains(nil, 'uel0001')", "EntityCategoryContains: no session loaded"},
    {"EntityCategoryFilterDown(nil, {})", "EntityCategoryFilterDown: no session loaded"},
    {"EntityCategoryFilterOut(nil, {})", "EntityCategoryFilterOut: no session loaded"},
};

} // namespace

TEST_CASE("Every Moho-gated user function raises Moho's text with no session",
          "[session][frontend]") {
    UserState s;
    // In a game, each answers.
    for (const auto& [call, error] : kSessionGated) {
        INFO(call);
        CHECK(s.error_of(call).empty());
    }

    // The front end: no game, and no sim for the UI.
    s.mgr.transition_to(osc::GameState::FRONT_END, nullptr);
    s.drop_sim();
    CHECK(s.error_of("assert(SessionIsActive() == false)").empty());
    for (const auto& [call, error] : kSessionGated) {
        INFO(call);
        CHECK(s.error_of(call).ends_with(error));
    }
    // The count is checked first, as Moho's binders do.
    CHECK(s.error_of("GameTick(1)").ends_with("expected 0 args, but got 1"));
    CHECK(s.error_of("FormatTime()").ends_with("expected 1 args, but got 0"));
    CHECK(s.error_of("SetFocusArmy(1, 2)").ends_with("expected 1 args, but got 2"));
    // A script's pcall sees it, as a front-end check of Moho's would.
    CHECK(s.error_of(R"(
        local ok, err = pcall(GameTick)
        assert(not ok and string.find(err, 'No active session', 1, true), tostring(err))
    )")
              .empty());
}

TEST_CASE("A game's session is active while it loads its sim and after its score screen ends it",
          "[session]") {
    UserState s;
    // Loading, before the sim is made (the launch's loading screen): none,
    // as SessionIsActive answers.
    s.mgr.transition_to(osc::GameState::LOADING, nullptr);
    s.drop_sim();
    CHECK(s.error_of("assert(SessionIsActive() == false)").empty());
    CHECK(s.error_of("GameTick()").ends_with("No active session"));

    // With its sim: one.
    UserState loaded;
    loaded.mgr.transition_to(osc::GameState::LOADING, nullptr);
    CHECK(loaded.error_of("assert(SessionIsActive() == true)").empty());
    CHECK(loaded.error_of("assert(GameTick() == 0)").empty());

    // The score screen's SessionEndGame stops the sim, but the session stays
    // active until the game is left (Moho's help: "The session says active"):
    // retail's score.lua reads GetArmiesTable and SessionIsReplay after it.
    UserState ended;
    REQUIRE(ended.error_of("SessionEndGame()").empty());
    CHECK(ended.error_of("assert(SessionIsActive() == false)").empty());
    CHECK(ended.error_of("assert(type(GetArmiesTable().armiesTable) == 'table')").empty());
    CHECK(ended.error_of("assert(SessionIsReplay() == false)").empty());
}
