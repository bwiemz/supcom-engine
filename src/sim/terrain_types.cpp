#include "sim/terrain_types.hpp"

extern "C" {
#include <lua.h>
}

namespace osc::sim {

namespace {

/// Push /lua/TerrainTypes.lua's entries indexed by TypeCode (loaded once),
/// or nil without FA's scripts.
void push_terrain_types(lua_State* L) {
    constexpr const char* kKey = "__osc_terrain_types";
    lua_pushstring(L, kKey);
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (lua_istable(L, -1)) return;
    const bool tried = !lua_isnil(L, -1);
    lua_pop(L, 1);
    if (tried) {
        lua_pushnil(L);
        return;
    }
    const int top = lua_gettop(L);
    lua_newtable(L); // TypeCode -> entry
    const int by_code = lua_gettop(L);
    bool loaded = false;
    lua_pushstring(L, "import");
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (lua_isfunction(L, -1)) {
        lua_pushstring(L, "/lua/terraintypes.lua");
        if (lua_pcall(L, 1, 1, 0) == 0 && lua_istable(L, -1)) {
            lua_pushstring(L, "TerrainTypes");
            lua_rawget(L, -2);
            if (lua_istable(L, -1)) {
                const int list = lua_gettop(L);
                for (int i = 1;; ++i) {
                    lua_rawgeti(L, list, i);
                    if (lua_isnil(L, -1)) {
                        lua_pop(L, 1);
                        break;
                    }
                    if (lua_istable(L, -1)) {
                        lua_pushstring(L, "TypeCode");
                        lua_rawget(L, -2);
                        if (lua_isnumber(L, -1)) {
                            const int code = static_cast<int>(lua_tonumber(L, -1));
                            lua_pushvalue(L, -2);
                            lua_rawseti(L, by_code, code);
                            loaded = true;
                        }
                        lua_pop(L, 1);
                    }
                    lua_pop(L, 1);
                }
            }
        }
    }
    lua_settop(L, by_code);
    lua_pushstring(L, kKey);
    if (loaded) lua_pushvalue(L, by_code);
    else lua_pushboolean(L, 0); // not there (tests without FA's scripts): don't retry
    lua_rawset(L, LUA_REGISTRYINDEX);
    if (!loaded) {
        lua_settop(L, top);
        lua_pushnil(L);
    }
}

} // namespace

void push_terrain_type(lua_State* L, u8 code) {
    push_terrain_types(L);
    if (lua_istable(L, -1)) {
        lua_rawgeti(L, -1, code);
        if (!lua_istable(L, -1)) { // a code the scripts don't define
            lua_pop(L, 1);
            lua_rawgeti(L, -1, 1);
        }
        if (lua_istable(L, -1)) {
            lua_remove(L, -2);
            return;
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    lua_newtable(L);
    lua_pushstring(L, "Name");
    lua_pushstring(L, "Default");
    lua_rawset(L, -3);
}

} // namespace osc::sim
