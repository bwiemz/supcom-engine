#include "core/game_state.hpp"

#include <algorithm>
#include <cmath>
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

namespace {
UIState g_ui_state = UIState::None;
} // namespace

UIState ui_state() {
    return g_ui_state;
}

void set_ui_state(UIState s) {
    g_ui_state = s;
}

const char* ui_state_to_string(UIState s) {
    switch (s) {
    case UIState::None: return "none";
    case UIState::Splash: return "splash";
    case UIState::FrontEnd: return "frontend";
    case UIState::Game: return "game";
    case UIState::Lobby: return "lobby";
    }
    return "none";
}

bool GameStateManager::transition_to(GameState new_state, lua_State* ui_L) {
    if (new_state == state_) return true;
    spdlog::info("GameState: {} -> {}", game_state_to_string(state_),
                 game_state_to_string(new_state));
    state_ = new_state;
    if (new_state == GameState::GAME) {
        paused_ = false;
        game_over_ = false;
        set_sim_rate(0); // a new session starts at normal speed
    }
    // SetupUI after the state is the new one: what it asks (SessionIsActive)
    // answers for the state it sets up. Called before, the front end after a
    // game was set up as in a session, and FAF's world border looked for a
    // map that was gone (uiutil.lua's UpdateWorldBorderState; WOPC OE-145).
    if (ui_L) call_setup_ui(ui_L);
    return true;
}

void GameStateManager::set_sim_rate(i32 rate) {
    sim_rate_ = std::clamp(rate, kMinSimRate, kMaxSimRate);
    speed_ = std::pow(10.0, sim_rate_ * 0.1);
}

void GameStateManager::call_setup_ui(lua_State* ui_L) {
    osc::core::call_setup_ui(ui_L);
}

} // namespace osc
