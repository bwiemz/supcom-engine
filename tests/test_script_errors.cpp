// An error raised by a game UI script the engine calls is logged with its
// message before the engine's own answer stands in for the script's, as
// Moho logs its script calls' errors (CScriptObject::LogScriptWarning).

#include <catch2/catch_test_macros.hpp>

#include "log_capture.hpp"

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

TEST_CASE("An ability order's click reports an error in GetCommandMode", "[lua][ui]") {
    osc::lua::LuaState state;
    // The mode is read once for the click, and again for the ability's order
    REQUIRE(state
                .do_string(R"(
        calls = 0
        __modules = {['/lua/ui/game/commandmode.lua'] = {GetCommandMode = function()
            calls = calls + 1
            if calls > 1 then error('boom') end
            return {'order', {name = 'RULEUCC_Script', TaskName = 'Probe'}}
        end}}
    )")
                .ok());
    LogCapture log;
    const auto mode = osc::app::read_command_mode(state.raw());
    CHECK(mode.mode == "order");
    CHECK(mode.name == "RULEUCC_Script");
    CHECK(log.count("Error running '/lua/ui/game/commandmode.lua:GetCommandMode': ") == 1);
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
