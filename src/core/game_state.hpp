#pragma once

#include "core/types.hpp"
#include "lua/smoke_test.hpp"
#include <spdlog/spdlog.h>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

namespace osc {

enum class GameState : u8 {
    INIT,
    FRONT_END,
    LOADING,
    GAME,
    SCORE,
};

const char* game_state_to_string(GameState s);

class GameStateManager {
public:
    GameState current() const { return state_; }
    bool transition_to(GameState new_state, lua_State* ui_L);
    bool paused() const { return paused_; }
    void set_paused(bool p, lua_State* ui_L);
    f64 speed() const { return speed_; }
    void set_speed(f64 s);
    bool game_over() const { return game_over_; }
    void set_game_over(bool v) { game_over_ = v; }

private:
    GameState state_ = GameState::INIT;
    bool paused_ = false;
    bool game_over_ = false;
    f64 speed_ = 1.0;
    static void call_setup_ui(lua_State* ui_L);
};

} // namespace osc

// Legacy helpers in osc::core namespace (used by existing main.cpp call sites)
namespace osc::core {

inline void call_lua_global(lua_State* L, const char* name) {
    lua_pushstring(L, name);
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (lua_isfunction(L, -1)) {
        if (lua_pcall(L, 0, 0, 0) != 0) {
            const char* raw = lua_tostring(L, -1);
            std::string err = raw ? raw : "(unknown error)";
            spdlog::warn("{} error: {}", name, err);
            auto* harness = osc::lua::SmokeTestHarness::active_instance();
            if (harness) {
                harness->record(osc::lua::SmokeCategory::PcallError, err, name);
            }
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
            const char* raw = lua_tostring(L, -1);
            std::string err = raw ? raw : "(unknown error)";
            spdlog::warn("OnBeat error: {}", err);
            auto* harness = osc::lua::SmokeTestHarness::active_instance();
            if (harness) {
                harness->record(osc::lua::SmokeCategory::PcallError, err, "OnBeat");
            }
            lua_pop(L, 1);
        }
    } else {
        lua_pop(L, 1);
    }
}

} // namespace osc::core
