#include "lua/sim_sync.hpp"

#include "core/lua_copy.hpp"
#include "core/test_status.hpp"

#include <spdlog/spdlog.h>

#include <string>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace osc::lua {

namespace {

/// pcall global `name` with the nargs arguments on top of the stack
/// (consumed). A missing function is not an error.
void call_global(lua_State* L, const char* name, int nargs) {
    const int base = lua_gettop(L) - nargs;
    lua_pushstring(L, name);
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (!lua_isfunction(L, -1)) {
        lua_settop(L, base);
        return;
    }
    lua_insert(L, base + 1);
    if (lua_pcall(L, nargs, 0, 0) != 0) {
        const char* raw = lua_tostring(L, -1);
        std::string err = std::string(name) + " error: " + (raw ? raw : "(unknown)");
        spdlog::warn("{}", err);
        if (test_status::count_lua_failures())
            test_status::record_failure("Lua sync error: " + err);
    }
    lua_settop(L, base);
}

int registry_int(lua_State* L, const char* key, int fallback) {
    lua_pushstring(L, key);
    lua_rawget(L, LUA_REGISTRYINDEX);
    const int v = lua_isnumber(L, -1) ? static_cast<int>(lua_tonumber(L, -1)) : fallback;
    lua_pop(L, 1);
    return v;
}

void set_registry_int(lua_State* L, const char* key, int v) {
    lua_pushstring(L, key);
    lua_pushnumber(L, v);
    lua_rawset(L, LUA_REGISTRYINDEX);
}

/// 0-based army (-1 observer) -> Lua's 1-based (-1 stays).
lua_Number to_lua_army(int army) { return army >= 0 ? army + 1 : -1; }

void apply_focus_request(lua_State* sim_L, lua_State* ui_L) {
    lua_pushstring(ui_L, kFocusArmyRequestKey);
    lua_rawget(ui_L, LUA_REGISTRYINDEX);
    if (!lua_isnumber(ui_L, -1)) {
        lua_pop(ui_L, 1);
        return;
    }
    const int requested = static_cast<int>(lua_tonumber(ui_L, -1));
    lua_pop(ui_L, 1);
    lua_pushstring(ui_L, kFocusArmyRequestKey);
    lua_pushnil(ui_L);
    lua_rawset(ui_L, LUA_REGISTRYINDEX);

    const int old = registry_int(ui_L, "__osc_focus_army", 0);
    if (requested == old) return;
    set_registry_int(ui_L, "__osc_focus_army", requested);
    set_registry_int(sim_L, "__osc_focus_army", requested);

    lua_pushnumber(sim_L, to_lua_army(requested));
    lua_pushnumber(sim_L, to_lua_army(old));
    call_global(sim_L, "NoteFocusArmyChanged", 2);

    lua_pushstring(sim_L, "Sync");
    lua_rawget(sim_L, LUA_GLOBALSINDEX);
    if (lua_istable(sim_L, -1)) {
        lua_pushstring(sim_L, "FocusArmyChanged");
        lua_newtable(sim_L);
        lua_pushstring(sim_L, "new");
        lua_pushnumber(sim_L, to_lua_army(requested));
        lua_rawset(sim_L, -3);
        lua_pushstring(sim_L, "old");
        lua_pushnumber(sim_L, to_lua_army(old));
        lua_rawset(sim_L, -3);
        lua_rawset(sim_L, -3);
    }
    lua_pop(sim_L, 1);
}

} // namespace

void sync_beat(lua_State* sim_L, lua_State* ui_L) {
    if (!sim_L || !ui_L) return;
    apply_focus_request(sim_L, ui_L);

    // PreviousSync = Sync; Sync = copy of the sim's Sync
    lua_pushstring(ui_L, "PreviousSync");
    lua_pushstring(ui_L, "Sync");
    lua_rawget(ui_L, LUA_GLOBALSINDEX);
    lua_rawset(ui_L, LUA_GLOBALSINDEX);

    lua_pushstring(sim_L, "Sync");
    lua_rawget(sim_L, LUA_GLOBALSINDEX);
    lua_pushstring(ui_L, "Sync");
    if (lua_istable(sim_L, -1))
        copy_lua_value(sim_L, -1, ui_L);
    else
        lua_newtable(ui_L);
    lua_rawset(ui_L, LUA_GLOBALSINDEX);
    lua_pop(sim_L, 1);

    call_global(sim_L, "ResetSyncTable", 0);
    call_global(ui_L, "OnSync", 0);
}

} // namespace osc::lua
