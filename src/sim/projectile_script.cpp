#include "sim/projectile_script.hpp"

#include "core/test_status.hpp"
#include "sim/projectile.hpp"
#include "sim/script_class.hpp"
#include "sim/sim_state.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <spdlog/spdlog.h>

#include <string>

namespace osc::sim {

namespace {

/// Push the bare moho.projectile_methods metatable (built once).
void push_bare_projectile_metatable(lua_State* L) {
    lua_pushstring(L, "__osc_proj_mt");
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (lua_istable(L, -1)) return;
    lua_pop(L, 1);
    lua_newtable(L);
    const int mt = lua_gettop(L);
    lua_pushstring(L, "__index");
    lua_pushvalue(L, mt);
    lua_rawset(L, mt);
    lua_pushstring(L, "moho");
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (lua_istable(L, -1)) {
        lua_pushstring(L, "projectile_methods");
        lua_rawget(L, -2);
        if (lua_istable(L, -1)) {
            const int src = lua_gettop(L);
            lua_pushnil(L);
            while (lua_next(L, src) != 0) {
                lua_pushvalue(L, -2);
                lua_pushvalue(L, -2);
                lua_rawset(L, mt);
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1); // projectile_methods
    }
    lua_pop(L, 1); // moho
    lua_pushstring(L, "__osc_proj_mt");
    lua_pushvalue(L, mt);
    lua_rawset(L, LUA_REGISTRYINDEX);
}

/// Push /lua/sim/Projectile.lua's Projectile class (loaded once), or nil.
void push_generic_projectile_class(lua_State* L) {
    constexpr const char* kKey = "__osc_generic_projectile_class";
    lua_pushstring(L, kKey);
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (!lua_isnil(L, -1)) {
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            lua_pushnil(L);
        }
        return;
    }
    lua_pop(L, 1);
    const int top = lua_gettop(L);
    lua_pushstring(L, "import");
    lua_rawget(L, LUA_GLOBALSINDEX);
    bool found = false;
    if (lua_isfunction(L, -1)) {
        lua_pushstring(L, "/lua/sim/Projectile.lua");
        if (lua_pcall(L, 1, 1, 0) == 0 && lua_istable(L, -1)) {
            lua_pushstring(L, "Projectile");
            lua_rawget(L, -2);
            found = lua_istable(L, -1);
        }
    }
    if (found) {
        lua_pushstring(L, kKey);
        lua_pushvalue(L, -2);
        lua_rawset(L, LUA_REGISTRYINDEX);
        lua_replace(L, top + 1);
        lua_settop(L, top + 1);
        return;
    }
    lua_settop(L, top);
    lua_pushstring(L, kKey);
    lua_pushboolean(L, 0); // not there (tests without FA's scripts): don't retry
    lua_rawset(L, LUA_REGISTRYINDEX);
    lua_pushnil(L);
}

void push_projectile_class(lua_State* L, const std::string& bp_id) {
    if (!bp_id.empty()) {
        push_blueprint_script_class(L, bp_id, "_proj.bp", "__osc_proj_script_classes",
                                    "Projectile");
        if (lua_istable(L, -1)) return;
        lua_pop(L, 1);
    }
    push_generic_projectile_class(L);
    if (lua_istable(L, -1)) return;
    lua_pop(L, 1);
    push_bare_projectile_metatable(L);
}

} // namespace

void create_projectile_object(lua_State* L, Projectile& proj, bool in_water, bool push) {
    if (!L) return;
    const int top = lua_gettop(L);
    lua_newtable(L);
    const int obj = lua_gettop(L);
    push_projectile_class(L, proj.blueprint_id());
    lua_setmetatable(L, obj);
    lua_pushstring(L, "_c_object");
    lua_pushlightuserdata(L, &proj);
    lua_rawset(L, obj);
    lua_pushstring(L, "_c_sim_gen");
    lua_pushnumber(L, static_cast<lua_Number>(SimState::sim_generation()));
    lua_rawset(L, obj);
    lua_pushvalue(L, obj);
    proj.set_lua_table_ref(luaL_ref(L, LUA_REGISTRYINDEX));

    // OnCreate(inWater): damage data, trails, sounds, homing ground targets.
    lua_pushstring(L, "OnCreate");
    lua_gettable(L, obj);
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, obj);
        lua_pushboolean(L, in_water ? 1 : 0);
        if (lua_pcall(L, 2, 0, 0) != 0) {
            const char* err = lua_tostring(L, -1);
            const std::string message = std::string("Projectile ") + proj.blueprint_id() +
                                        " OnCreate error: " + (err ? err : "(unknown)");
            spdlog::warn("{}", message);
            if (test_status::count_lua_failures()) test_status::record_failure(message);
        }
    }
    lua_settop(L, push ? obj : top);
}

} // namespace osc::sim
