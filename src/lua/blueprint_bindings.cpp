#include "lua/blueprint_bindings.hpp"
#include "lua/lua_state.hpp"
#include "blueprints/blueprint_store.hpp"

#include <spdlog/spdlog.h>

#include <utility>

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

// The blueprint's table reads afterwards as Moho's write-back leaves it
// (BlueprintStore::register_blueprint, moho_reflected_fields.hpp).
static int l_RegisterUnitBlueprint(lua_State* L) {
    return register_blueprint(L, blueprints::BlueprintType::Unit);
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

// SpecFootprints{ {Name=, SizeX=, SizeZ=, Caps=, MaxWaterDepth=,
// MinWaterDepth=, MaxSlope=, Flags=}, ... }: the footprint classes, in order
// (Moho's cfunc_SpecFootprintsL). /lua/footprints.lua calls it once.
static int l_SpecFootprints(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    auto* store = LuaState::get_blueprint_store(L);
    if (!store) return luaL_error(L, "BlueprintStore not initialized");
    const auto number = [&](int t, const char* key) {
        lua_pushstring(L, key);
        lua_rawget(L, t);
        const lua_Number v = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : 0;
        lua_pop(L, 1);
        return v;
    };
    for (int i = 1;; ++i) {
        lua_rawgeti(L, 1, i);
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            break;
        }
        const int t = lua_gettop(L);
        blueprints::NamedFootprint fp;
        lua_pushstring(L, "Name");
        lua_rawget(L, t);
        if (lua_isstring(L, -1)) fp.name = lua_tostring(L, -1);
        lua_pop(L, 1);
        fp.size_x = static_cast<u8>(number(t, "SizeX"));
        fp.size_z = static_cast<u8>(number(t, "SizeZ"));
        fp.caps = static_cast<u8>(number(t, "Caps"));
        fp.flags = static_cast<u8>(number(t, "Flags"));
        fp.max_slope = static_cast<f32>(number(t, "MaxSlope"));
        fp.min_water_depth = static_cast<f32>(number(t, "MinWaterDepth"));
        fp.max_water_depth = static_cast<f32>(number(t, "MaxWaterDepth"));
        store->add_footprint_class(std::move(fp));
        lua_pop(L, 1);
    }
    spdlog::debug("SpecFootprints: {} classes", store->footprint_classes().size());
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
