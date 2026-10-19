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

/// Take (and clear) a focus request pending in `L`'s registry.
bool take_focus_request(lua_State* L, int& requested) {
    lua_pushstring(L, kFocusArmyRequestKey);
    lua_rawget(L, LUA_REGISTRYINDEX);
    const bool pending = lua_isnumber(L, -1) != 0;
    if (pending) requested = static_cast<int>(lua_tonumber(L, -1));
    lua_pop(L, 1);
    if (pending) {
        lua_pushstring(L, kFocusArmyRequestKey);
        lua_pushnil(L);
        lua_rawset(L, LUA_REGISTRYINDEX);
    }
    return pending;
}

void apply_focus_request(lua_State* sim_L, lua_State* ui_L) {
    // The sim's (FAF's SetFocusArmy), then the UI's: both set the driver's
    // pending focus in Moho, the later winning
    int requested = 0;
    const bool from_sim = take_focus_request(sim_L, requested);
    const bool from_ui = take_focus_request(ui_L, requested);
    if (!from_sim && !from_ui) return;

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

constexpr const char* kSyncPacketsKey = "__osc_sync_packets";

/// The sim state's queue of beats' Sync tables, [first..last].
int push_packets(lua_State* sim_L) {
    lua_pushstring(sim_L, kSyncPacketsKey);
    lua_rawget(sim_L, LUA_REGISTRYINDEX);
    if (!lua_istable(sim_L, -1)) {
        lua_pop(sim_L, 1);
        lua_newtable(sim_L);
        lua_pushstring(sim_L, kSyncPacketsKey);
        lua_pushvalue(sim_L, -2);
        lua_rawset(sim_L, LUA_REGISTRYINDEX);
    }
    return lua_gettop(sim_L);
}

int packet_index(lua_State* sim_L, int packets, const char* end, int fallback) {
    lua_pushstring(sim_L, end);
    lua_rawget(sim_L, packets);
    const int v = lua_isnumber(sim_L, -1) ? static_cast<int>(lua_tonumber(sim_L, -1)) : fallback;
    lua_pop(sim_L, 1);
    return v;
}

void set_packet_index(lua_State* sim_L, int packets, const char* end, int v) {
    lua_pushstring(sim_L, end);
    lua_pushnumber(sim_L, v);
    lua_rawset(sim_L, packets);
}

void push_sim_sync(lua_State* sim_L) {
    lua_pushstring(sim_L, "Sync");
    lua_rawget(sim_L, LUA_GLOBALSINDEX);
    if (!lua_istable(sim_L, -1)) {
        lua_pop(sim_L, 1);
        lua_newtable(sim_L);
    }
}

} // namespace

int focus_army(lua_State* ui_L) {
    return registry_int(ui_L, "__osc_focus_army", 0);
}

void set_focus_army(lua_State* sim_L, lua_State* ui_L, int army) {
    if (sim_L) set_registry_int(sim_L, "__osc_focus_army", army);
    if (ui_L) set_registry_int(ui_L, "__osc_focus_army", army);
}

void queue_sync(lua_State* sim_L, lua_State* ui_L) {
    if (!sim_L || !ui_L) {
        return;
    }
    apply_focus_request(sim_L, ui_L);
    const int packets = push_packets(sim_L);
    const int last = packet_index(sim_L, packets, "last", 0) + 1;
    push_sim_sync(sim_L);
    lua_rawseti(sim_L, packets, last);
    set_packet_index(sim_L, packets, "last", last);
    lua_pop(sim_L, 1);
    call_global(sim_L, "ResetSyncTable", 0);
}

bool sync_pending(lua_State* sim_L) {
    const int packets = push_packets(sim_L);
    const bool pending =
        packet_index(sim_L, packets, "first", 1) <= packet_index(sim_L, packets, "last", 0);
    lua_pop(sim_L, 1);
    return pending;
}

void sync_beat(lua_State* sim_L, lua_State* ui_L) {
    if (!sim_L || !ui_L) return;
    const int packets = push_packets(sim_L);
    const int first = packet_index(sim_L, packets, "first", 1);
    const bool queued = first <= packet_index(sim_L, packets, "last", 0);
    if (queued) {
        lua_rawgeti(sim_L, packets, first);
        lua_pushnil(sim_L);
        lua_rawseti(sim_L, packets, first);
        set_packet_index(sim_L, packets, "first", first + 1);
    } else {
        apply_focus_request(sim_L, ui_L);
        push_sim_sync(sim_L);
    }

    // PreviousSync = Sync; Sync = copy of the beat's
    lua_pushstring(ui_L, "PreviousSync");
    lua_pushstring(ui_L, "Sync");
    lua_rawget(ui_L, LUA_GLOBALSINDEX);
    lua_rawset(ui_L, LUA_GLOBALSINDEX);
    lua_pushstring(ui_L, "Sync");
    copy_lua_value(sim_L, -1, ui_L);
    lua_rawset(ui_L, LUA_GLOBALSINDEX);
    lua_pop(sim_L, 2);

    if (!queued) {
        call_global(sim_L, "ResetSyncTable", 0);
    }
    call_global(ui_L, "OnSync", 0);
}

} // namespace osc::lua
