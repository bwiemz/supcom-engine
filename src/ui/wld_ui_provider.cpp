#include "ui/wld_ui_provider.hpp"
#include <spdlog/spdlog.h>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

namespace osc::ui {

void WldUIProvider::create_game_interface(lua_State* L, bool is_replay) {
    if (game_interface_created_) return;

    // Call gamemain.lua's CreateGameInterface(isReplay)
    lua_pushstring(L, "CreateGameInterface");
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (lua_isfunction(L, -1)) {
        lua_pushboolean(L, is_replay ? 1 : 0);
        if (lua_pcall(L, 1, 0, 0) != 0) {
            spdlog::error("CreateGameInterface error: {}", lua_tostring(L, -1));
            lua_pop(L, 1);
        } else {
            game_interface_created_ = true;
            spdlog::info("WldUIProvider: CreateGameInterface completed");
        }
    } else {
        lua_pop(L, 1);
        spdlog::warn("WldUIProvider: CreateGameInterface not found in Lua");
    }
}

void WldUIProvider::destroy_game_interface(lua_State* L) {
    if (!game_interface_created_) return;

    lua_pushstring(L, "DestroyGameInterface");
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (lua_isfunction(L, -1)) {
        if (lua_pcall(L, 0, 0, 0) != 0) {
            spdlog::warn("DestroyGameInterface error: {}", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    } else {
        lua_pop(L, 1);
    }
    game_interface_created_ = false;
}

void WldUIProvider::start_loading_dialog(lua_State* L) {
    // Call the Lua-side provider's StartLoadingDialog method
    lua_pushstring(L, "__osc_wld_ui_provider");
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (lua_istable(L, -1)) {
        lua_pushstring(L, "StartLoadingDialog");
        lua_gettable(L, -2);
        if (lua_isfunction(L, -1)) {
            lua_pushvalue(L, -2); // self
            if (lua_pcall(L, 1, 0, 0) != 0) {
                spdlog::warn("StartLoadingDialog error: {}", lua_tostring(L, -1));
                lua_pop(L, 1);
            } else {
                spdlog::info("WldUIProvider: StartLoadingDialog completed");
            }
        } else {
            lua_pop(L, 1);
        }
        lua_pop(L, 1); // provider table
    } else {
        lua_pop(L, 1);
        spdlog::debug("WldUIProvider: no __osc_wld_ui_provider in registry");
    }
}

void WldUIProvider::update_loading_dialog(lua_State* L, f32 progress) {
    spdlog::debug("WldUIProvider: UpdateLoadingDialog({:.1f}%)", progress * 100);
}

void WldUIProvider::stop_loading_dialog(lua_State* L) {
    lua_pushstring(L, "__osc_wld_ui_provider");
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (lua_istable(L, -1)) {
        lua_pushstring(L, "StopLoadingDialog");
        lua_gettable(L, -2);
        if (lua_isfunction(L, -1)) {
            lua_pushvalue(L, -2); // self
            if (lua_pcall(L, 1, 0, 0) != 0) {
                spdlog::warn("StopLoadingDialog error: {}", lua_tostring(L, -1));
                lua_pop(L, 1);
            } else {
                spdlog::info("WldUIProvider: StopLoadingDialog completed");
            }
        } else {
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    } else {
        lua_pop(L, 1);
    }
}

} // namespace osc::ui
