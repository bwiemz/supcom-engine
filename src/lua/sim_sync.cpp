#include "lua/sim_sync.hpp"

#include "core/test_status.hpp"

#include <spdlog/spdlog.h>

#include <string>
#include <unordered_map>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace osc::lua {

namespace {

/// Deeper nesting than any sync table carries; stops runaway structures.
constexpr int kMaxCopyDepth = 64;

bool is_plain_key(lua_State* L, int idx) {
    const int t = lua_type(L, idx);
    return t == LUA_TNUMBER || t == LUA_TSTRING || t == LUA_TBOOLEAN;
}

bool is_plain_value(lua_State* L, int idx) {
    const int t = lua_type(L, idx);
    return t == LUA_TNIL || t == LUA_TNUMBER || t == LUA_TSTRING ||
           t == LUA_TBOOLEAN || t == LUA_TTABLE;
}

class Copier {
public:
    Copier(lua_State* from, lua_State* to) : from_(from), to_(to) {}
    ~Copier() {
        for (const auto& entry : copies_) luaL_unref(to_, LUA_REGISTRYINDEX, entry.second);
    }
    Copier(const Copier&) = delete;
    Copier& operator=(const Copier&) = delete;

    /// Push the copy of from_[idx] (an absolute index) onto to_.
    void copy(int idx, int depth) {
        switch (lua_type(from_, idx)) {
        case LUA_TNUMBER: lua_pushnumber(to_, lua_tonumber(from_, idx)); return;
        case LUA_TBOOLEAN: lua_pushboolean(to_, lua_toboolean(from_, idx)); return;
        case LUA_TSTRING:
            lua_pushlstring(to_, lua_tostring(from_, idx), lua_strlen(from_, idx));
            return;
        case LUA_TTABLE: copy_table(idx, depth); return;
        default: lua_pushnil(to_); return;
        }
    }

private:
    void copy_table(int idx, int depth) {
        const void* key = lua_topointer(from_, idx);
        if (auto it = copies_.find(key); it != copies_.end()) {
            lua_rawgeti(to_, LUA_REGISTRYINDEX, it->second);
            return;
        }
        lua_newtable(to_);
        lua_pushvalue(to_, -1);
        copies_.emplace(key, luaL_ref(to_, LUA_REGISTRYINDEX));
        // Each level holds a key/value pair on `from` and a table plus a
        // key on `to`; past the depth cap, or without stack, stay empty.
        if (depth >= kMaxCopyDepth || !lua_checkstack(from_, 4) ||
            !lua_checkstack(to_, 4)) {
            return;
        }

        const int dst = lua_gettop(to_);
        lua_pushnil(from_);
        while (lua_next(from_, idx) != 0) {
            const int value = lua_gettop(from_);
            const int k = value - 1;
            if (is_plain_key(from_, k) && is_plain_value(from_, value)) {
                copy(k, depth + 1);
                copy(value, depth + 1);
                lua_rawset(to_, dst);
            }
            lua_pop(from_, 1); // value; key stays for lua_next
        }
    }

    lua_State* from_;
    lua_State* to_;
    std::unordered_map<const void*, int> copies_; // source table -> registry ref
};

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

void copy_lua_value(lua_State* from, int from_idx, lua_State* to) {
    if (from_idx < 0) from_idx = lua_gettop(from) + from_idx + 1;
    Copier(from, to).copy(from_idx, 0);
}

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
