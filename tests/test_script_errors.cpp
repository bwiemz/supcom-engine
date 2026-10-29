// An error raised by a game UI script the engine calls is logged with its
// message before the engine's own answer stands in for the script's, as
// Moho logs its script calls' errors (CScriptObject::LogScriptWarning).

#include <catch2/catch_test_macros.hpp>

#include "log_capture.hpp"

#include "app/app_internal.hpp"
#include "app/support.hpp"
#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "lua/special_files.hpp"
#include "lua/user_bindings.hpp"
#include "sim/sim_state.hpp"
#include "ui/console.hpp"

extern "C" {
#include <lua.h>
}

#include <string>

using osc::test::LogCapture;

TEST_CASE("An ability order's click reports an error in VerifyScriptCommand", "[lua][ui]") {
    // Moho's UI_VerifyScriptCommand logs it in the exe's own words (its
    // module path without /user/), and lets nothing go
    osc::lua::LuaState state;
    osc::sim::SimState sim{state.raw(), nullptr};
    osc::lua::register_moho_bindings(state, sim);
    REQUIRE(state
                .do_string(R"(
        function import(name)
            return {VerifyScriptCommand = function() error('boom') end}
        end
    )")
                .ok());
    LogCapture log;
    CHECK_FALSE(osc::app::verify_script_command(state.raw(), {}, {}, {1, 2, 3}, true));
    CHECK(log.count("Error running '/lua/UserScriptCommand.lua:VerifyScriptCommand': ") == 1);
    CHECK(log.count("boom") == 1);
}

TEST_CASE("The StartCommandMode console command reports an error in GetCommandMode",
          "[lua][ui][console]") {
    osc::lua::LuaState state;
    osc::sim::SimState sim{state.raw(), nullptr};
    osc::lua::register_moho_bindings(state, sim);
    REQUIRE(state
                .do_string(R"(
        function import(name)
            return {GetCommandMode = function() error('boom') end}
        end
    )")
                .ok());
    osc::ui::Console console;
    osc::lua::register_session_console_commands(console);
    LogCapture log;
    console.execute(state.raw(), "StartCommandMode order RULEUCC_Move");
    CHECK(log.count("Error running '/lua/ui/game/commandmode.lua:GetCommandMode': ") == 1);
    CHECK(log.count("boom") == 1);
}

TEST_CASE("The current profile's name reports an error in GetCurrentProfile", "[lua][ui]") {
    osc::lua::LuaState state;
    REQUIRE(state
                .do_string(R"(
        function import(name)
            return {GetCurrentProfile = function()
                if failing then error('boom') end
                return {Name = 'Alice'}
            end}
        end
    )")
                .ok());
    LogCapture log;
    CHECK(osc::lua::current_profile(state.raw()) == "Alice");
    CHECK(log.count("Error running") == 0);
    REQUIRE(state.do_string("failing = true").ok());
    CHECK(osc::lua::current_profile(state.raw()).empty());
    CHECK(log.count("Error running '/lua/user/prefs.lua:GetCurrentProfile': ") == 1);
    CHECK(log.count("boom") == 1);
}
