#include "lua/game_mods.hpp"

#include "sim/lua_bytes.hpp"

#include <spdlog/spdlog.h>

extern "C" {
#include <lua.h>
}

namespace osc::lua {

void set_active_mods(lua_State* L, const std::string& mods) {
    lua_pushstring(L, "__active_mods");
    bool read = false;
    if (!mods.empty() && sim::push_lua_bytes(L, mods)) {
        read = lua_istable(L, -1);
        if (!read) lua_pop(L, 1); // a value, but no list
    }
    if (!read) {
        if (!mods.empty()) spdlog::warn("The game's mods can't be read: it has none");
        lua_newtable(L);
    }
    lua_rawset(L, LUA_GLOBALSINDEX);
}

void ensure_active_mods(lua_State* L) {
    lua_pushstring(L, "__active_mods");
    lua_rawget(L, LUA_GLOBALSINDEX);
    const bool present = lua_istable(L, -1);
    lua_pop(L, 1);
    if (!present) set_active_mods(L, {});
}

namespace {
constexpr const char* kSessionInitKey = "__osc_session_init";
} // namespace

void mark_session_init(lua_State* L) {
    lua_pushstring(L, kSessionInitKey);
    lua_pushboolean(L, 1);
    lua_rawset(L, LUA_REGISTRYINDEX);
}

bool session_init_ran(lua_State* L) {
    lua_pushstring(L, kSessionInitKey);
    lua_rawget(L, LUA_REGISTRYINDEX);
    const bool ran = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    return ran;
}

} // namespace osc::lua
