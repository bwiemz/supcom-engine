#include <catch2/catch_test_macros.hpp>

#include "blueprints/blueprint_store.hpp"
#include "core/localization.hpp"
#include "log_capture.hpp"
#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "lua/sim_bindings.hpp"
#include "lua/sim_sync.hpp"
#include "lua/user_bindings.hpp"
#include "sim/sim_state.hpp"
#include "ui/console.hpp"
#include "ui/ui_control.hpp"

extern "C" {
#include <lua.h>
}

#include <string>
#include <vector>

namespace {

std::string run(osc::lua::LuaState& lua, const char* code) {
    auto r = lua.do_string(code);
    return r.ok() ? std::string() : r.error().message;
}

struct ConsoleWorld {
    osc::lua::LuaState sim_lua;
    osc::blueprints::BlueprintStore store{sim_lua.raw()};
    osc::sim::SimState sim{sim_lua.raw(), &store};
    osc::lua::LuaState ui;
    osc::ui::UIControlRegistry registry;

    ConsoleWorld() {
        sim.add_army("ARMY_1", "ARMY_1");
        osc::lua::register_moho_bindings(sim_lua, sim);
        osc::lua::register_sim_bindings(sim_lua, sim);
        osc::lua::register_moho_bindings(ui, sim);
        osc::lua::register_ui_bindings(ui, registry);
        osc::lua::register_user_bindings(ui);
        REQUIRE(run(ui, "lines = {}\n"
                        "AddConsoleOutputReciever(function(text) table.insert(lines, text) end)")
                    .empty());
    }

    std::vector<std::string> lines() {
        std::vector<std::string> out;
        lua_State* L = ui.raw();
        lua_pushstring(L, "lines");
        lua_rawget(L, LUA_GLOBALSINDEX);
        for (int i = 1;; ++i) {
            lua_rawgeti(L, -1, i);
            if (!lua_isstring(L, -1)) {
                lua_pop(L, 1);
                break;
            }
            out.emplace_back(lua_tostring(L, -1));
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
        return out;
    }
};

} // namespace

TEST_CASE("print in the user state gives the console's receivers its lines, as Moho writes them",
          "[console]") {
    ConsoleWorld w;
    REQUIRE(run(w.ui, "print('osc a', 'b')\n"
                      "print('abcdefgh', 'x')\n"
                      "print('a\\tb') print('ab\\tc') print('abcdefg\\th')\n"
                      "print('line1\\nline2')\n"
                      "print('cr\\rx')\n"
                      "print(1, 2.5, nil, true, false)\n"
                      "print() print('') print('trail\\n') print('\\n')")
                .empty());
    CHECK(w.lines() == std::vector<std::string>{"osc a     b", "abcdefgh        x", "a b", "ab  c",
                                                "abcdefg       h", "line1", "line2", "cr\\015x",
                                                "1 2.5     nil     true      false", "trail", ""});
}

TEST_CASE("The console's receivers hear each line in the order they were added, until removed",
          "[console]") {
    ConsoleWorld w;
    REQUIRE(run(w.ui, "local second = AddConsoleOutputReciever(function(text)\n"
                      "    table.insert(lines, 'second ' .. text)\n"
                      "end)\n"
                      "print('one')\n"
                      "RemoveConsoleOutputReciever(second)\n"
                      "print('two')")
                .empty());
    CHECK(w.lines() == std::vector<std::string>{"one", "second one", "two"});

    osc::test::LogCapture log;
    REQUIRE(run(w.ui, "none = AddConsoleOutputReciever(5)").empty());
    CHECK(log.count("AddConsoleOutputReciever received an object that was not a function!") == 1);
    CHECK(run(w.ui, "AddConsoleOutputReciever()").find("expected 1 args, but got 0") !=
          std::string::npos);
}

TEST_CASE("The sim's print reaches the user's console with its beat, after OnSync",
          "[console][sync]") {
    ConsoleWorld w;
    REQUIRE(run(w.ui, "function OnSync() table.insert(lines, 'sync') end").empty());
    REQUIRE(run(w.sim_lua, "print('osc sim print', 7) print('osc sim a\\nosc sim b')").empty());
    osc::lua::SyncQueue sync;
    sync.queue(w.sim_lua.raw(), nullptr);
    REQUIRE(run(w.sim_lua, "print('next beat')").empty());
    sync.deliver(w.sim_lua.raw(), w.ui.raw());
    CHECK(w.lines() ==
          std::vector<std::string>{"sync", "osc sim print     7", "osc sim a", "osc sim b"});
    sync.deliver(w.sim_lua.raw(), w.ui.raw());
    CHECK(w.lines() == std::vector<std::string>{"sync", "osc sim print     7", "osc sim a",
                                                "osc sim b", "sync", "next beat"});
}

TEST_CASE("An unknown console command and a command's message reach the console", "[console]") {
    ConsoleWorld w;
    osc::core::Localization loc;
    loc.add("_No_session", "No session");
    lua_State* L = w.ui.raw();
    lua_pushstring(L, "__osc_loc_cache");
    lua_pushlightuserdata(L, &loc);
    lua_rawset(L, LUA_REGISTRYINDEX);
    osc::ui::Console console;
    osc::lua::register_session_console_commands(console);
    console.execute(L, "NoSuchCommand 1");
    console.execute(L, "UI_ShowRenameDialog");
    CHECK(w.lines() ==
          std::vector<std::string>{"Unknown console command \"NoSuchCommand\"", "No session"});
}
