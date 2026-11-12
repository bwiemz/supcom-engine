// The game's UI provider (ui::WldUIProvider): the calls Moho's
// CLuaWldUIProvider makes on gamemain's Lua provider object.

#include <catch2/catch_test_macros.hpp>

#include "lua/lua_state.hpp"
#include "ui/wld_ui_provider.hpp"

extern "C" {
#include <lua.h>
}

#include <string>

namespace {

/// A UI state whose provider object (what InternalCreateWldUIProvider
/// registers) logs the calls it hears in the global `heard`.
struct ProviderState {
    osc::lua::LuaState ui;
    lua_State* L = ui.raw();

    explicit ProviderState(bool with_on_start) {
        const auto made = ui.do_string(R"(
            heard = ''
            provider = {
                CreateGameInterface = function(self, isReplay)
                    heard = heard .. 'CreateGameInterface;'
                end,
                DestroyGameInterface = function(self)
                    heard = heard .. 'DestroyGameInterface;'
                end,
            }
        )");
        INFO((made.ok() ? std::string() : made.error().message));
        REQUIRE(made.ok());
        if (with_on_start) {
            // Its arguments: self alone
            REQUIRE(ui.do_string(R"(
                provider.OnStart = function(...)
                    heard = heard .. 'OnStart'
                    if arg[1] ~= provider then heard = heard .. '(not self)' end
                    if arg.n ~= 1 then heard = heard .. '(' .. arg.n .. ' args)' end
                    heard = heard .. ';'
                end
            )")
                        .ok());
        }
        lua_pushstring(L, osc::ui::WldUIProvider::kLuaObjectKey);
        lua_getglobal(L, "provider");
        lua_rawset(L, LUA_REGISTRYINDEX);
    }

    /// What the provider heard since the last look.
    std::string heard() {
        lua_getglobal(L, "heard");
        const char* text = lua_tostring(L, -1);
        std::string out = text ? text : "";
        lua_pop(L, 1);
        lua_pushstring(L, "");
        lua_setglobal(L, "heard");
        return out;
    }
};

} // namespace

TEST_CASE("The game's UI provider hears OnStart once, after its interface is made", "[ui][lua]") {
    // Moho's frame actions: Initializing ends with CreateGameInterface; the
    // next frame enters Playing (every client ready) and calls the
    // provider's OnStart (CLuaWldUIProvider::OnStart, RunScript: self alone)
    ProviderState s(true);
    osc::ui::WldUIProvider wld;

    // No interface yet: nothing is due
    CHECK_FALSE(wld.start_due());
    CHECK_FALSE(wld.start_game(s.L));
    CHECK(s.heard().empty());

    // The interface is made; OnStart waits for the world to play
    const int top = lua_gettop(s.L);
    wld.create_game_interface(s.L, false);
    CHECK(s.heard() == "CreateGameInterface;");
    CHECK(wld.start_due());

    CHECK(wld.start_game(s.L));
    CHECK(s.heard() == "OnStart;");
    CHECK(lua_gettop(s.L) == top);
    // Once a game
    CHECK_FALSE(wld.start_due());
    CHECK_FALSE(wld.start_game(s.L));
    CHECK(s.heard().empty());

    // The next game's interface, its own OnStart
    wld.destroy_game_interface(s.L);
    wld.create_game_interface(s.L, true);
    CHECK(s.heard() == "DestroyGameInterface;CreateGameInterface;");
    CHECK(wld.start_game(s.L));
    CHECK(s.heard() == "OnStart;");

    // A game left before it plays never starts
    wld.destroy_game_interface(s.L);
    wld.create_game_interface(s.L, false);
    wld.destroy_game_interface(s.L);
    CHECK(s.heard() == "DestroyGameInterface;CreateGameInterface;DestroyGameInterface;");
    CHECK_FALSE(wld.start_due());
    CHECK_FALSE(wld.start_game(s.L));
    CHECK(s.heard().empty());
}

TEST_CASE("A provider without OnStart (retail's) starts its game quietly", "[ui][lua]") {
    // Retail's gamemain.CreateWldUIProvider defines no OnStart: Moho's
    // RunScript finds none and calls nothing
    ProviderState s(false);
    osc::ui::WldUIProvider wld;
    wld.create_game_interface(s.L, false);
    CHECK(s.heard() == "CreateGameInterface;");
    const int top = lua_gettop(s.L);
    CHECK_FALSE(wld.start_game(s.L));
    CHECK(lua_gettop(s.L) == top);
    CHECK_FALSE(wld.start_due());
    CHECK(s.heard().empty());
}

TEST_CASE("Without a provider no interface is made and nothing starts", "[ui][lua]") {
    osc::lua::LuaState ui;
    osc::ui::WldUIProvider wld;
    CHECK_FALSE(wld.create_game_interface(ui.raw(), false));
    CHECK_FALSE(wld.start_due());
    CHECK_FALSE(wld.start_game(ui.raw()));
}
