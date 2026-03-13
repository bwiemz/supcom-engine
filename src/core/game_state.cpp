#include "core/game_state.hpp"
#include <spdlog/spdlog.h>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

namespace osc {

const char* game_state_to_string(GameState s) {
    switch (s) {
    case GameState::INIT:      return "init";
    case GameState::FRONT_END: return "front-end";
    case GameState::LOADING:   return "loading";
    case GameState::GAME:      return "game";
    case GameState::SCORE:     return "score";
    }
    return "unknown";
}

bool GameStateManager::transition_to(GameState new_state, lua_State* ui_L) {
    if (new_state == state_) return true;
    spdlog::info("GameState: {} -> {}", game_state_to_string(state_),
                 game_state_to_string(new_state));
    if (ui_L) call_setup_ui(ui_L);
    state_ = new_state;
    if (new_state == GameState::GAME) {
        paused_ = false;
        game_over_ = false;
        speed_ = 1.0;
    }
    return true;
}

void GameStateManager::set_paused(bool p, lua_State* ui_L) {
    if (paused_ == p) return;
    paused_ = p;
    if (ui_L) {
        lua_pushstring(ui_L, p ? "OnPause" : "OnResume");
        lua_rawget(ui_L, LUA_GLOBALSINDEX);
        if (lua_isfunction(ui_L, -1)) {
            if (lua_pcall(ui_L, 0, 0, 0) != 0) {
                spdlog::warn("{} error: {}", p ? "OnPause" : "OnResume",
                             lua_tostring(ui_L, -1));
                lua_pop(ui_L, 1);
            }
        } else {
            lua_pop(ui_L, 1);
        }
    }
}

void GameStateManager::set_speed(f64 s) {
    if (s < 0.0) s = 0.0;
    if (s > 10.0) s = 10.0;
    speed_ = s;
}

void GameStateManager::call_setup_ui(lua_State* ui_L) {
    lua_pushstring(ui_L, "SetupUI");
    lua_rawget(ui_L, LUA_GLOBALSINDEX);
    if (lua_isfunction(ui_L, -1)) {
        if (lua_pcall(ui_L, 0, 0, 0) != 0) {
            spdlog::warn("SetupUI error: {}", lua_tostring(ui_L, -1));
            lua_pop(ui_L, 1);
        }
    } else {
        lua_pop(ui_L, 1);
    }
}

} // namespace osc
