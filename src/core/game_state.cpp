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
        // gamemain.OnPause(pausedBy, timeoutsRemaining) / OnResume().
        // pausedBy is a command source; only local pauses exist so far, so
        // it is this client's (SessionGetLocalCommandSource), with no pause
        // timeouts.
        if (p) {
            lua_Number source = 1;
            lua_pushstring(ui_L, "SessionGetLocalCommandSource");
            lua_rawget(ui_L, LUA_GLOBALSINDEX);
            if (lua_isfunction(ui_L, -1) && lua_pcall(ui_L, 0, 1, 0) == 0 &&
                lua_isnumber(ui_L, -1)) {
                source = lua_tonumber(ui_L, -1);
            }
            lua_pop(ui_L, 1);
            lua_pushnumber(ui_L, source);
            lua_pushnumber(ui_L, -1);
            core::call_ui_callback(ui_L, core::kGameMainModule, "OnPause", 2);
        } else {
            core::call_ui_callback(ui_L, core::kGameMainModule, "OnResume", 0);
        }
    }
}

void GameStateManager::set_speed(f64 s) {
    if (s < 0.0) s = 0.0;
    if (s > 10.0) s = 10.0;
    speed_ = s;
}

void GameStateManager::call_setup_ui(lua_State* ui_L) {
    osc::core::call_setup_ui(ui_L);
}

} // namespace osc
