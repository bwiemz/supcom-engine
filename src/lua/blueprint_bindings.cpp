#include "lua/blueprint_bindings.hpp"
#include "lua/lua_state.hpp"
#include "blueprints/blueprint_store.hpp"

#include <spdlog/spdlog.h>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

namespace osc::lua {

static int register_blueprint(lua_State* L, blueprints::BlueprintType type) {
    luaL_checktype(L, 1, LUA_TTABLE);
    auto* store = LuaState::get_blueprint_store(L);
    if (!store) {
        return luaL_error(L, "BlueprintStore not initialized");
    }
    store->register_blueprint(L, type, 1);
    return 0;
}

static int l_RegisterUnitBlueprint(lua_State* L) {
    register_blueprint(L, blueprints::BlueprintType::Unit);

    // The original engine always ensures Defense.Shield exists with a dummy
    // entry (ShieldSize=0, RegenAssistMult=1).  Unit.lua's OnStopBeingBuilt
    // accesses bp.Defense.Shield.ShieldSize without a nil check, so we must
    // guarantee the path exists.
    luaL_checktype(L, 1, LUA_TTABLE);

    // Ensure Defense table exists
    lua_pushstring(L, "Defense");
    lua_rawget(L, 1);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushstring(L, "Defense");
        lua_pushvalue(L, -2);
        lua_rawset(L, 1);
        // new Defense table is on top
    }
    int defense_idx = lua_gettop(L);

    // Ensure Defense.Shield table exists
    lua_pushstring(L, "Shield");
    lua_rawget(L, defense_idx);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);

        lua_pushstring(L, "ShieldSize");
        lua_pushnumber(L, 0);
        lua_rawset(L, -3);

        lua_pushstring(L, "RegenAssistMult");
        lua_pushnumber(L, 1);
        lua_rawset(L, -3);

        lua_pushstring(L, "Shield");
        lua_pushvalue(L, -2);
        lua_rawset(L, defense_idx);
    }
    lua_pop(L, 2); // pop Shield table and Defense table

    // Ensure Display.MovementEffects exists (Unit.lua line 2469 indexes it
    // without a nil check — "movementEffects.Land or movementEffects.Air ...")
    lua_pushstring(L, "Display");
    lua_rawget(L, 1);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushstring(L, "Display");
        lua_pushvalue(L, -2);
        lua_rawset(L, 1);
        // new Display table is on top
    }
    int display_idx = lua_gettop(L);

    lua_pushstring(L, "MovementEffects");
    lua_rawget(L, display_idx);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_pushstring(L, "MovementEffects");
        lua_newtable(L);
        lua_rawset(L, display_idx);
    } else {
        lua_pop(L, 1);
    }
    lua_pop(L, 1); // Display

    return 0;
}

static int l_RegisterProjectileBlueprint(lua_State* L) {
    return register_blueprint(L, blueprints::BlueprintType::Projectile);
}

static int l_RegisterMeshBlueprint(lua_State* L) {
    return register_blueprint(L, blueprints::BlueprintType::Mesh);
}

static int l_RegisterPropBlueprint(lua_State* L) {
    return register_blueprint(L, blueprints::BlueprintType::Prop);
}

static int l_RegisterBeamBlueprint(lua_State* L) {
    return register_blueprint(L, blueprints::BlueprintType::Beam);
}

static int l_RegisterEmitterBlueprint(lua_State* L) {
    return register_blueprint(L, blueprints::BlueprintType::Emitter);
}

static int l_RegisterTrailEmitterBlueprint(lua_State* L) {
    return register_blueprint(L, blueprints::BlueprintType::TrailEmitter);
}

static int l_SpecFootprints(lua_State* L) {
    // Store footprints — for now just log and ignore
    spdlog::debug("SpecFootprints called");
    return 0;
}

void register_blueprint_store_bindings(LuaState& state) {
    state.register_function("RegisterUnitBlueprint", l_RegisterUnitBlueprint);
    state.register_function("RegisterProjectileBlueprint",
                            l_RegisterProjectileBlueprint);
    state.register_function("RegisterMeshBlueprint", l_RegisterMeshBlueprint);
    state.register_function("RegisterPropBlueprint", l_RegisterPropBlueprint);
    state.register_function("RegisterBeamBlueprint", l_RegisterBeamBlueprint);
    state.register_function("RegisterEmitterBlueprint",
                            l_RegisterEmitterBlueprint);
    state.register_function("RegisterTrailEmitterBlueprint",
                            l_RegisterTrailEmitterBlueprint);
    state.register_function("SpecFootprints", l_SpecFootprints);
}

} // namespace osc::lua
