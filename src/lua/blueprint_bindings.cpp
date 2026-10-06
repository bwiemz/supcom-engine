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

/// The table bp[`section`] (made if missing), on top of the stack.
static int push_section(lua_State* L, int bp, const char* section) {
    lua_pushstring(L, section);
    lua_rawget(L, bp);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushstring(L, section);
        lua_pushvalue(L, -2);
        lua_rawset(L, bp);
    }
    return lua_gettop(L);
}

/// Set each field of the table at `t` it leaves out to its default.
template <size_t N>
static void default_numbers(lua_State* L, int t,
                            const std::pair<const char*, lua_Number> (&fields)[N]) {
    for (const auto& [field, value] : fields) {
        lua_pushstring(L, field);
        lua_rawget(L, t);
        const bool missing = lua_isnil(L, -1);
        lua_pop(L, 1);
        if (missing) {
            lua_pushstring(L, field);
            lua_pushnumber(L, value);
            lua_rawset(L, t);
        }
    }
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

    // Its threat levels and regeneration, which Moho's RUnitBlueprint
    // defaults to 0: the AI's GetThreatOfUnits adds SurfaceThreatLevel
    // unguarded, and 174 of retail's 568 units (the UEF T1 transport among
    // them) leave all four threat levels out.
    for (const char* field : {"AirThreatLevel", "SurfaceThreatLevel", "SubThreatLevel",
                              "EconomyThreatLevel", "RegenRate"}) {
        lua_pushstring(L, field);
        lua_rawget(L, defense_idx);
        const bool missing = lua_isnil(L, -1);
        lua_pop(L, 1);
        if (missing) {
            lua_pushstring(L, field);
            lua_pushnumber(L, 0);
            lua_rawset(L, defense_idx);
        }
    }

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

    // More of RUnitBlueprint's defaults (faf-re RUnitBlueprint.cpp), which
    // FAF's unit detail view formats unguarded as a build button is hovered
    // (string.format on nil fails, and the button's click with it): vision
    // and water vision 10 (a T1 tank gives no WaterVisionRadius), the other
    // intel radii 0, a cap cost of 1, motion types none, and the drive's
    // numbers 0 -- the reverse speed the top speed, as ComputeDerivedQuantities
    // makes it (-1 for a unit that doesn't move).
    static constexpr std::pair<const char*, lua_Number> kIntelDefaults[] = {
        {"VisionRadius", 10},
        {"WaterVisionRadius", 10},
        {"RadarRadius", 0},
        {"SonarRadius", 0},
        {"OmniRadius", 0},
        {"RadarStealthFieldRadius", 0},
        {"SonarStealthFieldRadius", 0},
        {"CloakFieldRadius", 0},
    };
    default_numbers(L, push_section(L, 1, "Intel"), kIntelDefaults);
    lua_pop(L, 1);
    static constexpr std::pair<const char*, lua_Number> kGeneralDefaults[] = {{"CapCost", 1}};
    default_numbers(L, push_section(L, 1, "General"), kGeneralDefaults);
    lua_pop(L, 1);
    const int physics = push_section(L, 1, "Physics");
    static constexpr std::pair<const char*, lua_Number> kPhysicsDefaults[] = {
        {"MaxSpeed", 0},
        {"MaxAcceleration", 0},
        {"MaxBrake", 0},
        {"TurnRate", 0},
    };
    default_numbers(L, physics, kPhysicsDefaults);
    for (const char* field : {"MotionType", "AltMotionType"}) {
        lua_pushstring(L, field);
        lua_rawget(L, physics);
        const bool missing = lua_isnil(L, -1);
        lua_pop(L, 1);
        if (missing) {
            lua_pushstring(L, field);
            lua_pushstring(L, "RULEUMT_None");
            lua_rawset(L, physics);
        }
    }
    lua_pushstring(L, "MaxSpeedReverse");
    lua_rawget(L, physics);
    const bool no_reverse = lua_isnil(L, -1);
    lua_pop(L, 1);
    if (no_reverse) {
        lua_pushstring(L, "MaxSpeed");
        lua_rawget(L, physics);
        const lua_Number max_speed = lua_tonumber(L, -1);
        lua_pop(L, 1);
        lua_pushstring(L, "MaxSpeedReverse");
        lua_pushnumber(L, max_speed != 0 ? max_speed : -1);
        lua_rawset(L, physics);
    }
    lua_pop(L, 1); // Physics

    // Moho hands scripts blueprints rebuilt from its typed copies, so a
    // numeric weapon field a .bp leaves out reads as its default
    // (RUnitBlueprintWeapon's: 0, and RateOfFire 1). Retail's weapon scripts
    // rely on it: GetDamageTable adds DamageRadius (which 228 of retail's 494
    // weapons omit, the UEF commander's gun among them) and the firing
    // states compare the rack times without nil checks; FAF's projectile
    // weapons divide by RateOfFire as they are made (the UEF T1 transport's
    // guidance system has none).
    static constexpr std::pair<const char*, lua_Number> kWeaponNumberDefaults[] = {
        {"Damage", 0},
        {"DamageRadius", 0},
        {"RackRecoilDistance", 0},
        {"RackReloadTimeout", 0},
        {"RackSalvoChargeTime", 0},
        {"RackSalvoReloadTime", 0},
        {"RateOfFire", 1},
        {"WeaponRepackTimeout", 0},
    };
    lua_pushstring(L, "Weapon");
    lua_rawget(L, 1);
    if (lua_istable(L, -1)) {
        const int weapons = lua_gettop(L);
        for (int i = 1;; ++i) {
            lua_rawgeti(L, weapons, i);
            if (lua_isnil(L, -1)) {
                lua_pop(L, 1);
                break;
            }
            if (lua_istable(L, -1)) {
                for (const auto& [field, value] : kWeaponNumberDefaults) {
                    lua_pushstring(L, field);
                    lua_rawget(L, -2);
                    const bool missing = lua_isnil(L, -1);
                    lua_pop(L, 1);
                    if (!missing) continue;
                    lua_pushstring(L, field);
                    lua_pushnumber(L, value);
                    lua_rawset(L, -3);
                }
                // Its Label, an empty string when the .bp gives none (Moho's
                // string default): FAF's weapons copy it to self.Label and
                // the unit indexes its WeaponInstances by it, which a nil
                // key breaks (the UEF T1 mobile AA's first weapon has none).
                lua_pushstring(L, "Label");
                lua_rawget(L, -2);
                const bool unlabelled = lua_isnil(L, -1);
                lua_pop(L, 1);
                if (unlabelled) {
                    lua_pushstring(L, "Label");
                    lua_pushstring(L, "");
                    lua_rawset(L, -3);
                }
            }
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1); // Weapon

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
