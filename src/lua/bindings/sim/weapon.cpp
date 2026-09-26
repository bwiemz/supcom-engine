// Weapons: moho.weapon_methods.
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

static sim::Unit* check_weapon_unit(lua_State* L, int idx = 1) {
    if (!lua_istable(L, idx)) return nullptr;
    lua_pushstring(L, "_c_unit");
    lua_rawget(L, idx);
    auto* u = lua_isuserdata(L, -1)
                  ? static_cast<sim::Unit*>(lua_touserdata(L, -1))
                  : nullptr;
    lua_pop(L, 1);
    return u;
}

/// weapon:PlaySound(sound) -- a one-shot at the weapon's unit
static int weapon_PlaySound(lua_State* L) {
    auto* mgr = get_sound_mgr(L);
    if (!mgr) return 0;

    auto* unit = check_weapon_unit(L);
    if (!unit || unit->destroyed()) return 0;

    std::string bank, cue, lod;
    if (!extract_sound_table(L, 2, bank, cue, &lod)) return 0;

    auto pos = unit->position();
    mgr->play(bank, cue, &pos, lod);
    return 0;
}

// ====================================================================
// weapon_methods — real implementations
// ====================================================================

static int weapon_GetBlueprint(lua_State* L) {
    auto* w = check_weapon(L);
    if (!w || w->blueprint_ref < 0) { lua_pushnil(L); return 1; }
    lua_rawgeti(L, LUA_REGISTRYINDEX, w->blueprint_ref);
    return 1;
}

static int weapon_GetCurrentTarget(lua_State* L) {
    auto* w = check_weapon(L);
    if (!w || w->target_entity_id == 0) { lua_pushnil(L); return 1; }
    auto* sim = get_sim(L);
    if (!sim) { lua_pushnil(L); return 1; }
    auto* target = sim->entity_registry().find(w->target_entity_id);
    if (!target || target->destroyed() || target->lua_table_ref() < 0) {
        lua_pushnil(L);
        return 1;
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, target->lua_table_ref());
    return 1;
}

static int weapon_GetCurrentTargetPos(lua_State* L) {
    auto* w = check_weapon(L);
    auto* sim = get_sim(L);
    const auto at = w && sim ? w->target_point(sim->entity_registry()) : std::nullopt;
    if (!at) { lua_pushnil(L); return 1; }
    push_vector3(L, *at);
    return 1;
}

static int weapon_WeaponHasTarget(lua_State* L) {
    auto* w = check_weapon(L);
    lua_pushboolean(L, w && w->target_entity_id > 0);
    return 1;
}

// Moho's firing condition (a target, enabled, the unit not Busy, ...), not
// the fire clock: retail's reload state asks it while its unit is Busy.
static int weapon_CanFire(lua_State* L) {
    auto* w = check_weapon(L);
    auto* sim = get_sim(L);
    bool can = false;
    if (w && sim) {
        auto* owner = sim->entity_registry().find(w->owner_entity_id);
        can = owner && !owner->destroyed() && owner->is_unit() &&
              w->can_fire(static_cast<const sim::Unit&>(*owner), sim->entity_registry());
    }
    lua_pushboolean(L, can);
    return 1;
}

static int weapon_SetEnabled(lua_State* L) {
    auto* w = check_weapon(L);
    if (w) w->enabled = lua_toboolean(L, 2) != 0;
    return 0;
}

static int weapon_GetMaxRadius(lua_State* L) {
    auto* w = check_weapon(L);
    lua_pushnumber(L, w ? w->max_range : 0);
    return 1;
}

static int weapon_GetMinRadius(lua_State* L) {
    auto* w = check_weapon(L);
    lua_pushnumber(L, w ? w->min_range : 0);
    return 1;
}

static int weapon_SetTargetEntity(lua_State* L) {
    auto* w = check_weapon(L);
    if (!w) return 0;
    auto* target = check_entity(L, 2);
    w->set_target_entity((target && !target->destroyed()) ? target->entity_id() : 0);
    return 0;
}

// weapon:ResetTarget(): drop the target and look again on the next tick,
// whatever TargetCheckInterval says ("force the weapon to recheck").
static int weapon_ResetTarget(lua_State* L) {
    auto* w = check_weapon(L);
    if (w) {
        w->set_target_entity(0);
        w->target_check_clock = 0;
    }
    return 0;
}

static int weapon_GetFireClockPct(lua_State* L) {
    auto* w = check_weapon(L);
    if (!w) { lua_pushnumber(L, 1); return 1; }
    const f64 pct = 1.0 - static_cast<f64>(w->fire_clock) / w->fire_period();
    lua_pushnumber(L, std::clamp(pct, 0.0, 1.0));
    return 1;
}

static int weapon_ChangeMaxRadius(lua_State* L) {
    auto* w = check_weapon(L);
    if (w) w->max_range = static_cast<f32>(luaL_checknumber(L, 2));
    return 0;
}

static int weapon_ChangeMinRadius(lua_State* L) {
    auto* w = check_weapon(L);
    if (w) w->min_range = static_cast<f32>(luaL_checknumber(L, 2));
    return 0;
}

static int weapon_ChangeDamage(lua_State* L) {
    auto* w = check_weapon(L);
    if (w) w->damage = static_cast<f32>(luaL_checknumber(L, 2));
    return 0;
}

static int weapon_ChangeRateOfFire(lua_State* L) {
    auto* w = check_weapon(L);
    if (w) w->rate_of_fire = static_cast<f32>(luaL_checknumber(L, 2));
    return 0;
}

// CreateProjectile — weapon creates a projectile entity and returns its Lua table.
// FA calls this from the weapon state machine; for M10 we already create projectiles
// in C++ auto-fire, but this allows Lua to manually create them too.
static int weapon_CreateProjectile(lua_State* L) {
    auto* w = check_weapon(L);
    auto* unit = check_weapon_unit(L);
    if (!w || !unit || unit->destroyed()) { lua_pushnil(L); return 1; }

    auto* sim = get_sim(L);
    if (!sim) { lua_pushnil(L); return 1; }

    // From the muzzle bone the script names (retail's CreateProjectileAtMuzzle
    // passes it), else the weapon's own first muzzle, else the unit.
    i32 bone = -1;
    if (!lua_isnoneornil(L, 2)) bone = resolve_bone_index(unit, L, 2);
    else if (!w->muzzle_bone_name.empty() && unit->bone_data())
        bone = unit->bone_data()->find_bone(w->muzzle_bone_name);
    const sim::Vector3 spawn = bone >= 0 ? bone_world_position(unit, bone) : unit->position();
    // A silo missile leaves the way its muzzle faces.
    std::optional<sim::Vector3> muzzle_dir;
    if (bone >= 0 && unit->bone_data()) muzzle_dir = unit->bone_world_forward(bone);

    const sim::Entity* target = nullptr;
    if (w->target_entity_id > 0) {
        target = sim->entity_registry().find(w->target_entity_id);
        if (target && target->destroyed()) target = nullptr;
    }
    auto* proj = w->launch(*unit, spawn, target, sim->entity_registry(), L,
                           under_water(sim, spawn), muzzle_dir);
    if (!proj || proj->lua_table_ref() < 0) {
        lua_pushnil(L);
        return 1;
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, proj->lua_table_ref());
    return 1;
}

static int weapon_SetFiringRandomness(lua_State* L) {
    auto* w = check_weapon(L);
    if (w) w->firing_randomness = static_cast<f32>(lua_tonumber(L, 2));
    return 0;
}

static int weapon_GetFiringRandomness(lua_State* L) {
    auto* w = check_weapon(L);
    lua_pushnumber(L, w ? w->firing_randomness : 0);
    return 1;
}

static int weapon_SetFireTargetLayerCaps(lua_State* L) {
    auto* w = check_weapon(L);
    if (!w) return 0;
    if (lua_type(L, 2) != LUA_TSTRING) {
        w->fire_target_layer_caps = 0xFF; // no string → reset to all layers
        return 0;
    }
    w->fire_target_layer_caps = sim::parse_layer_caps(lua_tostring(L, 2));
    return 0;
}

static int weapon_ChangeDamageRadius(lua_State* L) {
    auto* w = check_weapon(L);
    if (w) w->damage_radius = static_cast<f32>(luaL_checknumber(L, 2));
    return 0;
}

static int weapon_ChangeDamageType(lua_State* L) {
    auto* w = check_weapon(L);
    if (!w) return 0;
    if (lua_type(L, 2) == LUA_TSTRING)
        w->damage_type = lua_tostring(L, 2);
    return 0;
}

static int weapon_ChangeMaxHeightDiff(lua_State* L) {
    auto* w = check_weapon(L);
    if (w) w->max_height_diff = static_cast<f32>(luaL_checknumber(L, 2));
    return 0;
}

static int weapon_ChangeFiringTolerance(lua_State* L) {
    auto* w = check_weapon(L);
    if (w) w->firing_tolerance = static_cast<f32>(luaL_checknumber(L, 2));
    return 0;
}

static int weapon_ChangeProjectileBlueprint(lua_State* L) {
    auto* w = check_weapon(L);
    if (!w) return 0;
    if (lua_type(L, 2) == LUA_TSTRING)
        w->projectile_bp_id = lua_tostring(L, 2);
    return 0;
}

static int weapon_SetOnTransport(lua_State* L) {
    auto* w = check_weapon(L);
    if (w) w->enabled = !(lua_toboolean(L, 2) != 0);
    return 0;
}

// --- Weapon targeting + control ---

static int weapon_GetProjectileBlueprint(lua_State* L) {
    auto* w = check_weapon(L);
    if (w && !w->projectile_bp_id.empty()) {
        lua_pushstring(L, w->projectile_bp_id.c_str());
    } else {
        lua_pushnil(L);
    }
    return 1;
}

/// SetTargetGround(position): aim at a point on the ground.
static int weapon_SetTargetGround(lua_State* L) {
    auto* w = check_weapon(L);
    if (!w || !lua_istable(L, 2)) return 0;
    lua_rawgeti(L, 2, 1);
    lua_rawgeti(L, 2, 2);
    lua_rawgeti(L, 2, 3);
    w->set_target_ground({static_cast<f32>(lua_tonumber(L, -3)),
                          static_cast<f32>(lua_tonumber(L, -2)),
                          static_cast<f32>(lua_tonumber(L, -1))});
    lua_pop(L, 3);
    return 0;
}

// weapon:SetFireControl(label): the aim controller of that label gates
// firing (OnTarget) instead of the first one created.
static int weapon_SetFireControl(lua_State* L) {
    auto* w = check_weapon(L);
    if (w) w->fire_control_label = lua_type(L, 2) == LUA_TSTRING ? lua_tostring(L, 2) : "";
    return 0;
}

// weapon:IsFireControl(label): whether that aim controller gates firing.
static int weapon_IsFireControl(lua_State* L) {
    auto* w = check_weapon(L);
    bool is = false;
    if (w && lua_type(L, 2) == LUA_TSTRING) {
        lua_pushstring(L, "_c_unit");
        lua_rawget(L, 1);
        const auto* unit = static_cast<const sim::Unit*>(lua_touserdata(L, -1));
        lua_pop(L, 1);
        const sim::AimManipulator* aim = unit ? w->fire_control(*unit) : nullptr;
        is = aim && aim->label() == lua_tostring(L, 2);
    }
    lua_pushboolean(L, is ? 1 : 0);
    return 1;
}

static int weapon_TransferTarget(lua_State* L) {
    auto* w = check_weapon(L);
    auto* src = check_weapon(L, 2);
    if (w && src) w->target_entity_id = src->target_entity_id;
    return 0;
}

// weapon:SetTargetingPriorities({category, ...}): compiled now, as Moho
// copies them (FAF clears the table it passes right after the call).
static int weapon_SetTargetingPriorities(lua_State* L) {
    auto* w = check_weapon(L);
    if (!w) return 0;
    w->target_priorities.clear();
    if (lua_istable(L, 2)) {
        for (int i = 1;; ++i) {
            lua_rawgeti(L, 2, i);
            if (lua_isnil(L, -1)) {
                lua_pop(L, 1);
                break;
            }
            sim::CategoryExpr priority = lua_type(L, -1) == LUA_TSTRING
                                             ? sim::parse_category_list(lua_tostring(L, -1))
                                             : sim::compile_category(L, -1);
            lua_pop(L, 1);
            if (!priority.empty()) w->target_priorities.push_back(std::move(priority));
        }
    }
    return 0;
}

static int weapon_SetWeaponPriorities(lua_State* L) {
    auto* w = check_weapon(L);
    if (!w) return 0;
    if (w->weapon_priorities_ref >= 0)
        luaL_unref(L, LUA_REGISTRYINDEX, w->weapon_priorities_ref);
    if (lua_istable(L, 2)) {
        lua_pushvalue(L, 2);
        w->weapon_priorities_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    } else {
        w->weapon_priorities_ref = -2;
    }
    return 0;
}

// ---- M51: weapon fire + control bindings ----

// weapon:FireWeapon() — trigger weapon fire cycle
static int weapon_FireWeapon(lua_State* L) {
    auto* w = check_weapon(L);
    if (!w) { lua_pushboolean(L, 0); return 1; }
    auto* sim = get_sim(L);
    if (!sim) { lua_pushboolean(L, 0); return 1; }
    auto* owner = sim->entity_registry().find(w->owner_entity_id);
    if (!owner || owner->destroyed() || !owner->is_unit()) {
        lua_pushboolean(L, 0); return 1;
    }
    auto& unit = static_cast<sim::Unit&>(*owner);
    // A stunned unit's weapons don't fire (Moho's UnitWeapon::Fire).
    if (unit.is_stunned()) {
        lua_pushboolean(L, 0);
        return 1;
    }
    // A weapon its script fires hears OnFire, as the fire clock would give
    // it (the Othuy's beam fires this way); the engine fires the rest.
    if (w->fires_through_script() && w->lua_table_ref >= 0) {
        lua_pushstring(L, "OnFire");
        lua_gettable(L, 1);
        bool fired = false;
        if (lua_isfunction(L, -1)) {
            lua_pushvalue(L, 1);
            fired = lua_pcall(L, 1, 0, 0) == 0;
            if (!fired) {
                spdlog::warn("FireWeapon OnFire error: {}", lua_tostring(L, -1));
                lua_pop(L, 1);
            }
        } else {
            lua_pop(L, 1);
        }
        lua_pushboolean(L, fired ? 1 : 0);
        return 1;
    }
    bool fired = w->try_fire(unit, sim->entity_registry(), L);
    lua_pushboolean(L, fired ? 1 : 0);
    return 1;
}

// weapon:DoInstaHit(target_bone, target_entity, damage_amount, [damage_type])
// Applies damage directly to target without projectile
static int weapon_DoInstaHit(lua_State* L) {
    auto* w = check_weapon(L);
    if (!w) return 0;
    // arg2 = target bone (string, ignored)
    // arg3 = target entity (table with _c_object)
    // arg4 = damage amount (number, optional — defaults to weapon damage)
    // FA actually calls: DoInstaHit(self, 0, target, damageAmount, damageTable)
    // Find the entity argument (first table arg after self)
    int target_idx = 0;
    for (int i = 2; i <= lua_gettop(L); ++i) {
        if (lua_istable(L, i)) {
            lua_pushstring(L, "_c_object");
            lua_rawget(L, i);
            if (lua_isuserdata(L, -1)) {
                target_idx = i;
                lua_pop(L, 1);
                break;
            }
            lua_pop(L, 1);
        }
    }
    if (target_idx == 0) return 0;

    auto* target_e = check_entity(L, target_idx);
    if (!target_e || target_e->destroyed()) return 0;

    f32 amount = w->damage;
    // Check if there's a number after the target table for damage amount
    if (target_idx + 1 <= lua_gettop(L) && lua_isnumber(L, target_idx + 1))
        amount = static_cast<f32>(lua_tonumber(L, target_idx + 1));

    if (amount <= 0) return 0;

    // Apply armor multiplier if target is a unit
    auto* sim = get_sim(L);
    if (sim && target_e->is_unit()) {
        auto* target_unit = static_cast<sim::Unit*>(target_e);
        if (!target_unit->can_take_damage()) return 0;
        amount *= sim->armor_definition().get_multiplier(
            target_unit->armor_type(), w->damage_type);
        if (amount <= 0) return 0;
    }

    // Call OnDamage on target
    lua_pushstring(L, "OnDamage");
    lua_gettable(L, target_idx);
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, target_idx); // self
        // Find instigator (owner unit)
        if (sim) {
            auto* owner = sim->entity_registry().find(w->owner_entity_id);
            if (owner && owner->lua_table_ref() >= 0) {
                lua_rawgeti(L, LUA_REGISTRYINDEX, owner->lua_table_ref());
            } else {
                lua_pushnil(L);
            }
        } else {
            lua_pushnil(L);
        }
        lua_pushnumber(L, amount);
        lua_pushnil(L); // vector (unused)
        lua_pushstring(L, w->damage_type.c_str());
        if (lua_pcall(L, 5, 0, 0) != 0) { lua_pop(L, 1); }
    } else {
        lua_pop(L, 1);
        // Fallback: direct HP reduction
        target_e->set_health(target_e->health() - amount);
    }
    return 0;
}

// weapon:SetValidTargetsForCurrentLayer(layer)
// Sets fire_target_layer_caps from the weapon blueprint's FireTargetLayerCapsTable
static int weapon_SetValidTargetsForCurrentLayer(lua_State* L) {
    auto* w = check_weapon(L);
    if (!w) return 0;
    // arg2 = layer string (e.g. "Land", "Water")
    const char* layer = (lua_type(L, 2) == LUA_TSTRING) ? lua_tostring(L, 2) : nullptr;
    if (!layer) return 0;

    // Look up bp.FireTargetLayerCapsTable[layer]
    if (w->blueprint_ref >= 0) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, w->blueprint_ref);
        lua_pushstring(L, "FireTargetLayerCapsTable");
        lua_rawget(L, -2);
        if (lua_istable(L, -1)) {
            lua_pushstring(L, layer);
            lua_rawget(L, -2);
            if (lua_type(L, -1) == LUA_TSTRING) {
                w->fire_target_layer_caps = sim::parse_layer_caps(lua_tostring(L, -1));
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 2); // FireTargetLayerCapsTable + bp table
    }
    return 0;
}

// weapon:BeenDestroyed() — returns true if weapon's owner unit is destroyed
static int weapon_BeenDestroyed(lua_State* L) {
    auto* w = check_weapon(L);
    if (!w) { lua_pushboolean(L, 1); return 1; }
    auto* sim = get_sim(L);
    if (!sim) { lua_pushboolean(L, 1); return 1; }
    auto* owner = sim->entity_registry().find(w->owner_entity_id);
    lua_pushboolean(L, (!owner || owner->destroyed()) ? 1 : 0);
    return 1;
}

// clang-format off
const MethodEntry weapon_methods[] = {
    // Real implementations
    {"GetBlueprint",                weapon_GetBlueprint},
    {"GetCurrentTarget",            weapon_GetCurrentTarget},
    {"GetCurrentTargetPos",         weapon_GetCurrentTargetPos},
    {"WeaponHasTarget",             weapon_WeaponHasTarget},
    {"CanFire",                     weapon_CanFire},
    {"SetEnabled",                  weapon_SetEnabled},
    {"GetMaxRadius",                weapon_GetMaxRadius},
    {"GetMinRadius",                weapon_GetMinRadius},
    {"SetTargetEntity",             weapon_SetTargetEntity},
    {"ResetTarget",                 weapon_ResetTarget},
    {"GetFireClockPct",             weapon_GetFireClockPct},
    {"ChangeMaxRadius",             weapon_ChangeMaxRadius},
    {"ChangeMinRadius",             weapon_ChangeMinRadius},
    {"ChangeDamage",                weapon_ChangeDamage},
    {"ChangeRateOfFire",            weapon_ChangeRateOfFire},
    {"CreateProjectile",            weapon_CreateProjectile},
    // Stubs (still needed by FA Lua but not critical for M10)
    {"GetProjectileBlueprint",      weapon_GetProjectileBlueprint},
    {"FireWeapon",                  weapon_FireWeapon},
    {"DoInstaHit",                  weapon_DoInstaHit},
    {"SetTargetGround",             weapon_SetTargetGround},
    {"SetTargetingPriorities",      weapon_SetTargetingPriorities},
    {"TransferTarget",              weapon_TransferTarget},
    {"SetFireControl",              weapon_SetFireControl},
    {"IsFireControl",               weapon_IsFireControl},
    {"SetFiringRandomness",         weapon_SetFiringRandomness},
    {"GetFiringRandomness",         weapon_GetFiringRandomness},
    {"SetFireTargetLayerCaps",      weapon_SetFireTargetLayerCaps},
    {"ChangeDamageRadius",          weapon_ChangeDamageRadius},
    {"ChangeDamageType",            weapon_ChangeDamageType},
    {"ChangeMaxHeightDiff",         weapon_ChangeMaxHeightDiff},
    {"ChangeFiringTolerance",       weapon_ChangeFiringTolerance},
    {"ChangeProjectileBlueprint",   weapon_ChangeProjectileBlueprint},
    {"BeenDestroyed",               weapon_BeenDestroyed},
    {"PlaySound",                   weapon_PlaySound},
    {"SetValidTargetsForCurrentLayer", weapon_SetValidTargetsForCurrentLayer},
    {"SetWeaponPriorities",         weapon_SetWeaponPriorities},
    {"SetOnTransport",              weapon_SetOnTransport},
    {nullptr, nullptr},
};
// clang-format on

} // namespace osc::lua
