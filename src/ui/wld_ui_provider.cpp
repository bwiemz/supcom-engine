#include "ui/wld_ui_provider.hpp"
#include "core/game_state.hpp"
#include <spdlog/spdlog.h>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

namespace osc::ui {

bool WldUIProvider::call_method(lua_State* L, const char* name, int nargs) {
    const int base = lua_gettop(L) - nargs; // args are base+1 .. top
    lua_pushstring(L, kLuaObjectKey);
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (!lua_istable(L, -1)) {
        spdlog::debug("WldUIProvider: no provider object for {}", name);
        lua_settop(L, base);
        return false;
    }
    lua_pushstring(L, name);
    lua_gettable(L, -2);
    if (!lua_isfunction(L, -1)) {
        spdlog::debug("WldUIProvider: provider has no {}", name);
        lua_settop(L, base);
        return false;
    }
    // [args..., obj, fn] -> [fn, obj, args...]
    lua_insert(L, base + 1);
    lua_insert(L, base + 2);
    if (lua_pcall(L, nargs + 1, 0, 0) != 0) {
        spdlog::error("WldUIProvider {} error: {}", name, lua_tostring(L, -1));
        lua_settop(L, base);
        return false;
    }
    return true;
}

static void set_world_ui_active(lua_State* L, bool active) {
    lua_pushstring(L, core::kWorldUiActiveKey);
    lua_pushboolean(L, active ? 1 : 0);
    lua_rawset(L, LUA_REGISTRYINDEX);
}

bool WldUIProvider::create_game_interface(lua_State* L, bool is_replay) {
    if (game_interface_created_) return true;
    lua_pushstring(L, kLuaObjectKey);
    lua_rawget(L, LUA_REGISTRYINDEX);
    const bool have_provider = lua_istable(L, -1);
    lua_pop(L, 1);
    if (!have_provider) return false;

    lua_pushboolean(L, is_replay ? 1 : 0);
    const bool ok = call_method(L, "CreateGameInterface", 1);
    if (ok) spdlog::info("WldUIProvider: CreateGameInterface completed");
    // Even a partly built interface (an error mid-CreateUI) has controls and
    // beat functions, so it exists until DestroyGameInterface.
    game_interface_created_ = true;
    set_world_ui_active(L, true);
    return ok;
}

bool WldUIProvider::destroy_game_interface(lua_State* L) {
    set_world_ui_active(L, false);
    if (!game_interface_created_) return true;
    game_interface_created_ = false;
    return call_method(L, "DestroyGameInterface", 0);
}

bool WldUIProvider::start_loading_dialog(lua_State* L) {
    return call_method(L, "StartLoadingDialog", 0);
}

bool WldUIProvider::update_loading_dialog(lua_State* L, f32 elapsed_seconds) {
    lua_pushnumber(L, elapsed_seconds);
    return call_method(L, "UpdateLoadingDialog", 1);
}

bool WldUIProvider::stop_loading_dialog(lua_State* L) {
    return call_method(L, "StopLoadingDialog", 0);
}

} // namespace osc::ui
