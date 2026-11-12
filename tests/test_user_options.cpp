// The player's options as the engine reads them: Moho's OPTIONS_GetInt,
// OPTIONS_GetFloat, OPTIONS_GetString and OPTIONS_GetBool ask the UI state's
// /lua/user/prefs.lua GetOption(key), with their fallbacks when it fails.

#include <catch2/catch_test_macros.hpp>

#include "app/window_commands.hpp"
#include "app/window_mode.hpp"
#include "core/preferences.hpp"
#include "lua/lua_state.hpp"
#include "lua/user_options.hpp"

extern "C" {
#include <lua.h>
}

#include <string>

namespace {

/// A UI state whose `import` gives a stub /lua/user/prefs.lua: its
/// GetOption answers from the global table `answers`, and logs each call's
/// key in the global `asked` ("key;" each), as `import` logs its paths in
/// `imported`.
struct OptionsState {
    osc::lua::LuaState ui;
    lua_State* L = ui.raw();

    OptionsState() {
        run(R"(
            asked = ''
            imported = ''
            answers = {}
            prefs = {
                GetOption = function(...)
                    asked = asked .. tostring(arg[1])
                    if arg.n ~= 1 then asked = asked .. '(' .. arg.n .. ' args)' end
                    asked = asked .. ';'
                    return answers[arg[1]]
                end,
            }
            function import(path)
                imported = imported .. path .. ';'
                if path == '/lua/user/prefs.lua' then return prefs end
                error('no module ' .. path)
            end
        )");
    }

    void run(const char* code) {
        const auto r = ui.do_string(code);
        INFO((r.ok() ? std::string() : r.error().message));
        REQUIRE(r.ok());
    }

    /// A global string's text, then emptied.
    std::string take(const char* name) {
        lua_getglobal(L, name);
        const char* text = lua_tostring(L, -1);
        std::string out = text ? text : "";
        lua_pop(L, 1);
        lua_pushstring(L, "");
        lua_setglobal(L, name);
        return out;
    }
};

} // namespace

TEST_CASE("The engine reads an option through prefs.lua's GetOption, as Moho's OPTIONS_Get*",
          "[prefs][lua]") {
    OptionsState s;
    s.run(R"(answers = {vsync = 1, primary_adapter = 'windowed', antialiasing = '4',
                        frame_rate = 59.9, cam_ZoomAmount = 0.25, display_eta = true,
                        mainmenu_bgmovie = false})");
    const int top = lua_gettop(s.L);

    // The module's GetOption, with the key alone
    CHECK(osc::lua::options_get_int(s.L, "vsync") == 1);
    CHECK(s.take("imported") == "/lua/user/prefs.lua;");
    CHECK(s.take("asked") == "vsync;");

    // LuaPlus's conversions: a numeric string reads as its number, a number
    // as its text, an int truncated
    CHECK(osc::lua::options_get_string(s.L, "primary_adapter") == "windowed");
    CHECK(osc::lua::options_get_int(s.L, "antialiasing") == 4);
    CHECK(osc::lua::options_get_string(s.L, "vsync") == "1");
    CHECK(osc::lua::options_get_int(s.L, "frame_rate") == 59);
    CHECK(osc::lua::options_get_float(s.L, "cam_ZoomAmount") == 0.25f);
    CHECK(osc::lua::options_get_bool(s.L, "display_eta"));
    CHECK_FALSE(osc::lua::options_get_bool(s.L, "mainmenu_bgmovie"));
    CHECK(s.take("asked") ==
          "primary_adapter;antialiasing;vsync;frame_rate;cam_ZoomAmount;display_eta;"
          "mainmenu_bgmovie;");

    // No answer (nil): 0, "", false
    CHECK(osc::lua::options_get_int(s.L, "missing") == 0);
    CHECK(osc::lua::options_get_float(s.L, "missing") == 0.0f);
    CHECK(osc::lua::options_get_string(s.L, "missing").empty());
    CHECK_FALSE(osc::lua::options_get_bool(s.L, "missing"));

    // Asked again each time: nothing is kept
    s.run("answers.vsync = 0");
    CHECK(osc::lua::options_get_int(s.L, "vsync") == 0);
    CHECK(lua_gettop(s.L) == top);
}

TEST_CASE("A GetOption that fails answers Moho's fallbacks: 0, 0, \"\" and false", "[prefs][lua]") {
    // OPTIONS_Get* catch what the call raises, warn "Error running
    // '/lua/user/prefs.lua:GetOption' : ..." and answer their fallback
    OptionsState s;
    s.run("answers = {vsync = 1, cam_ZoomAmount = 0.25, primary_adapter = 'windowed', "
          "display_eta = true}");
    const int top = lua_gettop(s.L);
    const auto falls_back = [&s, top] {
        CHECK(osc::lua::options_get_int(s.L, "vsync") == 0);
        CHECK(osc::lua::options_get_float(s.L, "cam_ZoomAmount") == 0.0f);
        CHECK(osc::lua::options_get_string(s.L, "primary_adapter").empty());
        CHECK_FALSE(osc::lua::options_get_bool(s.L, "display_eta"));
        CHECK(lua_gettop(s.L) == top);
    };

    // Its script's error
    s.run("prefs.GetOption = function(key) error('broken ' .. key) end");
    falls_back();
    // No GetOption in the module
    s.run("prefs.GetOption = nil");
    falls_back();
    // No module
    s.run("function import(path) error('no module ' .. path) end");
    falls_back();
    // No import at all
    s.run("import = nil");
    falls_back();
}

TEST_CASE("The window's startup asks GetOption for primary_adapter, unless the command line "
          "chooses",
          "[prefs][lua][window]") {
    // CScApp reads the option (OPTIONS_GetString) only when no /windowed,
    // /fullscreen or the like chose the head
    OptionsState s;
    s.run("answers = {primary_adapter = 'windowed'}");
    osc::core::Preferences prefs;
    prefs.set_int("Windows.Main.width", 1500);
    prefs.set_int("Windows.Main.height", 900);

    osc::app::WindowPrefs w = osc::app::startup_window_prefs(s.L, prefs, {"opensupcom"});
    CHECK(w.primary_adapter == "windowed");
    CHECK(w.width.value_or(0u) == 1500u);
    CHECK(s.take("asked") == "primary_adapter;");

    w = osc::app::startup_window_prefs(s.L, prefs, {"opensupcom", "/windowed", "1280", "800"});
    CHECK(s.take("asked").empty());
    CHECK(osc::app::startup_window_mode({"opensupcom", "/windowed", "1280", "800"}, w).overridden);

    // A failed call: "", and the head full screen at the default size
    s.run("import = nil");
    w = osc::app::startup_window_prefs(s.L, prefs, {});
    CHECK(w.primary_adapter.empty());
    const osc::app::WindowMode m = osc::app::startup_window_mode({}, w);
    CHECK(m.fullscreen);
    CHECK(m.size == osc::app::Resolution{1024, 768, 60});
}
