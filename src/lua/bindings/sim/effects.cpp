// Effects: IEffect and collision beams.
// Split out of moho_bindings.cpp by class (M191 step 2); what the files
// share is declared in lua/moho_bindings_internal.hpp.

#include "lua/moho_bindings.hpp"
#include "lua/moho_bindings_internal.hpp"
#include "lua/lua_stubs.hpp"
#include "core/dmath.hpp"
#include "sim/blueprint_categories.hpp"
#include "lua/category_utils.hpp"
#include "video/video_decoder.hpp"
#include "map/scmap_parser.hpp"
#include "lua/factory_queue.hpp"
#include "lua/order_helpers.hpp"
#include "lua/lua_state.hpp"
#include "lua/sim_bindings.hpp"
#include "sim/army_brain.hpp"
#include "sim/build_placement.hpp"
#include "sim/bone_data.hpp"
#include "sim/collision.hpp"
#include "sim/entity.hpp"
#include "sim/entity_registry.hpp"
#include "sim/ieffect.hpp"
#include "sim/manipulator.hpp"
#include "core/test_status.hpp"
#include "sim/category_expr.hpp"
#include "sim/prop.hpp"
#include "sim/prop_script.hpp"
#include "sim/sim_state.hpp"
#include "sim/collision_beam.hpp"
#include "sim/projectile_script.hpp"
#include "sim/thread_manager.hpp"
#include "map/visibility_grid.hpp"
#include "sim/unit.hpp"
#include "sim/navigator.hpp"
#include "sim/platoon.hpp"
#include "sim/projectile.hpp"
#include "sim/shield.hpp"
#include "sim/unit_command.hpp"
#include "map/pathfinder.hpp"
#include "map/pathfinding_grid.hpp"
#include "sim/weapon.hpp"
#include "blueprints/blueprint_store.hpp"
#include "audio/sound_manager.hpp"
#include "ui/ui_control.hpp"
#include "ui/world_view.hpp"
#include "ui/font_metrics_provider.hpp"
#include "ui/keymap.hpp"
#include "ui/wld_ui_provider.hpp"
#include "sim/sim_callback_queue.hpp"
#include "map/terrain.hpp"
#include "vfs/virtual_file_system.hpp"
#include "core/front_end_data.hpp"
#include "core/game_state.hpp"
#include "core/localization.hpp"
#include "core/preferences.hpp"
#include "lua/beat_system.hpp"
#include "lua/mp_net_state.hpp"
#include "lua/sim_sync.hpp"
#include <algorithm>
#include <cctype>
#include <map>
#include <chrono>
#include <cmath>
#include <cstring>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>
#include <vector>
#include <spdlog/spdlog.h>
#include <lua.h>
#include <lauxlib.h>

#include <utility>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

namespace osc::lua {

// IEffect — real methods that update C++ state and return self for chaining.
// _c_object lightuserdata points to sim::IEffect*.

// Effects are named by id (see push_ieffect_table): nullptr once destroyed.
static sim::IEffect* check_ieffect(lua_State* L, int idx = 1) {
    if (!lua_istable(L, idx)) return nullptr;
    auto* sim = get_sim(L);
    lua_pushstring(L, "_c_effect_id");
    lua_rawget(L, idx);
    auto* fx = sim && lua_isnumber(L, -1)
                   ? sim->effect_registry().find(static_cast<u32>(lua_tonumber(L, -1)))
                   : nullptr;
    lua_pop(L, 1);
    return fx;
}

// ScaleEmitter(scale) → self
static int ieffect_ScaleEmitter(lua_State* L) {
    auto* fx = check_ieffect(L);
    if (fx) fx->set_scale(static_cast<f32>(luaL_optnumber(L, 2, 1.0)));
    lua_pushvalue(L, 1);
    return 1;
}

// OffsetEmitter(x, y, z) → self
static int ieffect_OffsetEmitter(lua_State* L) {
    auto* fx = check_ieffect(L);
    if (fx) fx->set_offset(
        static_cast<f32>(luaL_optnumber(L, 2, 0)),
        static_cast<f32>(luaL_optnumber(L, 3, 0)),
        static_cast<f32>(luaL_optnumber(L, 4, 0)));
    lua_pushvalue(L, 1);
    return 1;
}

// SetEmitterParam(paramName, value) → self
static int ieffect_SetEmitterParam(lua_State* L) {
    auto* fx = check_ieffect(L);
    if (fx) {
        const char* name = luaL_optstring(L, 2, "");
        f64 value = luaL_optnumber(L, 3, 0);
        fx->set_param(name, value);
        // An emitter's LIFETIME (ticks from when it was made) sets when it
        // ends, as its blueprint's Lifetime did; negative, it emits on.
        if (std::string_view(name) == "LIFETIME" && fx->has_emitter_blueprint())
            fx->end_after(value, sim::SimState::SECONDS_PER_TICK);
    }
    lua_pushvalue(L, 1);
    return 1;
}

// SetEmitterCurveParam(curveName, keyIndex, value) → self
static int ieffect_SetEmitterCurveParam(lua_State* L) {
    auto* fx = check_ieffect(L);
    if (fx) {
        const char* curve = luaL_optstring(L, 2, "");
        i32 key = static_cast<i32>(luaL_optnumber(L, 3, 0));
        f64 value = luaL_optnumber(L, 4, 0);
        // Store as "CURVE:key" for future rendering
        std::string param_key = std::string(curve) + ":" + std::to_string(key);
        fx->set_param(param_key, value);
    }
    lua_pushvalue(L, 1);
    return 1;
}

// Destroy — mark C++ object destroyed + set _destroyed on Lua table
static int ieffect_Destroy(lua_State* L) {
    auto* fx = check_ieffect(L);
    if (fx) fx->mark_destroyed();
    // Also set _destroyed on Lua table for BeenDestroyed check
    if (lua_istable(L, 1)) {
        lua_pushstring(L, "_destroyed");
        lua_pushboolean(L, 1);
        lua_rawset(L, 1);
    }
    return 0;
}

// clang-format off
const MethodEntry ieffect_methods[] = {
    {"ScaleEmitter",            ieffect_ScaleEmitter},
    {"OffsetEmitter",           ieffect_OffsetEmitter},
    {"SetEmitterParam",         ieffect_SetEmitterParam},
    {"SetEmitterCurveParam",    ieffect_SetEmitterCurveParam},
    {"Destroy",                 ieffect_Destroy},
    {"BeenDestroyed",           been_destroyed_check},
    {nullptr, nullptr},
};
// clang-format on

// ====================================================================
// CollisionBeam — real implementations (M91)
// ====================================================================

/// __init(self, spec) — C++ factory for CollisionBeamEntity.
/// Called from ClassFactory when `CollisionBeam(beamSpec)` is instantiated.
/// spec = { Weapon = weaponTable, BeamBone = 0, CollisionCheckInterval = N, OtherBone = muzzle }
/// Creates a C++ Entity marked as collision beam and sets _c_object.
static int collision_beam_init(lua_State* L) {
    auto* sim = get_sim(L);
    if (!sim) return 0;
    if (!lua_istable(L, 1)) return luaL_error(L, "CollisionBeamEntity.__init: self must be table");

    // Extract launcher (weapon's unit) from spec.Weapon.unit, and what it
    // fires from (M206c): the weapon, spec.OtherBone on the launcher (the
    // retail engine's own note: "bone of weapon's unit to attach to"), and
    // spec.CollisionCheckInterval in ticks.
    u32 launcher_id = 0;
    i32 army = 0;
    sim::Entity::BeamSetup setup;
    if (lua_istable(L, 2)) {
        lua_pushstring(L, "Weapon");
        lua_rawget(L, 2);
        if (lua_istable(L, -1)) {
            int weapon_idx = lua_gettop(L); // absolute index of Weapon table
            lua_pushstring(L, "unit");
            lua_rawget(L, weapon_idx);
            sim::Entity* unit = nullptr;
            if (lua_istable(L, -1)) {
                int unit_idx = lua_gettop(L); // absolute index of unit table
                unit = check_entity(L, unit_idx);
                if (unit) {
                    launcher_id = unit->entity_id();
                    army = unit->army();
                }
            }
            lua_pop(L, 1); // pop unit
            if (auto* weapon = check_weapon(L, weapon_idx)) {
                weapon->beam = true;
                setup.weapon = weapon->weapon_index;
                setup.length =
                    weapon->max_beam_length > 0 ? weapon->max_beam_length : weapon->max_range;
            }
            if (unit) {
                lua_pushstring(L, "OtherBone");
                lua_rawget(L, 2);
                if (!lua_isnil(L, -1) && unit->bone_data())
                    setup.muzzle_bone = resolve_bone_index(unit, L, lua_gettop(L));
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1); // pop Weapon
        lua_pushstring(L, "CollisionCheckInterval");
        lua_rawget(L, 2);
        if (lua_isnumber(L, -1) && lua_tonumber(L, -1) > 0)
            setup.check_interval = static_cast<u32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
    }

    // Create entity in registry
    auto& reg = sim->entity_registry();
    auto entity = std::make_unique<sim::Entity>();
    entity->set_collision_beam(true);
    entity->set_army(army);
    entity->set_beam_launcher_id(launcher_id);
    entity->beam_setup() = setup;
    // Copy launcher position as initial beam origin
    if (launcher_id) {
        auto* launcher = reg.find(launcher_id);
        if (launcher) entity->set_position(launcher->position());
    }

    u32 id = reg.register_entity(std::move(entity));
    auto* ent = reg.find(id);
    if (!ent) return luaL_error(L, "CollisionBeamEntity.__init: failed to register entity");

    // Store Lua table ref
    lua_pushvalue(L, 1);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    ent->set_lua_table_ref(ref);

    // Set _c_object on self
    lua_pushstring(L, "_c_object");
    lua_pushlightuserdata(L, ent);
    lua_rawset(L, 1);
    sim->track_collision_beam(id);

    spdlog::debug("CollisionBeamEntity.__init: entity #{} army={} launcher={}",
                  id, army, launcher_id);

    // Its script's OnCreate(spec) (CollisionBeam's makes its Trash): the
    // engine calls it, as no Lua __post_init does for a CollisionBeamEntity.
    lua_pushstring(L, "OnCreate");
    lua_gettable(L, 1);
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, 1);
        if (lua_istable(L, 2)) lua_pushvalue(L, 2);
        else lua_pushnil(L);
        if (lua_pcall(L, 2, 0, 0) != 0) {
            const char* err = lua_tostring(L, -1);
            const std::string message =
                std::string("CollisionBeam OnCreate error: ") + (err ? err : "(unknown)");
            spdlog::warn("{}", message);
            if (osc::test_status::count_lua_failures()) osc::test_status::record_failure(message);
            lua_pop(L, 1);
        }
    } else {
        lua_pop(L, 1);
    }
    return 0;
}

/// Enable() — activates the beam and fires OnEnable callback
static int collision_beam_Enable(lua_State* L) {
    auto* ent = check_entity(L);
    if (!ent || !ent->is_collision_beam()) return 0;
    if (ent->beam_enabled()) return 0; // already enabled
    ent->set_beam_enabled(true);
    ent->beam_setup().check_clock = 0; // its first check is this tick's

    // Fire OnEnable callback on Lua table
    if (lua_istable(L, 1)) {
        lua_pushstring(L, "OnEnable");
        lua_rawget(L, 1);
        if (lua_isfunction(L, -1)) {
            lua_pushvalue(L, 1); // self
            if (lua_pcall(L, 1, 0, 0) != 0) { lua_pop(L, 1); }
        } else {
            lua_pop(L, 1);
        }
    }
    return 0;
}

/// Disable() — deactivates the beam and fires OnDisable callback
static int collision_beam_Disable(lua_State* L) {
    auto* ent = check_entity(L);
    if (!ent || !ent->is_collision_beam()) return 0;
    if (!ent->beam_enabled()) return 0; // already disabled
    ent->set_beam_enabled(false);

    // Fire OnDisable callback on Lua table
    if (lua_istable(L, 1)) {
        lua_pushstring(L, "OnDisable");
        lua_rawget(L, 1);
        if (lua_isfunction(L, -1)) {
            lua_pushvalue(L, 1); // self
            if (lua_pcall(L, 1, 0, 0) != 0) { lua_pop(L, 1); }
        } else {
            lua_pop(L, 1);
        }
    }
    return 0;
}

/// IsEnabled() → bool
static int collision_beam_IsEnabled(lua_State* L) {
    auto* ent = check_entity(L);
    lua_pushboolean(L, (ent && ent->is_collision_beam() && ent->beam_enabled()) ? 1 : 0);
    return 1;
}

/// SetBeamFx(beamEmitter, bCollideOnStart) — stores beam emitter ref on entity
static int collision_beam_SetBeamFx(lua_State* L) {
    auto* ent = check_entity(L);
    if (!ent || !ent->is_collision_beam()) return 0;

    // arg2 = beam emitter table (IEffect), store registry ref
    if (lua_istable(L, 2)) {
        // Unref old beam fx if any
        if (ent->beam_fx_ref() >= 0) {
            luaL_unref(L, LUA_REGISTRYINDEX, ent->beam_fx_ref());
        }
        lua_pushvalue(L, 2);
        int ref = luaL_ref(L, LUA_REGISTRYINDEX);
        ent->set_beam_fx_ref(ref);
    }

    // arg3 = checkCollision: a continuous beam checks at once, and the next
    // check comes its interval + 1 ticks later.
    if (lua_toboolean(L, 3) && ent->beam_enabled()) {
        if (auto* sim = get_sim(L))
            sim::check_collision_beam(*sim, L, ent->entity_id(), /*before_pass=*/true);
    }
    return 0;
}

/// GetLauncher() → unit Lua table or nil
static int collision_beam_GetLauncher(lua_State* L) {
    auto* ent = check_entity(L);
    if (!ent || !ent->is_collision_beam() || ent->beam_launcher_id() == 0) {
        lua_pushnil(L);
        return 1;
    }
    auto* sim = get_sim(L);
    if (!sim) { lua_pushnil(L); return 1; }
    auto* launcher = sim->entity_registry().find(ent->beam_launcher_id());
    if (!launcher || launcher->destroyed() || launcher->lua_table_ref() < 0) {
        lua_pushnil(L);
        return 1;
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, launcher->lua_table_ref());
    return 1;
}

/// Destroy — its beam fx let go, then gone as any entity goes: its
/// OnDestroy (its trash, scorch threads), its ambient loop stopped, and out
/// of the registry.
static int collision_beam_Destroy(lua_State* L) {
    auto* ent = check_entity(L);
    if (ent && ent->is_collision_beam() && ent->beam_fx_ref() >= 0) {
        luaL_unref(L, LUA_REGISTRYINDEX, ent->beam_fx_ref());
        ent->set_beam_fx_ref(-2);
    }
    // Also set _destroyed on Lua table
    if (lua_istable(L, 1)) {
        lua_pushstring(L, "_destroyed");
        lua_pushboolean(L, 1);
        lua_rawset(L, 1);
    }
    return entity_Destroy(L);
}

// clang-format off
const MethodEntry collision_beam_methods[] = {
    {"__init",                  collision_beam_init},
    {"Enable",                  collision_beam_Enable},
    {"Disable",                 collision_beam_Disable},
    {"IsEnabled",               collision_beam_IsEnabled},
    {"SetBeamFx",               collision_beam_SetBeamFx},
    {"GetLauncher",             collision_beam_GetLauncher},
    {"Destroy",                 collision_beam_Destroy},
    {"BeenDestroyed",           been_destroyed_check},
    {nullptr, nullptr},
};
// clang-format on

} // namespace osc::lua
