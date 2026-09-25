// FrontEndData carries values between Lua states -- the lobby's, the
// game's, the next front end's -- as copies, so a value outlives the state
// that set it (M191 step 4).

#include <catch2/catch_test_macros.hpp>

#include "core/front_end_data.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

#include <string>

namespace {

struct LuaGuard {
    lua_State* L = lua_open();
    LuaGuard() {
        luaopen_base(L); // assert
        lua_settop(L, 0);
    }
    ~LuaGuard() {
        if (L) lua_close(L);
    }
};

bool run(lua_State* L, const char* code) {
    if (luaL_loadbuffer(L, code, std::string(code).size(), "test") != 0 ||
        lua_pcall(L, 0, 0, 0) != 0) {
        UNSCOPED_INFO(lua_tostring(L, -1));
        lua_pop(L, 1);
        return false;
    }
    return true;
}

void set_from_global(osc::FrontEndData& fed, lua_State* L, const char* key, const char* global) {
    lua_pushstring(L, global);
    lua_rawget(L, LUA_GLOBALSINDEX);
    fed.set(L, key, -1);
    lua_pop(L, 1);
}

void get_into_global(osc::FrontEndData& fed, lua_State* L, const char* key, const char* global) {
    lua_pushstring(L, global);
    fed.get(L, key);
    lua_rawset(L, LUA_GLOBALSINDEX);
}

} // namespace

TEST_CASE("FrontEndData outlives the Lua state that set it", "[frontend]") {
    osc::FrontEndData fed;
    {
        LuaGuard lobby;
        REQUIRE(run(lobby.L, R"(
            shared = { team = 2 }
            config = {
                ScenarioFile = '/maps/SCMP_009/SCMP_009_scenario.lua',
                PlayerOptions = { { PlayerName = 'Player', Team = 1 }, { AIPersonality = 'rush' } },
                a = shared, b = shared,
                OnLaunch = function() end,
            }
        )"));
        set_from_global(fed, lobby.L, "sessionConfig", "config");
        lua_pushstring(lobby.L, "replay.oscreplay");
        fed.set(lobby.L, "replay_filename", -1);
        lua_pop(lobby.L, 1);
    } // the lobby's state is gone

    LuaGuard game;
    get_into_global(fed, game.L, "sessionConfig", "config");
    get_into_global(fed, game.L, "replay_filename", "replay");
    get_into_global(fed, game.L, "missing", "missing");
    CHECK(run(game.L, R"(
        assert(config.ScenarioFile == '/maps/SCMP_009/SCMP_009_scenario.lua')
        assert(config.PlayerOptions[1].PlayerName == 'Player')
        assert(config.PlayerOptions[2].AIPersonality == 'rush')
        assert(config.a == config.b and config.a.team == 2, 'a shared table stays shared')
        assert(config.OnLaunch == nil, 'functions are not data')
        assert(replay == 'replay.oscreplay')
        assert(missing == nil)
    )"));
}

TEST_CASE("FrontEndData hands out copies; nil removes a key", "[frontend]") {
    osc::FrontEndData fed;
    LuaGuard L;
    REQUIRE(run(L.L, "t = { n = 1 }"));
    set_from_global(fed, L.L, "t", "t");
    REQUIRE(run(L.L, "t.n = 2")); // the original changes after the set
    get_into_global(fed, L.L, "t", "first");
    get_into_global(fed, L.L, "t", "second");
    CHECK(run(L.L, R"(
        assert(first.n == 1, 'the value as it was set')
        first.n = 3
        assert(second.n == 1, 'each get is its own copy')
    )"));

    lua_pushnil(L.L);
    fed.set(L.L, "t", -1);
    lua_pop(L.L, 1);
    get_into_global(fed, L.L, "t", "gone");
    CHECK(run(L.L, "assert(gone == nil)"));

    REQUIRE(run(L.L, "x = 'kept'"));
    set_from_global(fed, L.L, "x", "x");
    fed.clear();
    get_into_global(fed, L.L, "x", "cleared");
    CHECK(run(L.L, "assert(cleared == nil)"));
}
