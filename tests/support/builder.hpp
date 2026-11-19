#pragma once

#include "sim/unit.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <initializer_list>

namespace osc::test {

/// What a mobile build's order is asked as it comes up: a blueprint whose
/// BuildableCategory names each of `targets`, and a CheckBuildRestriction that allows.
inline void make_builder(lua_State* L, sim::Unit& builder, const char* builder_bp,
                         std::initializer_list<const char*> targets) {
    lua_pushstring(L, "__blueprints");
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushstring(L, "__blueprints");
        lua_pushvalue(L, -2);
        lua_rawset(L, LUA_GLOBALSINDEX);
    }
    const int blueprints = lua_gettop(L);
    const auto bare = [L](const char* id) {
        lua_newtable(L);
        lua_pushstring(L, "BlueprintId");
        lua_pushstring(L, id);
        lua_rawset(L, -3);
    };
    lua_pushstring(L, builder_bp);
    bare(builder_bp);
    lua_pushstring(L, "Economy");
    lua_newtable(L);
    lua_pushstring(L, "BuildableCategory");
    lua_newtable(L);
    int n = 0;
    for (const char* target : targets) {
        lua_pushstring(L, target);
        lua_rawseti(L, -2, ++n);
    }
    lua_rawset(L, -3);
    lua_rawset(L, -3);
    lua_rawset(L, blueprints);
    for (const char* target : targets) {
        lua_pushstring(L, target);
        lua_rawget(L, blueprints);
        const bool known = lua_istable(L, -1);
        lua_pop(L, 1);
        if (!known) {
            lua_pushstring(L, target);
            bare(target);
            lua_rawset(L, blueprints);
        }
    }
    lua_pop(L, 1);
    builder.set_blueprint_id(builder_bp);

    if (builder.lua_table_ref() < 0) {
        lua_newtable(L);
        lua_pushstring(L, "_c_object");
        lua_pushlightuserdata(L, &builder);
        lua_rawset(L, -3);
        builder.set_lua_table_ref(luaL_ref(L, LUA_REGISTRYINDEX));
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, builder.lua_table_ref());
    lua_pushstring(L, "CheckBuildRestriction");
    lua_gettable(L, -2);
    const bool has = lua_isfunction(L, -1);
    lua_pop(L, 1);
    if (!has) {
        lua_pushstring(L, "CheckBuildRestriction");
        lua_pushcfunction(L, [](lua_State* S) {
            lua_pushboolean(S, 1);
            return 1;
        });
        lua_rawset(L, -3);
    }
    lua_pop(L, 1);
}

} // namespace osc::test
