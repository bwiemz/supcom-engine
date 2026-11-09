// The session's functions in the user state, as Moho's binders check their
// calls: the argument count first (kLuaExpectedArgsWarning, with each
// function's own usage), then, for those that read the session, that one is
// active (WLD_GetActiveSession), as faf-re's Sim.cpp and SessionStartup.cpp
// show.

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>

#include "app/app_internal.hpp"
#include "core/game_state.hpp"
#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "lua/sim_bindings.hpp"
#include "lua/user_bindings.hpp"
#include "sim/lua_sim.hpp"
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
    void drop_sim() { osc::sim::set_sim(lua.raw(), nullptr); }

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

TEST_CASE("GetGameTime and FormatTime give Moho's hours, minutes and seconds", "[session][time]") {
    UserState s;
    CHECK(s.error_of("assert(GetGameTime() == '00:00:00', GetGameTime())").empty());
    for (int i = 0; i < 375; ++i) {
        s.sim.tick();
    }
    CHECK(s.error_of("assert(GetGameTime() == '00:00:37', GetGameTime())").empty());

    constexpr std::pair<const char*, const char*> kFormatted[] = {
        {"3725", "01:02:05"}, {"59.9", "00:00:59"}, {"90061.7", "25:01:01"}, {"-5", "00:00:-5"}};
    for (const auto& [seconds, text] : kFormatted) {
        CHECK(s.error_of(
                   fmt::format("assert(FormatTime({0}) == '{1}', FormatTime({0}))", seconds, text))
                  .empty());
    }
}

TEST_CASE("Every army's score is shown, a civilian's too, until SetArmyShowScore hides it",
          "[session]") {
    // A sim of a player and a civilian army, its own state with its own
    // bindings, and the UI state bound to it.
    osc::lua::LuaState sim_lua;
    osc::sim::SimState sim{sim_lua.raw(), nullptr};
    osc::lua::register_moho_bindings(sim_lua, sim);
    osc::lua::register_sim_bindings(sim_lua, sim);
    sim.add_army("ARMY_1", "ARMY_1");
    sim.add_army("NEUTRAL_CIVILIAN", "NEUTRAL_CIVILIAN");
    sim.get_army(1)->set_civilian(true);
    osc::lua::LuaState ui;
    osc::ui::UIControlRegistry registry;
    osc::GameStateManager mgr;
    osc::lua::register_moho_bindings(ui, sim);
    osc::lua::register_ui_bindings(ui, registry);
    osc::lua::register_user_bindings(ui);
    osc::app::register_session_ui_globals(ui);
    lua_State* L = ui.raw();
    lua_pushstring(L, "__osc_game_state_mgr");
    lua_pushlightuserdata(L, &mgr);
    lua_rawset(L, LUA_REGISTRYINDEX);
    mgr.transition_to(osc::GameState::GAME, nullptr);
    const auto run = [](osc::lua::LuaState& state, const char* code) {
        auto r = state.do_string(code);
        return r.ok() ? std::string() : r.error().message;
    };

    // Moho's mShowScore starts on for every army: the score screens leave
    // civilians out by `civilian`, and the unit view names their units.
    CHECK(run(ui, "local t = GetArmiesTable().armiesTable\n"
                  "assert(t[1].showScore == true and t[2].showScore == true)\n"
                  "assert(t[2].civilian == true)\n")
              .empty());
    // SetArmyShowScore takes the army by number or name.
    CHECK(run(sim_lua, "SetArmyShowScore(2, false)").empty());
    CHECK_FALSE(sim.get_army(1)->show_score());
    CHECK(run(ui, "assert(GetArmiesTable().armiesTable[2].showScore == false)").empty());
    CHECK(run(sim_lua, "SetArmyShowScore('ARMY_1', false)\nSetArmyShowScore('ARMY_1', true)")
              .empty());
    CHECK(sim.get_army(0)->show_score());
    CHECK(run(sim_lua, "SetArmyShowScore(1)")
              .ends_with("SetArmyColor(army, bool) - determines if the user should be able to "
                         "see the army score\n  expected 2 args, but got 1"));
}

TEST_CASE("ShouldCreateInitialArmyUnits is false with /noinitialunits on the command line",
          "[session][scenario]") {
    osc::lua::LuaState sim_lua;
    osc::sim::SimState sim(sim_lua.raw(), nullptr);
    osc::lua::register_moho_bindings(sim_lua, sim);
    osc::lua::register_sim_bindings(sim_lua, sim);
    const auto answer = [&sim_lua] {
        auto r = sim_lua.do_string("__answer = ShouldCreateInitialArmyUnits()");
        REQUIRE(r.ok());
        lua_State* L = sim_lua.raw();
        lua_pushstring(L, "__answer");
        lua_rawget(L, LUA_GLOBALSINDEX);
        const bool yes = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        return yes;
    };

    // No command line known: the starting units are made.
    osc::lua::set_process_command_line(nullptr);
    CHECK(answer());

    // Moho's reads the process's command line (CFG_GetArgOption), case aside.
    const std::vector<std::string> with{"/map", "/maps/x/x_scenario.lua", "/NoInitialUnits"};
    const std::vector<std::string> without{"/map", "/maps/x/x_scenario.lua", "/nomovie"};
    osc::lua::set_process_command_line(&with);
    CHECK_FALSE(answer());
    // Scripts don't get it from the process: HasCommandLineArg answers from
    // a state's own copy, which only the UI state is given.
    osc::lua::LuaState ui;
    osc::ui::UIControlRegistry registry;
    osc::lua::register_moho_bindings(ui, sim);
    osc::lua::register_ui_bindings(ui, registry);
    CHECK(ui.do_string("assert(HasCommandLineArg('/noinitialunits') == false)").ok());
    osc::lua::set_process_command_line(&without);
    CHECK(answer());

    // Its usage is empty in Moho, so its count error is the bare count.
    auto r = sim_lua.do_string("ShouldCreateInitialArmyUnits(1)");
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().message.find("expected 0 args, but got 1") != std::string::npos);
    osc::lua::set_process_command_line(nullptr);
}
