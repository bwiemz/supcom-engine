#pragma once

#include "core/types.hpp"
#include <spdlog/spdlog.h>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

namespace osc::core {

enum class GameState : u8 {
    INIT,
    FRONT_END,
    LOADING,
    GAME,
    SCORE
};

inline const char* game_state_string(GameState state) {
    switch (state) {
        case GameState::INIT:      return "init";
        case GameState::FRONT_END: return "front-end";
        case GameState::LOADING:   return "loading";
        case GameState::GAME:      return "game";
        case GameState::SCORE:     return "score";
    }
    return "unknown";
}

inline void call_lua_global(lua_State* L, const char* name) {
    lua_pushstring(L, name);
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (lua_isfunction(L, -1)) {
        if (lua_pcall(L, 0, 0, 0) != 0) {
            spdlog::warn("{} error: {}", name, lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    } else {
        lua_pop(L, 1);
    }
}

inline void call_setup_ui(lua_State* L) { call_lua_global(L, "SetupUI"); }
inline void call_start_game_ui(lua_State* L) { call_lua_global(L, "StartGameUI"); }
inline void call_on_first_update(lua_State* L) { call_lua_global(L, "OnFirstUpdate"); }

inline void call_on_beat(lua_State* L, f64 dt) {
    lua_pushstring(L, "OnBeat");
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (lua_isfunction(L, -1)) {
        lua_pushnumber(L, dt);
        if (lua_pcall(L, 1, 0, 0) != 0) {
            spdlog::warn("OnBeat error: {}", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    } else {
        lua_pop(L, 1);
    }
}

} // namespace osc::core
