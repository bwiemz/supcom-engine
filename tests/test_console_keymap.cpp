#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/game_state.hpp"
#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "sim/sim_state.hpp"
#include "ui/console.hpp"
#include "ui/keymap.hpp"
#include "ui/ui_control.hpp"

#include <GLFW/glfw3.h>

#include <string>
#include <vector>

extern "C" {
#include <lua.h>
}

using osc::u32;
using osc::ui::Console;
using osc::ui::KeyMapRegistry;
using osc::ui::parse_console_command;
using Tokens = std::vector<std::string>;

namespace {

Tokens tokens_of(std::string_view line, std::string* rest = nullptr) {
    Tokens t;
    std::string r;
    parse_console_command(line, t, r);
    if (rest) *rest = r;
    return t;
}

} // namespace

TEST_CASE("Console lines parse as Moho's CON_ParseCommand", "[ui][console]") {
    // The lines as constants: MSVC's preprocessor mangles raw strings with
    // escapes inside a macro's arguments.
    // Retail's key actions: a quote mid-token is a character.
    const std::string retail_action = R"(UI_Lua import("/lua/ui/uimain.lua").EscapeHandler())";
    const std::string retail_code = R"(import("/lua/ui/uimain.lua").EscapeHandler())";
    CHECK(tokens_of(retail_action) == Tokens{"UI_Lua", retail_code});
    CHECK(tokens_of("  foo   bar\tbaz ") == Tokens{"foo", "bar", "baz"});
    // A token opened by a quote runs to the next one, with escapes.
    const std::string quoted = R"(say "hello world" x)";
    const std::string escaped = R"(a "b \"c\" \\d" e)";
    const std::string unescaped = R"(b "c" \d)";
    const std::string unclosed = R"("unclosed to the end)";
    const std::string leading_quote = R"(\"x y)";
    CHECK(tokens_of(quoted) == Tokens{"say", "hello world", "x"});
    CHECK(tokens_of(escaped) == Tokens{"a", unescaped, "e"});
    CHECK(tokens_of(unclosed) == Tokens{"unclosed to the end"});
    CHECK(tokens_of(leading_quote) == Tokens{"\"x", "y"});
    // `;` ends a command; `#` and `//` end the line (a lone `/` doesn't).
    std::string rest;
    CHECK(tokens_of("a 1; b 2", &rest) == Tokens{"a", "1"});
    CHECK(rest == " b 2");
    const std::string quoted_semicolon = R"(a "x;y" z)";
    CHECK(tokens_of(quoted_semicolon) == Tokens{"a", "x;y", "z"});
    CHECK(tokens_of("a # b") == Tokens{"a"});
    CHECK(tokens_of("a // b") == Tokens{"a"});
    CHECK(tokens_of("a/b /c") == Tokens{"a/b", "/c"});
    CHECK(tokens_of("").empty());
}

TEST_CASE("The console runs each command of a line, by name in any case", "[ui][console]") {
    Console console;
    std::vector<Tokens> ran;
    console.add("Echo", [&](lua_State*, const Tokens& args) { ran.push_back(args); });
    CHECK(console.has("echo"));
    CHECK_FALSE(console.has("Nope"));
    console.execute(nullptr, "echo 1; ECHO 2 3;; nope 4; Echo \"5 6\"");
    REQUIRE(ran.size() == 3);
    CHECK(ran[0] == Tokens{"echo", "1"});
    CHECK(ran[1] == Tokens{"ECHO", "2", "3"});
    CHECK(ran[2] == Tokens{"Echo", "5 6"});
    ran.clear();
    console.execute(nullptr, "# only a comment");
    console.execute(nullptr, "");
    CHECK(ran.empty());
}

TEST_CASE("Key chords parse and bind as Moho's CUIKeyHandler", "[ui][keymap]") {
    osc::lua::LuaState lua;
    lua_State* L = lua.raw();
    KeyMapRegistry km;
    CHECK(km.key_name(0x1B) == "Unknown1B");
    REQUIRE(lua.do_string(R"(names = { ['1B'] = 'Esc', ['10'] = 'Shift', ['41'] = 'A',
                                       ['0x70'] = 'F1', ['6B'] = 'NumPlus', ['100'] = 'Bad' })")
                .ok());
    lua_getglobal(L, "names");
    km.set_key_names(L, -1);
    lua_pop(L, 1);
    CHECK(km.key_name(0x1B) == "Esc");
    CHECK(km.key_name(0x70) == "F1");

    CHECK(km.parse("Esc") == 0x1B);
    CHECK(km.parse("esc") == 0x1B);
    const auto ctrl_shift_a =
        static_cast<osc::i32>(0x41u | KeyMapRegistry::kCtrl | KeyMapRegistry::kShift);
    CHECK(km.parse("Ctrl-Shift-A") == ctrl_shift_a);
    CHECK(km.parse("shift-CTRL-a") == ctrl_shift_a);
    CHECK(km.parse("Alt-F1") == static_cast<osc::i32>(0x70u | KeyMapRegistry::kAlt));
    CHECK(km.parse("Nope") == -1);
    CHECK(km.parse("Ctrl-Nope") == -1);
    CHECK(km.parse("") == 0);
    CHECK(km.parse("Meta-A") == 0x41); // an unknown modifier is warned about, not kept

    CHECK(KeyMapRegistry::chord(GLFW_KEY_ESCAPE, 0) == 0x1Bu);
    CHECK(KeyMapRegistry::chord(GLFW_KEY_A, GLFW_MOD_SHIFT | GLFW_MOD_CONTROL) ==
          static_cast<u32>(ctrl_shift_a));
    CHECK(KeyMapRegistry::chord(GLFW_KEY_KP_ADD, GLFW_MOD_ALT) == (0x6Bu | KeyMapRegistry::kAlt));

    REQUIRE(lua.do_string(R"(
        first = { Esc = { action = 'UI_Lua one()' }, ['Ctrl-Shift-A'] = { action = 'two', keyRepeat = true },
                  A = {} }
        second = { esc = { action = 'three' } }
    )")
                .ok());
    lua_getglobal(L, "first");
    km.add(L, -1);
    lua_pop(L, 1);
    REQUIRE(km.action(0x1B));
    CHECK(*km.action(0x1B) == "UI_Lua one()");
    REQUIRE(km.action(static_cast<u32>(ctrl_shift_a)));
    CHECK(*km.action(static_cast<u32>(ctrl_shift_a)) == "two");
    CHECK(km.repeats(static_cast<u32>(ctrl_shift_a)));
    CHECK_FALSE(km.repeats(0x1B));
    REQUIRE(km.action(0x41)); // bound, to nothing
    CHECK(km.action(0x41)->empty());
    CHECK(km.action(0x10) == nullptr);
    // A later table overwrites a chord; removing a table unbinds its chords.
    lua_getglobal(L, "second");
    km.add(L, -1);
    lua_pop(L, 1);
    CHECK(*km.action(0x1B) == "three");
    lua_getglobal(L, "first");
    km.remove(L, -1);
    lua_pop(L, 1);
    CHECK(km.action(0x1B) == nullptr);
    CHECK(km.action(static_cast<u32>(ctrl_shift_a)) == nullptr);
    CHECK_FALSE(km.repeats(static_cast<u32>(ctrl_shift_a)));
    CHECK(km.size() == 0);
}

TEST_CASE("The sim rate: an integer game speed, 10^(rate/10) times normal", "[ui][console]") {
    osc::GameStateManager mgr;
    CHECK(mgr.sim_rate() == 0);
    CHECK(mgr.speed() == 1.0);
    mgr.set_sim_rate(10);
    CHECK_THAT(mgr.speed(), Catch::Matchers::WithinRel(10.0, 1e-12));
    mgr.set_sim_rate(-10);
    CHECK_THAT(mgr.speed(), Catch::Matchers::WithinRel(0.1, 1e-12));
    mgr.set_sim_rate(-11);
    CHECK(mgr.sim_rate() == -10);
    mgr.set_sim_rate(60);
    CHECK(mgr.sim_rate() == 50);
    // A new game starts at normal speed, its rate and speed together.
    mgr.transition_to(osc::GameState::GAME, nullptr);
    CHECK(mgr.sim_rate() == 0);
    CHECK(mgr.speed() == 1.0);

    // Through the console and the Lua globals, as retail reaches it.
    osc::lua::LuaState lua;
    osc::sim::SimState sim(lua.raw(), nullptr);
    osc::ui::UIControlRegistry registry;
    osc::lua::register_moho_bindings(lua, sim);
    osc::lua::register_ui_bindings(lua, registry);
    Console console;
    osc::lua::register_console_commands(console);
    lua_State* L = lua.raw();
    lua_pushstring(L, "__osc_game_state_mgr");
    lua_pushlightuserdata(L, &mgr);
    lua_rawset(L, LUA_REGISTRYINDEX);
    lua_pushstring(L, "__osc_console");
    lua_pushlightuserdata(L, &console);
    lua_rawset(L, LUA_REGISTRYINDEX);
    const auto rate_after = [&](const char* code) {
        REQUIRE(lua.do_string(code).ok());
        return mgr.sim_rate();
    };
    CHECK(rate_after("ConExecute('WLD_ResetSimRate')") == 0);
    CHECK(rate_after("ConExecute('WLD_IncreaseSimRate') ConExecute('wld_increasesimrate')") == 2);
    CHECK(rate_after("ConExecute('WLD_DecreaseSimRate')") == 1);
    CHECK(rate_after("ConExecute('WLD_GameSpeed -4')") == -4);
    CHECK(rate_after("ConExecute('WLD_GameSpeed')") == -4); // usage only
    CHECK(rate_after("SetGameSpeed(0)") == 0);              // retail's cutscenes: normal speed
    CHECK(rate_after("SetGameSpeed(3)") == 3);
    REQUIRE(lua.do_string("assert(GetGameSpeed() == 3)").ok());
    CHECK(rate_after("ConExecute('UI_Lua SetGameSpeed(5)')") == 5);
    CHECK(rate_after("ConExecuteSave('UI_Lua SetGameSpeed(6) ; WLD_DecreaseSimRate')") == 5);
}
