#include <catch2/catch_test_macros.hpp>

#include "lua/engine_bindings.hpp"
#include "lua/game_mods.hpp"
#include "lua/lua_bytes.hpp"
#include "lua/lua_state.hpp"
#include "lua/session_manager.hpp"

#include <string>

extern "C" {
#include <lua.h>
}

using namespace osc::lua;

namespace {

bool lua_true(LuaState& state, const std::string& expr) {
    REQUIRE(state.do_string("__check = " + expr).ok());
    lua_getglobal(state.raw(), "__check");
    const bool value = lua_toboolean(state.raw(), -1) != 0;
    lua_pop(state.raw(), 1);
    return value;
}

/// read_session_config of the global table `name`.
osc::sim::GameSetup setup_of(LuaState& state, const char* name) {
    lua_getglobal(state.raw(), name);
    auto setup = read_session_config(state.raw(), lua_gettop(state.raw()));
    lua_pop(state.raw(), 1);
    return setup;
}

} // namespace

TEST_CASE("A launch's mods: the lobby's GameMods, else scenarioMods", "[lua][mods]") {
    LuaState launch;
    const auto made = launch.do_string(R"(
        lobby = { GameOptions = {}, PlayerOptions = {},
                  GameMods = { { uid = 'b-mod', location = '/mods/b' },
                               { uid = 'a-mod', location = '/mods/a', hookdir = '/h' } },
                  scenarioMods = { { uid = 'not-this-one' } } }
        campaign = { scenarioMods = { { uid = 'campaign', location = '/mods/c' } } }
        none = { PlayerOptions = {} }
        cyclic = { GameMods = {} }
        cyclic.GameMods[1] = cyclic.GameMods
    )");
    REQUIRE(made.ok());

    const auto lobby = setup_of(launch, "lobby");
    const auto campaign = setup_of(launch, "campaign");
    CHECK(setup_of(launch, "none").mods.empty());
    CHECK(setup_of(launch, "cyclic").mods.empty()); // reported; no mods

    // Each game state reads them back as its __active_mods, in order
    LuaState game;
    set_active_mods(game.raw(), lobby.mods);
    CHECK(lua_true(game, "table.getn(__active_mods) == 2"));
    CHECK(lua_true(game, "__active_mods[1].uid == 'b-mod' and __active_mods[2].hookdir == '/h'"));
    set_active_mods(game.raw(), campaign.mods);
    CHECK(lua_true(game, "table.getn(__active_mods) == 1 and __active_mods[1].uid == 'campaign'"));
}

TEST_CASE("set_active_mods: the game's list, or an empty one", "[lua][mods]") {
    LuaState state;
    set_active_mods(state.raw(), "");
    CHECK(lua_true(state, "type(__active_mods) == 'table' and next(__active_mods) == nil"));

    // Bytes that hold no list, or nothing at all: no mods, never a crash
    REQUIRE(state.do_string("__value = 'a string'").ok());
    lua_getglobal(state.raw(), "__value");
    const auto not_a_list = lua_to_bytes(state.raw(), -1);
    lua_pop(state.raw(), 1);
    REQUIRE(not_a_list);
    const int top = lua_gettop(state.raw());
    for (const std::string& bytes : {*not_a_list, std::string("\x05\xff", 2)}) {
        set_active_mods(state.raw(), bytes);
        CHECK(lua_true(state, "type(__active_mods) == 'table' and next(__active_mods) == nil"));
        CHECK(lua_gettop(state.raw()) == top);
    }
}

TEST_CASE("The bindings keep a launch's __active_mods", "[lua][mods]") {
    LuaState state;
    REQUIRE(state.do_string("__active_mods = { { uid = 'kept' } }").ok());
    register_blueprint_bindings(state); // as load_blueprints does, after the launch set them
    CHECK(lua_true(state, "__active_mods[1].uid == 'kept'"));

    LuaState bare; // ...and give one to a state without
    register_blueprint_bindings(bare);
    CHECK(lua_true(bare, "type(__active_mods) == 'table' and next(__active_mods) == nil"));
    REQUIRE(bare.do_string("__active_mods = 'not a list'").ok());
    ensure_active_mods(bare.raw());
    CHECK(lua_true(bare, "type(__active_mods) == 'table'"));
}

TEST_CASE("A UI state remembers that it ran SessionInit.lua", "[lua][mods]") {
    LuaState state;
    CHECK_FALSE(session_init_ran(state.raw()));
    mark_session_init(state.raw());
    CHECK(session_init_ran(state.raw()));
    CHECK_FALSE(session_init_ran(LuaState().raw()));
}
