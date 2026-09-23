#pragma once

#include "core/types.hpp"
#include "core/test_status.hpp"
#include "core/ui_registry_keys.hpp"
#include "lua/smoke_test.hpp"
#include <spdlog/spdlog.h>

#include <cstring>

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
    void set_game_over(bool v) {
        game_over_ = v;
        if (!v) sim_stopped_ = false; // a new session
    }
    /// SessionEndGame (the score screen opening): the session is over and
    /// the sim no longer ticks.
    bool sim_stopped() const { return sim_stopped_; }
    void stop_sim() { sim_stopped_ = true; }

private:
    GameState state_ = GameState::INIT;
    bool paused_ = false;
    bool game_over_ = false;
    bool sim_stopped_ = false;
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

/// Push uimain's function `name` (SetupUI, StartGameUI): the global if a
/// script defined one (FAF's doscript'ed uimain), else
/// import('/lua/ui/uimain.lua')[name] as Moho calls it -- retail uimain
/// imports module-relatively ('uiutil.lua') and only works loaded as a
/// module. Pushes nil when neither exists.
inline void push_uimain_function(lua_State* L, const char* name) {
    lua_pushstring(L, name);
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (lua_isfunction(L, -1)) return;
    lua_pop(L, 1);
    // A chunk returning a function of the name (LuaPlus main chunks get no
    // `arg`, and config.lua makes reading it an error).
    static const char* kImport =
        "return function(name)\n"
        "  local ok, m = pcall(import, '/lua/ui/uimain.lua')\n"
        "  if ok and type(m) == 'table' then return m[name] end\n"
        "end\n";
    if (luaL_loadbuffer(L, kImport, std::strlen(kImport), "=uimain") != 0 ||
        lua_pcall(L, 0, 1, 0) != 0) {
        spdlog::warn("uimain loader error: {}", lua_tostring(L, -1));
        lua_pop(L, 1);
        lua_pushnil(L);
        return;
    }
    lua_pushstring(L, name);
    if (lua_pcall(L, 1, 1, 0) != 0) {
        spdlog::warn("uimain import error: {}", lua_tostring(L, -1));
        lua_pop(L, 1);
        lua_pushnil(L);
    }
}

inline void push_setup_ui(lua_State* L) { push_uimain_function(L, "SetupUI"); }

inline void call_setup_ui(lua_State* L) {
    push_setup_ui(L);
    if (!lua_isfunction(L, -1)) { lua_pop(L, 1); return; }
    if (lua_pcall(L, 0, 0, 0) != 0) {
        spdlog::warn("SetupUI error: {}", lua_tostring(L, -1));
        lua_pop(L, 1);
    }
}
/// uimain.StartGameUI: Moho calls it when a world session starts; it
/// creates the Lua WldUIProvider (gamemain.CreateWldUIProvider), which the
/// engine then drives (loading dialog, CreateGameInterface).
inline void call_start_game_ui(lua_State* L) {
    push_uimain_function(L, "StartGameUI");
    if (!lua_isfunction(L, -1)) { lua_pop(L, 1); return; }
    if (lua_pcall(L, 0, 0, 0) != 0) {
        spdlog::warn("StartGameUI error: {}", lua_tostring(L, -1));
        lua_pop(L, 1);
    }
}
inline void call_on_first_update(lua_State* L) { call_lua_global(L, "OnFirstUpdate"); }

/// Engine -> game UI callbacks. Moho calls them in UI modules:
inline constexpr const char* kGameMainModule = "/lua/ui/game/gamemain.lua";
inline constexpr const char* kUiMainModule = "/lua/ui/uimain.lua";


/// Push module[name] if `module` is already imported (retail import.lua's
/// __modules), else nil. Never imports: the engine calls into the game UI
/// only once it exists; importing gamemain from the front end would load
/// half the game UI.
inline void push_loaded_module_function(lua_State* L, const char* module,
                                        const char* name) {
    lua_pushstring(L, "__modules");
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (lua_istable(L, -1)) {
        lua_pushstring(L, module);
        lua_rawget(L, -2);
        if (lua_istable(L, -1)) {
            lua_pushstring(L, name);
            lua_rawget(L, -2);
            lua_remove(L, -2); // module
        }
        lua_remove(L, -2); // __modules
    }
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        lua_pushnil(L);
    }
}

/// Call an engine -> UI callback with the nargs arguments on top of the
/// stack (consumed): module[name] when the module is loaded, as Moho calls
/// it (a null module skips this), and also a global `name` when a script
/// defined one (the engine's own UI scripts and tests). Errors are logged,
/// reported to the smoke harness, and count as failures in test modes.
inline void call_ui_callback(lua_State* L, const char* module, const char* name,
                             int nargs) {
    const int base = lua_gettop(L) - nargs;
    for (int pass = 0; pass < 2; ++pass) {
        if (pass == 0) {
            if (!module) continue;
            push_loaded_module_function(L, module, name);
        } else {
            lua_pushstring(L, name);
            lua_rawget(L, LUA_GLOBALSINDEX);
        }
        if (!lua_isfunction(L, -1)) { lua_pop(L, 1); continue; }
        for (int i = 1; i <= nargs; ++i) lua_pushvalue(L, base + i);
        if (lua_pcall(L, nargs, 0, 0) != 0) {
            const char* raw = lua_tostring(L, -1);
            std::string err = fmt::format("{} error: {}", name,
                                          raw ? raw : "(unknown error)");
            spdlog::warn("{}", err);
            if (auto* harness = osc::lua::SmokeTestHarness::active_instance())
                harness->record(osc::lua::SmokeCategory::PcallError, err, name);
            if (osc::test_status::count_lua_failures())
                osc::test_status::record_failure("Lua UI callback error: " + err);
            lua_pop(L, 1);
        }
    }
    lua_settop(L, base);
}

/// The engine's own per-UI-frame heartbeat: a global OnBeat(dt) if a
/// script defined one. (Retail has none; its beat is call_game_beat.)
inline void call_on_beat(lua_State* L, f64 dt) {
    lua_pushnumber(L, dt);
    call_ui_callback(L, nullptr, "OnBeat", 1);
}

/// gamemain.OnBeat(), once per sim beat while FA's game interface exists:
/// runs the game UI's beat functions (economy, score, avatars, objectives).
inline void call_game_beat(lua_State* L) {
    lua_pushstring(L, kWorldUiActiveKey);
    lua_rawget(L, LUA_REGISTRYINDEX);
    const bool world_ui = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    if (!world_ui) return;
    push_loaded_module_function(L, kGameMainModule, "OnBeat");
    if (!lua_isfunction(L, -1)) { lua_pop(L, 1); return; }
    lua_pop(L, 1);
    call_ui_callback(L, kGameMainModule, "OnBeat", 0);
}

/// uimain.NoteGameOver: the session ended (score screen follows).
inline void call_note_game_over(lua_State* L) {
    call_ui_callback(L, kUiMainModule, "NoteGameOver", 0);
}

} // namespace osc::core
