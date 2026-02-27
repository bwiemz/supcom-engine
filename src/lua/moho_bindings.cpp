#include "lua/moho_bindings.hpp"
#include "lua/category_utils.hpp"
#include "lua/lua_state.hpp"
#include "sim/army_brain.hpp"
#include "sim/entity.hpp"
#include "sim/entity_registry.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/navigator.hpp"
#include "sim/platoon.hpp"
#include "sim/projectile.hpp"
#include "sim/unit_command.hpp"
#include "sim/weapon.hpp"
#include "blueprints/blueprint_store.hpp"

#include <cmath>
#include <cstring>
#include <spdlog/spdlog.h>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

namespace osc::lua {

// ====================================================================
// Helper: extract C++ pointers from Lua self tables
// ====================================================================

static sim::SimState* get_sim(lua_State* L) {
    lua_pushstring(L, "osc_sim_state");
    lua_rawget(L, LUA_REGISTRYINDEX);
    auto* sim = static_cast<sim::SimState*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return sim;
}

static sim::Entity* check_entity(lua_State* L, int idx = 1) {
    lua_pushstring(L, "_c_object");
    lua_rawget(L, idx);
    auto* entity = static_cast<sim::Entity*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return entity;
}

static sim::Unit* check_unit(lua_State* L, int idx = 1) {
    auto* e = check_entity(L, idx);
    if (e && e->is_unit())
        return static_cast<sim::Unit*>(e);
    return nullptr;
}

static sim::ArmyBrain* check_brain(lua_State* L, int idx = 1) {
    lua_pushstring(L, "_c_object");
    lua_rawget(L, idx);
    auto* brain = static_cast<sim::ArmyBrain*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return brain;
}

static sim::Weapon* check_weapon(lua_State* L, int idx = 1) {
    if (!lua_istable(L, idx)) return nullptr;
    lua_pushstring(L, "_c_object");
    lua_rawget(L, idx);
    auto* w = lua_isuserdata(L, -1)
                  ? static_cast<sim::Weapon*>(lua_touserdata(L, -1))
                  : nullptr;
    lua_pop(L, 1);
    return w;
}

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

static sim::Projectile* check_projectile(lua_State* L, int idx = 1) {
    auto* e = check_entity(L, idx);
    if (e && e->is_projectile())
        return static_cast<sim::Projectile*>(e);
    return nullptr;
}

static sim::Platoon* check_platoon(lua_State* L, int idx = 1) {
    if (!lua_istable(L, idx)) return nullptr;
    lua_pushstring(L, "_c_object");
    lua_rawget(L, idx);
    auto* p = lua_isuserdata(L, -1)
                  ? static_cast<sim::Platoon*>(lua_touserdata(L, -1))
                  : nullptr;
    lua_pop(L, 1);
    return (p && !p->destroyed()) ? p : nullptr;
}

// Push a Vector3 as a Lua table {[1]=x, [2]=y, [3]=z}
static void push_vector3(lua_State* L, const sim::Vector3& v) {
    lua_newtable(L);
    lua_pushnumber(L, 1);
    lua_pushnumber(L, v.x);
    lua_settable(L, -3);
    lua_pushnumber(L, 2);
    lua_pushnumber(L, v.y);
    lua_settable(L, -3);
    lua_pushnumber(L, 3);
    lua_pushnumber(L, v.z);
    lua_settable(L, -3);
}

// ====================================================================
// Stub helpers
// ====================================================================

static int stub_noop(lua_State*) { return 0; }
static int stub_return_nil(lua_State* L) {
    lua_pushnil(L);
    return 1;
}
static int stub_return_false(lua_State* L) {
    lua_pushboolean(L, 0);
    return 1;
}
static int stub_return_true(lua_State* L) {
    lua_pushboolean(L, 1);
    return 1;
}
static int stub_return_zero(lua_State* L) {
    lua_pushnumber(L, 0);
    return 1;
}
static int stub_return_one(lua_State* L) {
    lua_pushnumber(L, 1);
    return 1;
}
static int stub_return_empty_table(lua_State* L) {
    lua_newtable(L);
    return 1;
}
static int stub_return_self(lua_State* L) {
    lua_pushvalue(L, 1);
    return 1;
}
static int stub_return_empty_string(lua_State* L) {
    lua_pushstring(L, "");
    return 1;
}

// ====================================================================
// Threat helper
// ====================================================================

/// Map a threat type string to the appropriate cached threat value on a unit.
/// Returns 0 if the type doesn't apply (e.g. "Commander" for a non-COMMAND unit).
static f32 get_unit_threat_for_type(const sim::Unit* unit, const char* type) {
    if (!type || !unit) return 0;

    if (std::strcmp(type, "AntiSurface") == 0 ||
        std::strcmp(type, "Surface") == 0 ||
        std::strcmp(type, "Land") == 0) {
        return unit->surface_threat();
    }
    if (std::strcmp(type, "AntiAir") == 0 ||
        std::strcmp(type, "Air") == 0) {
        return unit->air_threat();
    }
    if (std::strcmp(type, "Sub") == 0 ||
        std::strcmp(type, "SubSurface") == 0) {
        return unit->sub_threat();
    }
    if (std::strcmp(type, "Economy") == 0) {
        return unit->economy_threat();
    }
    if (std::strcmp(type, "Commander") == 0) {
        return unit->has_category("COMMAND") ? unit->surface_threat() : 0;
    }
    if (std::strcmp(type, "Structures") == 0) {
        return unit->has_category("STRUCTURE") ? unit->surface_threat() : 0;
    }
    if (std::strcmp(type, "StructuresNotMex") == 0) {
        return (unit->has_category("STRUCTURE") &&
                !unit->has_category("MASSEXTRACTION"))
                   ? unit->surface_threat()
                   : 0;
    }
    if (std::strcmp(type, "Overall") == 0) {
        return unit->surface_threat() + unit->air_threat() +
               unit->sub_threat() + unit->economy_threat();
    }
    // Unknown type — return overall as fallback
    return unit->surface_threat() + unit->air_threat() +
           unit->sub_threat() + unit->economy_threat();
}

// ====================================================================
// entity_methods — real implementations
// ====================================================================

static int entity_GetPosition(lua_State* L) {
    auto* e = check_entity(L);
    if (!e) {
        push_vector3(L, {0, 0, 0});
        return 1;
    }
    push_vector3(L, e->position());
    return 1;
}

static int entity_GetPositionXYZ(lua_State* L) {
    auto* e = check_entity(L);
    if (!e) {
        lua_pushnumber(L, 0);
        lua_pushnumber(L, 0);
        lua_pushnumber(L, 0);
        return 3;
    }
    lua_pushnumber(L, e->position().x);
    lua_pushnumber(L, e->position().y);
    lua_pushnumber(L, e->position().z);
    return 3;
}

static int entity_SetPosition(lua_State* L) {
    auto* e = check_entity(L);
    if (!e) return 0;

    if (lua_istable(L, 2)) {
        lua_rawgeti(L, 2, 1);
        lua_rawgeti(L, 2, 2);
        lua_rawgeti(L, 2, 3);
        sim::Vector3 v;
        v.x = static_cast<f32>(lua_tonumber(L, -3));
        v.y = static_cast<f32>(lua_tonumber(L, -2));
        v.z = static_cast<f32>(lua_tonumber(L, -1));
        lua_pop(L, 3);
        e->set_position(v);
    }
    return 0;
}

static int entity_GetOrientation(lua_State* L) {
    auto* e = check_entity(L);
    sim::Quaternion q;
    if (e) q = e->orientation();
    lua_newtable(L);
    lua_pushnumber(L, 1);
    lua_pushnumber(L, q.x);
    lua_settable(L, -3);
    lua_pushnumber(L, 2);
    lua_pushnumber(L, q.y);
    lua_settable(L, -3);
    lua_pushnumber(L, 3);
    lua_pushnumber(L, q.z);
    lua_settable(L, -3);
    lua_pushnumber(L, 4);
    lua_pushnumber(L, q.w);
    lua_settable(L, -3);
    return 1;
}

static int entity_SetOrientation(lua_State* L) {
    auto* e = check_entity(L);
    if (!e) return 0;

    if (lua_istable(L, 2)) {
        sim::Quaternion q;
        lua_rawgeti(L, 2, 1);
        q.x = static_cast<f32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        lua_rawgeti(L, 2, 2);
        q.y = static_cast<f32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        lua_rawgeti(L, 2, 3);
        q.z = static_cast<f32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        lua_rawgeti(L, 2, 4);
        q.w = static_cast<f32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        e->set_orientation(q);
    }
    return 0;
}

static int entity_GetHealth(lua_State* L) {
    auto* e = check_entity(L);
    lua_pushnumber(L, e ? e->health() : 0);
    return 1;
}

static int entity_SetHealth(lua_State* L) {
    auto* e = check_entity(L);
    if (!e) return 0;
    // SetHealth(self, instigator, health)
    f32 h = static_cast<f32>(lua_tonumber(L, 3));
    e->set_health(h);
    return 0;
}

static int entity_GetMaxHealth(lua_State* L) {
    auto* e = check_entity(L);
    lua_pushnumber(L, e ? e->max_health() : 0);
    return 1;
}

static int entity_SetMaxHealth(lua_State* L) {
    auto* e = check_entity(L);
    if (!e) return 0;
    f32 h = static_cast<f32>(lua_tonumber(L, 2));
    e->set_max_health(h);
    return 0;
}

static int entity_AdjustHealth(lua_State* L) {
    auto* e = check_entity(L);
    if (!e) return 0;
    // AdjustHealth(self, instigator, delta)
    f32 delta = static_cast<f32>(lua_tonumber(L, 3));
    e->set_health(e->health() + delta);
    return 0;
}

static int entity_GetEntityId(lua_State* L) {
    auto* e = check_entity(L);
    lua_pushnumber(L, e ? e->entity_id() : 0);
    return 1;
}

static int entity_GetArmy(lua_State* L) {
    auto* e = check_entity(L);
    // FA expects 1-based army index (used as key into ArmyBrains[] and ListArmies())
    lua_pushnumber(L, (e && e->army() >= 0) ? e->army() + 1 : -1);
    return 1;
}

static int entity_GetBlueprint(lua_State* L) {
    auto* e = check_entity(L);
    if (!e || e->blueprint_id().empty()) {
        lua_pushnil(L);
        return 1;
    }

    auto* store = LuaState::get_blueprint_store(L);
    if (!store) {
        lua_pushnil(L);
        return 1;
    }

    auto* entry = store->find(e->blueprint_id());
    if (!entry) {
        lua_pushnil(L);
        return 1;
    }

    store->push_lua_table(*entry);
    return 1;
}

static int entity_GetFractionComplete(lua_State* L) {
    auto* e = check_entity(L);
    lua_pushnumber(L, e ? e->fraction_complete() : 1.0);
    return 1;
}

static int entity_Destroy(lua_State* L) {
    auto* e = check_entity(L);
    if (e) {
        u32 id = e->entity_id();
        int lua_ref = e->lua_table_ref();
        e->mark_destroyed();

        // If this is a unit, clean up weapon Lua refs before freeing
        if (e->is_unit()) {
            auto* unit = static_cast<sim::Unit*>(e);
            for (i32 i = 0; i < unit->weapon_count(); ++i) {
                auto* w = unit->get_weapon(i);
                if (!w) continue;
                // Null out _c_object in the weapon's Lua table
                if (w->lua_table_ref >= 0) {
                    lua_rawgeti(L, LUA_REGISTRYINDEX, w->lua_table_ref);
                    lua_pushstring(L, "_c_object");
                    lua_pushlightuserdata(L, nullptr);
                    lua_rawset(L, -3);
                    lua_pop(L, 1);
                    luaL_unref(L, LUA_REGISTRYINDEX, w->lua_table_ref);
                    w->lua_table_ref = LUA_NOREF;
                }
                // Release weapon blueprint ref
                if (w->blueprint_ref >= 0) {
                    luaL_unref(L, LUA_REGISTRYINDEX, w->blueprint_ref);
                    w->blueprint_ref = LUA_NOREF;
                }
            }
        }

        // Null out _c_object in the Lua table to prevent use-after-free
        lua_pushstring(L, "_c_object");
        lua_pushlightuserdata(L, nullptr);
        lua_rawset(L, 1);

        // Release Lua registry ref before freeing the C++ object
        if (lua_ref >= 0) {
            luaL_unref(L, LUA_REGISTRYINDEX, lua_ref);
        }

        auto* sim = get_sim(L);
        if (sim) sim->entity_registry().unregister_entity(id);
    }
    return 0;
}

static int entity_BeenDestroyed(lua_State* L) {
    auto* e = check_entity(L);
    lua_pushboolean(L, e ? e->destroyed() : 1);
    return 1;
}

static int entity_GetAIBrain(lua_State* L) {
    auto* e = check_entity(L);
    if (!e) { lua_pushnil(L); return 1; }

    // Look up the army brain's Lua table via its registry ref
    auto* sim = get_sim(L);
    if (sim) {
        auto* brain = sim->get_army(e->army());
        if (brain && brain->lua_table_ref() >= 0) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, brain->lua_table_ref());
            return 1;
        }
    }

    // Fallback: return a stub table if brain not yet created
    lua_newtable(L);
    lua_pushstring(L, "Army");
    lua_pushnumber(L, e->army() >= 0 ? e->army() + 1 : -1); // 1-based for FA Lua
    lua_rawset(L, -3);
    return 1;
}

static int entity_GetBoneCount(lua_State* L) {
    lua_pushnumber(L, 1);
    return 1;
}

static int entity_GetBoneName(lua_State* L) {
    lua_pushstring(L, "root");
    return 1;
}

// ====================================================================
// unit_methods — real implementations
// ====================================================================

static int unit_GetUnitId(lua_State* L) {
    auto* u = check_unit(L);
    lua_pushstring(L, u ? u->unit_id().c_str() : "");
    return 1;
}

static int unit_GetBuildRate(lua_State* L) {
    auto* u = check_unit(L);
    lua_pushnumber(L, u ? u->build_rate() : 1);
    return 1;
}

static int unit_GetWeaponCount(lua_State* L) {
    auto* u = check_unit(L);
    lua_pushnumber(L, u ? u->weapon_count() : 0);
    return 1;
}

static int unit_GetCurrentLayer(lua_State* L) {
    auto* u = check_unit(L);
    lua_pushstring(L, u ? u->layer().c_str() : "Land");
    return 1;
}

static int unit_IsBeingBuilt(lua_State* L) {
    auto* u = check_unit(L);
    lua_pushboolean(L, u && u->is_being_built());
    return 1;
}

// ====================================================================
// Build-related method implementations
// ====================================================================

static int entity_SetFractionComplete(lua_State* L) {
    auto* e = check_entity(L);
    if (e) e->set_fraction_complete(static_cast<f32>(lua_tonumber(L, 2)));
    return 0;
}

static int unit_IsIdleState(lua_State* L) {
    auto* u = check_unit(L);
    bool idle = false;
    if (u) {
        idle = u->command_queue().empty() && !u->is_building() &&
               !u->is_being_built() && !u->is_repairing() &&
               !u->is_capturing();
    }
    lua_pushboolean(L, idle ? 1 : 0);
    return 1;
}

static int unit_IsUnitState(lua_State* L) {
    auto* u = check_unit(L);
    const char* state = luaL_checkstring(L, 2);
    bool result = false;
    if (u) {
        if (std::strcmp(state, "Building") == 0)
            result = u->is_building();
        else if (std::strcmp(state, "Moving") == 0)
            result = u->is_moving();
        else if (std::strcmp(state, "BeingBuilt") == 0)
            result = u->is_being_built();
        else if (std::strcmp(state, "Guarding") == 0)
            result = !u->command_queue().empty() &&
                     u->command_queue().front().type == sim::CommandType::Guard;
        else if (std::strcmp(state, "Reclaiming") == 0)
            result = u->is_reclaiming();
        else if (std::strcmp(state, "Repairing") == 0)
            result = u->is_repairing();
        else if (std::strcmp(state, "Busy") == 0)
            result = u->busy();
        else if (std::strcmp(state, "BlockCommandQueue") == 0)
            result = u->block_command_queue();
        else if (std::strcmp(state, "Upgrading") == 0)
            result = !u->command_queue().empty() &&
                     u->command_queue().front().type == sim::CommandType::Upgrade;
        else if (std::strcmp(state, "Patrolling") == 0)
            result = !u->command_queue().empty() &&
                     u->command_queue().front().type == sim::CommandType::Patrol;
        else if (std::strcmp(state, "Attacking") == 0)
            result = !u->command_queue().empty() &&
                     u->command_queue().front().type == sim::CommandType::Attack;
        else if (std::strcmp(state, "Capturing") == 0)
            result = u->is_capturing();
        else if (std::strcmp(state, "BeingCaptured") == 0)
            result = false; // TODO: track being_captured state if needed
        else if (std::strcmp(state, "Diving") == 0)
            result = u->layer() == "Sub" || u->layer() == "Seabed";
        else if (std::strcmp(state, "Enhancing") == 0)
            result = u->is_enhancing();
        else
            result = u->has_unit_state(state);
    }
    lua_pushboolean(L, result ? 1 : 0);
    return 1;
}

static int unit_GetUnitBeingBuilt(lua_State* L) {
    auto* u = check_unit(L);
    if (!u || u->build_target_id() == 0) {
        lua_pushnil(L);
        return 1;
    }
    auto* sim = get_sim(L);
    if (!sim) { lua_pushnil(L); return 1; }
    auto* target = sim->entity_registry().find(u->build_target_id());
    if (!target || target->lua_table_ref() < 0) {
        lua_pushnil(L);
        return 1;
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, target->lua_table_ref());
    return 1;
}

static int unit_SetBusy(lua_State* L) {
    auto* u = check_unit(L);
    if (u) u->set_busy(lua_toboolean(L, 2) != 0);
    return 0;
}

static int unit_SetBlockCommandQueue(lua_State* L) {
    auto* u = check_unit(L);
    if (u) u->set_block_command_queue(lua_toboolean(L, 2) != 0);
    return 0;
}

static int unit_SetFireState(lua_State* L) {
    auto* u = check_unit(L);
    if (u) u->set_fire_state(static_cast<i32>(lua_tonumber(L, 2)));
    return 0;
}

static int unit_GetFireState(lua_State* L) {
    auto* u = check_unit(L);
    lua_pushnumber(L, u ? u->fire_state() : 0);
    return 1;
}

static int unit_GetNumBuildOrders(lua_State* L) {
    auto* u = check_unit(L);
    int count = 0;
    if (u) {
        // Active build command stays in queue while building, so just
        // count Build commands in the queue (no separate is_building check).
        for (const auto& cmd : u->command_queue()) {
            if (cmd.type == sim::CommandType::BuildMobile ||
                cmd.type == sim::CommandType::BuildFactory)
                count++;
        }
    }
    lua_pushnumber(L, count);
    return 1;
}

// GetGuards(): return table of units whose front command is Guard targeting us
static int unit_GetGuards(lua_State* L) {
    auto* u = check_unit(L);
    auto* sim = get_sim(L);
    lua_newtable(L);
    if (!u || !sim) return 1;

    int result = lua_gettop(L);
    u32 my_id = u->entity_id();
    int idx = 1;

    sim->entity_registry().for_each([&](sim::Entity& e) {
        if (e.destroyed() || !e.is_unit()) return;
        auto* other = static_cast<sim::Unit*>(&e);
        if (other->command_queue().empty()) return;
        const auto& front = other->command_queue().front();
        if (front.type == sim::CommandType::Guard && front.target_id == my_id) {
            if (other->lua_table_ref() >= 0) {
                lua_pushnumber(L, idx++);
                lua_rawgeti(L, LUA_REGISTRYINDEX, other->lua_table_ref());
                lua_rawset(L, result);
            }
        }
    });
    return 1;
}

// GetGuardedUnit(): return the unit this unit is guarding, or nil
static int unit_GetGuardedUnit(lua_State* L) {
    auto* u = check_unit(L);
    if (!u || u->command_queue().empty()) {
        lua_pushnil(L);
        return 1;
    }
    const auto& front = u->command_queue().front();
    if (front.type != sim::CommandType::Guard || front.target_id == 0) {
        lua_pushnil(L);
        return 1;
    }
    auto* sim = get_sim(L);
    if (!sim) { lua_pushnil(L); return 1; }
    auto* target = sim->entity_registry().find(front.target_id);
    if (!target || target->destroyed() || target->lua_table_ref() < 0) {
        lua_pushnil(L);
        return 1;
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, target->lua_table_ref());
    return 1;
}

// GetFocusUnit(): return the unit this unit is actively building/assisting/capturing
static int unit_GetFocusUnit(lua_State* L) {
    auto* u = check_unit(L);
    if (!u) { lua_pushnil(L); return 1; }

    u32 focus_id = u->build_target_id();
    if (focus_id == 0) focus_id = u->capture_target_id();
    if (focus_id == 0) { lua_pushnil(L); return 1; }

    auto* sim = get_sim(L);
    if (!sim) { lua_pushnil(L); return 1; }
    auto* target = sim->entity_registry().find(focus_id);
    if (!target || target->destroyed() || target->lua_table_ref() < 0) {
        lua_pushnil(L);
        return 1;
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, target->lua_table_ref());
    return 1;
}


static int unit_GetWorkProgress(lua_State* L) {
    auto* u = check_unit(L);
    lua_pushnumber(L, u ? u->work_progress() : 0);
    return 1;
}

static int unit_SetWorkProgress(lua_State* L) {
    auto* u = check_unit(L);
    if (u) u->set_work_progress(static_cast<f32>(lua_tonumber(L, 2)));
    return 0;
}

static int unit_IsCapturable(lua_State* L) {
    auto* u = check_unit(L);
    lua_pushboolean(L, u ? (u->capturable() ? 1 : 0) : 0);
    return 1;
}

static int unit_SetCapturable(lua_State* L) {
    auto* u = check_unit(L);
    if (u) u->set_capturable(lua_toboolean(L, 2) != 0);
    return 0;
}

// GetParent(): returns self (no transport system yet)
// FA's TransferUnitsOwnership checks unit:GetParent() ~= unit to skip attached
static int unit_GetParent(lua_State* L) {
    auto* u = check_unit(L);
    if (!u || u->lua_table_ref() < 0) {
        lua_pushnil(L);
        return 1;
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, u->lua_table_ref());
    return 1;
}

// ====================================================================
// Method table definitions
// ====================================================================

struct MethodEntry {
    const char* name;
    lua_CFunction func;
};

// clang-format off
static const MethodEntry entity_methods[] = {
    // Real implementations
    {"GetPosition",         entity_GetPosition},
    {"GetPositionXYZ",      entity_GetPositionXYZ},
    {"SetPosition",         entity_SetPosition},
    {"GetOrientation",      entity_GetOrientation},
    {"SetOrientation",      entity_SetOrientation},
    {"GetHealth",           entity_GetHealth},
    {"SetHealth",           entity_SetHealth},
    {"GetMaxHealth",        entity_GetMaxHealth},
    {"SetMaxHealth",        entity_SetMaxHealth},
    {"AdjustHealth",        entity_AdjustHealth},
    {"GetEntityId",         entity_GetEntityId},
    {"GetArmy",             entity_GetArmy},
    {"GetAIBrain",          entity_GetAIBrain},
    {"GetBlueprint",        entity_GetBlueprint},
    {"GetFractionComplete", entity_GetFractionComplete},
    {"Destroy",             entity_Destroy},
    {"BeenDestroyed",       entity_BeenDestroyed},
    {"GetBoneCount",        entity_GetBoneCount},
    {"GetBoneName",         entity_GetBoneName},
    // Stubs
    {"SetCollisionShape",       stub_noop},
    {"SetDrawScale",            stub_noop},
    {"SetMesh",                 stub_noop},
    {"SetScale",                stub_noop},
    {"SetParentOffset",         stub_noop},
    {"SetVizToAllies",          stub_noop},
    {"SetVizToEnemies",         stub_noop},
    {"SetVizToFocusPlayer",     stub_noop},
    {"SetVizToNeutrals",        stub_noop},
    {"ShakeCamera",             stub_noop},
    {"AttachBoneTo",            stub_noop},
    {"AttachBoneToEntityBone",  stub_noop},
    {"AttachTo",                stub_noop},
    {"DetachFrom",              stub_noop},
    {"DetachAll",               stub_noop},
    {"GetBoneDirection",        entity_GetPosition}, // returns a vector
    {"CreateProjectile",        stub_return_nil},
    {"CreateProjectileAtBone",  stub_return_nil},
    {"PlaySound",               stub_noop},
    {"SetFractionComplete",     entity_SetFractionComplete},
    {"AddManualScroller",       stub_noop},
    {"AddPingPongScroller",     stub_noop},
    {"AddThreadScroller",       stub_noop},
    {"RemoveScroller",          stub_noop},
    {"RequestRefreshUI",        stub_noop},
    {"SetCustomName",           stub_noop},
    {nullptr, nullptr},
};

// ====================================================================
// Unit: navigation / command queue implementations
// ====================================================================

// GetNavigator(self) — returns a table with _c_object = Navigator* and
// moho.navigator_methods as metatable (via __index).
static int unit_GetNavigator(lua_State* L) {
    auto* unit = check_unit(L);
    if (!unit) { lua_pushnil(L); return 1; }

    // Create navigator table: {_c_object = lightuserdata, _c_unit = lightuserdata}
    lua_newtable(L);
    lua_pushstring(L, "_c_object");
    lua_pushlightuserdata(L, &unit->navigator());
    lua_rawset(L, -3);
    lua_pushstring(L, "_c_unit");
    lua_pushlightuserdata(L, unit);
    lua_rawset(L, -3);

    // Set metatable — look up or create shared navigator metatable from registry
    lua_pushstring(L, "__osc_nav_mt");
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        // Build metatable with __index = self, copying from moho.navigator_methods
        lua_newtable(L); // mt
        int mt_idx = lua_gettop(L);
        lua_pushstring(L, "__index");
        lua_pushvalue(L, mt_idx); // mt.__index = mt
        lua_rawset(L, mt_idx);
        // Copy methods from moho.navigator_methods using absolute index
        lua_pushstring(L, "moho");
        lua_rawget(L, LUA_GLOBALSINDEX);
        if (lua_istable(L, -1)) {
            lua_pushstring(L, "navigator_methods");
            lua_rawget(L, -2);
            if (lua_istable(L, -1)) {
                int src_idx = lua_gettop(L);
                lua_pushnil(L);
                while (lua_next(L, src_idx) != 0) {
                    lua_pushvalue(L, -2); // copy key
                    lua_pushvalue(L, -2); // copy value
                    lua_rawset(L, mt_idx);
                    lua_pop(L, 1); // pop value, keep key
                }
            }
            lua_pop(L, 1); // navigator_methods
        }
        lua_pop(L, 1); // moho
        // Cache in registry
        lua_pushstring(L, "__osc_nav_mt");
        lua_pushvalue(L, mt_idx);
        lua_rawset(L, LUA_REGISTRYINDEX);
    }
    lua_setmetatable(L, -2);
    return 1;
}

static int unit_IsMoving(lua_State* L) {
    auto* unit = check_unit(L);
    lua_pushboolean(L, unit && unit->is_moving() ? 1 : 0);
    return 1;
}

static int unit_GetCommandQueue(lua_State* L) {
    auto* unit = check_unit(L);
    if (!unit) {
        lua_newtable(L);
        return 1;
    }
    const auto& queue = unit->command_queue();
    lua_newtable(L);
    int idx = 1;
    for (const auto& cmd : queue) {
        lua_newtable(L);
        lua_pushstring(L, "commandType");
        lua_pushnumber(L, static_cast<int>(cmd.type));
        lua_rawset(L, -3);
        if (cmd.type == sim::CommandType::Move ||
            cmd.type == sim::CommandType::Attack) {
            lua_pushstring(L, "x");
            lua_pushnumber(L, cmd.target_pos.x);
            lua_rawset(L, -3);
            lua_pushstring(L, "y");
            lua_pushnumber(L, cmd.target_pos.y);
            lua_rawset(L, -3);
            lua_pushstring(L, "z");
            lua_pushnumber(L, cmd.target_pos.z);
            lua_rawset(L, -3);
        }
        if (cmd.target_id > 0) {
            lua_pushstring(L, "targetId");
            lua_pushnumber(L, cmd.target_id);
            lua_rawset(L, -3);
        }
        lua_rawseti(L, -2, idx++);
    }
    return 1;
}

// unit:GetBlip(armyIndex) → blip table or nil
// No fog of war — returns a thin wrapper around the entity itself.
static int unit_GetBlip(lua_State* L) {
    auto* e = check_entity(L);
    if (!e || e->destroyed()) {
        lua_pushnil(L);
        return 1;
    }

    // Create blip table: { _c_object = lightuserdata(entity) }
    lua_newtable(L);
    int blip_tbl = lua_gettop(L);

    lua_pushstring(L, "_c_object");
    lua_pushlightuserdata(L, e);
    lua_rawset(L, blip_tbl);

    // Set cached __osc_blip_mt metatable
    lua_pushstring(L, "__osc_blip_mt");
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        // Build it: { __index = methods_table }
        lua_newtable(L); // metatable
        lua_pushstring(L, "__index");
        // Get moho.blip_methods
        lua_pushstring(L, "moho");
        lua_rawget(L, LUA_GLOBALSINDEX);
        if (lua_istable(L, -1)) {
            lua_pushstring(L, "blip_methods");
            lua_rawget(L, -2);
            lua_remove(L, -2); // remove moho table
        } else {
            lua_pop(L, 1);     // pop the non-table
            lua_pushnil(L);    // explicit nil — no methods
        }
        lua_settable(L, -3); // metatable.__index = blip_methods
        // Cache it
        lua_pushstring(L, "__osc_blip_mt");
        lua_pushvalue(L, -2);
        lua_rawset(L, LUA_REGISTRYINDEX);
    }
    lua_setmetatable(L, blip_tbl);

    return 1;
}

// ====================================================================
// Navigator methods
// ====================================================================

static sim::Navigator* check_navigator(lua_State* L, int idx = 1) {
    if (!lua_istable(L, idx)) return nullptr;
    lua_pushstring(L, "_c_object");
    lua_rawget(L, idx);
    auto* nav = lua_isuserdata(L, -1)
                    ? static_cast<sim::Navigator*>(lua_touserdata(L, -1))
                    : nullptr;
    lua_pop(L, 1);
    return nav;
}

static sim::Unit* check_nav_unit(lua_State* L, int idx = 1) {
    if (!lua_istable(L, idx)) return nullptr;
    lua_pushstring(L, "_c_unit");
    lua_rawget(L, idx);
    auto* unit = lua_isuserdata(L, -1)
                     ? static_cast<sim::Unit*>(lua_touserdata(L, -1))
                     : nullptr;
    lua_pop(L, 1);
    return unit;
}

// navigator:SetGoal(position) — position is {x, y, z} table
static int nav_SetGoal(lua_State* L) {
    auto* unit = check_nav_unit(L);
    if (!unit || unit->destroyed()) return 0;
    auto* nav = check_navigator(L);
    if (!nav) return 0;
    if (!lua_istable(L, 2)) return 0;
    sim::Vector3 pos;
    lua_pushnumber(L, 1);
    lua_gettable(L, 2);
    pos.x = static_cast<f32>(lua_tonumber(L, -1));
    lua_pop(L, 1);
    lua_pushnumber(L, 2);
    lua_gettable(L, 2);
    pos.y = static_cast<f32>(lua_tonumber(L, -1));
    lua_pop(L, 1);
    lua_pushnumber(L, 3);
    lua_gettable(L, 2);
    pos.z = static_cast<f32>(lua_tonumber(L, -1));
    lua_pop(L, 1);
    nav->set_goal(pos);
    return 0;
}

// navigator:AbortMove()
static int nav_AbortMove(lua_State* L) {
    auto* unit = check_nav_unit(L);
    if (!unit || unit->destroyed()) return 0;
    auto* nav = check_navigator(L);
    if (nav) nav->abort_move();
    return 0;
}

// navigator:GetGoal() — returns position table or nil
static int nav_GetGoal(lua_State* L) {
    auto* unit = check_nav_unit(L);
    if (!unit || unit->destroyed()) { lua_pushnil(L); return 1; }
    auto* nav = check_navigator(L);
    if (!nav || !nav->is_moving()) {
        lua_pushnil(L);
        return 1;
    }
    push_vector3(L, nav->goal());
    return 1;
}

// navigator:GetCurrentTargetSpeed() — returns speed if moving, 0 otherwise
static int nav_GetCurrentTargetSpeed(lua_State* L) {
    auto* unit = check_nav_unit(L);
    if (!unit || unit->destroyed()) { lua_pushnumber(L, 0); return 1; }
    auto* nav = check_navigator(L);
    if (nav && nav->is_moving()) {
        lua_pushnumber(L, unit->max_speed());
    } else {
        lua_pushnumber(L, 0);
    }
    return 1;
}

// unit:GetWeapon(index) — 1-based Lua index → 0-based C++
static int unit_GetWeapon(lua_State* L) {
    auto* unit = check_unit(L);
    if (!unit) { lua_pushnil(L); return 1; }

    i32 idx = static_cast<i32>(luaL_checknumber(L, 2)) - 1; // 1→0-based
    auto* weapon = unit->get_weapon(idx);
    if (!weapon) { lua_pushnil(L); return 1; }

    // If weapon already has a Lua table, return it
    if (weapon->lua_table_ref >= 0) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, weapon->lua_table_ref);
        return 1;
    }

    // Create weapon Lua table: {_c_object = weapon*, _c_unit = unit*, Label = "..."}
    lua_newtable(L);
    lua_pushstring(L, "_c_object");
    lua_pushlightuserdata(L, weapon);
    lua_rawset(L, -3);
    lua_pushstring(L, "_c_unit");
    lua_pushlightuserdata(L, unit);
    lua_rawset(L, -3);
    // FA accesses weapon.Label (property, not method) in Unit.OnCreate line 299
    lua_pushstring(L, "Label");
    lua_pushstring(L, weapon->label.c_str());
    lua_rawset(L, -3);

    // Set metatable — look up or create shared weapon metatable from registry
    lua_pushstring(L, "__osc_weapon_mt");
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L); // mt
        int mt_idx = lua_gettop(L);
        lua_pushstring(L, "__index");
        lua_pushvalue(L, mt_idx);
        lua_rawset(L, mt_idx);
        // Copy methods from moho.weapon_methods
        lua_pushstring(L, "moho");
        lua_rawget(L, LUA_GLOBALSINDEX);
        if (lua_istable(L, -1)) {
            lua_pushstring(L, "weapon_methods");
            lua_rawget(L, -2);
            if (lua_istable(L, -1)) {
                int src_idx = lua_gettop(L);
                lua_pushnil(L);
                while (lua_next(L, src_idx) != 0) {
                    lua_pushvalue(L, -2);
                    lua_pushvalue(L, -2);
                    lua_rawset(L, mt_idx);
                    lua_pop(L, 1);
                }
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
        // Cache in registry
        lua_pushstring(L, "__osc_weapon_mt");
        lua_pushvalue(L, mt_idx);
        lua_rawset(L, LUA_REGISTRYINDEX);
    }
    lua_setmetatable(L, -2);

    // Store Lua table ref on the weapon
    lua_pushvalue(L, -1);
    weapon->lua_table_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    return 1;
}

// ====================================================================
// Unit economy bindings
// ====================================================================

static int unit_SetProductionPerSecondEnergy(lua_State* L) {
    auto* u = check_unit(L);
    if (u) u->economy().production_energy = lua_tonumber(L, 2);
    return 0;
}

static int unit_SetProductionPerSecondMass(lua_State* L) {
    auto* u = check_unit(L);
    if (u) u->economy().production_mass = lua_tonumber(L, 2);
    return 0;
}

static int unit_SetConsumptionPerSecondEnergy(lua_State* L) {
    auto* u = check_unit(L);
    if (u) u->economy().consumption_energy = lua_tonumber(L, 2);
    return 0;
}

static int unit_SetConsumptionPerSecondMass(lua_State* L) {
    auto* u = check_unit(L);
    if (u) u->economy().consumption_mass = lua_tonumber(L, 2);
    return 0;
}

static int unit_GetProductionPerSecondEnergy(lua_State* L) {
    auto* u = check_unit(L);
    lua_pushnumber(L, u ? u->economy().production_energy : 0);
    return 1;
}

static int unit_GetProductionPerSecondMass(lua_State* L) {
    auto* u = check_unit(L);
    lua_pushnumber(L, u ? u->economy().production_mass : 0);
    return 1;
}

static int unit_GetConsumptionPerSecondEnergy(lua_State* L) {
    auto* u = check_unit(L);
    lua_pushnumber(L, u ? u->economy().consumption_energy : 0);
    return 1;
}

static int unit_GetConsumptionPerSecondMass(lua_State* L) {
    auto* u = check_unit(L);
    lua_pushnumber(L, u ? u->economy().consumption_mass : 0);
    return 1;
}

static int unit_SetConsumptionActive(lua_State* L) {
    auto* u = check_unit(L);
    if (u) u->economy().consumption_active = lua_toboolean(L, 2) != 0;
    return 0;
}

static int unit_SetProductionActive(lua_State* L) {
    auto* u = check_unit(L);
    if (u) u->economy().production_active = lua_toboolean(L, 2) != 0;
    return 0;
}

static int unit_SetMaintenanceConsumptionActive(lua_State* L) {
    auto* u = check_unit(L);
    if (u) u->economy().maintenance_active = true;
    return 0;
}

static int unit_SetMaintenanceConsumptionInactive(lua_State* L) {
    auto* u = check_unit(L);
    if (u) u->economy().maintenance_active = false;
    return 0;
}

static int unit_SetEnergyMaintenanceConsumptionOverride(lua_State* L) {
    auto* u = check_unit(L);
    if (u) u->economy().energy_maintenance_override = lua_tonumber(L, 2);
    return 0;
}

static int unit_SetBuildRate(lua_State* L) {
    auto* u = check_unit(L);
    if (u) u->set_build_rate(static_cast<f32>(lua_tonumber(L, 2)));
    return 0;
}

// Rally point — factories send produced units here
static int unit_GetRallyPoint(lua_State* L) {
    auto* u = check_unit(L);
    if (!u || !u->has_rally_point()) {
        // Default: return own position
        push_vector3(L, u ? u->position() : sim::Vector3{0, 0, 0});
        return 1;
    }
    push_vector3(L, u->rally_point());
    return 1;
}

static int unit_SetRallyPoint(lua_State* L) {
    auto* u = check_unit(L);
    if (!u) return 0;
    if (lua_istable(L, 2)) {
        sim::Vector3 pos;
        lua_rawgeti(L, 2, 1);
        pos.x = static_cast<f32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        lua_rawgeti(L, 2, 2);
        pos.y = static_cast<f32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        lua_rawgeti(L, 2, 3);
        pos.z = static_cast<f32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        u->set_rally_point(pos);
    }
    return 0;
}

// ====================================================================
// Toggle / Script Bit helpers
// ====================================================================

/// Map RULEUTC_* toggle cap name to script bit index (0-8). Returns -1 if unknown.
static i32 toggle_cap_to_bit(const char* name) {
    if (std::strcmp(name, "RULEUTC_ShieldToggle") == 0) return 0;
    if (std::strcmp(name, "RULEUTC_WeaponToggle") == 0) return 1;
    if (std::strcmp(name, "RULEUTC_JammingToggle") == 0) return 2;
    if (std::strcmp(name, "RULEUTC_IntelToggle") == 0) return 3;
    if (std::strcmp(name, "RULEUTC_ProductionToggle") == 0) return 4;
    if (std::strcmp(name, "RULEUTC_StealthToggle") == 0) return 5;
    if (std::strcmp(name, "RULEUTC_GenericToggle") == 0) return 6;
    if (std::strcmp(name, "RULEUTC_SpecialToggle") == 0) return 7;
    if (std::strcmp(name, "RULEUTC_CloakToggle") == 0) return 8;
    return -1;
}

/// Fire OnScriptBitSet(bit) or OnScriptBitClear(bit) Lua callback on a unit.
static void fire_script_bit_callback(lua_State* L, sim::Unit* u, i32 bit, bool set) {
    if (!u || u->lua_table_ref() < 0) return;
    lua_rawgeti(L, LUA_REGISTRYINDEX, u->lua_table_ref());
    int tbl = lua_gettop(L);
    lua_pushstring(L, set ? "OnScriptBitSet" : "OnScriptBitClear");
    lua_gettable(L, tbl);
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, tbl); // self
        lua_pushnumber(L, static_cast<lua_Number>(bit));
        if (lua_pcall(L, 2, 0, 0) != 0) {
            spdlog::warn("{} error: {}",
                         set ? "OnScriptBitSet" : "OnScriptBitClear",
                         lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    } else {
        lua_pop(L, 1); // non-function
    }
    lua_pop(L, 1); // tbl
}

// SetScriptBit(self, capNameOrBit, value)
static int unit_SetScriptBit(lua_State* L) {
    auto* u = check_unit(L);
    if (!u) return 0;

    // Arg 2: string (RULEUTC_* name) or number (bit index)
    // Use lua_type() not lua_isstring/lua_isnumber — Lua 5.0 coerces numbers↔strings
    i32 bit = -1;
    if (lua_type(L, 2) == LUA_TNUMBER) {
        bit = static_cast<i32>(lua_tonumber(L, 2));
    } else if (lua_type(L, 2) == LUA_TSTRING) {
        bit = toggle_cap_to_bit(lua_tostring(L, 2));
    }
    if (bit < 0 || bit > 8) return 0;

    bool value = lua_toboolean(L, 3) != 0;
    bool old_value = u->get_script_bit(bit);
    u->set_script_bit(bit, value);

    // Only fire callback if state actually changed (prevents infinite recursion)
    if (value != old_value) {
        fire_script_bit_callback(L, u, bit, value);
    }
    return 0;
}

// GetScriptBit(self, capNameOrBit)
static int unit_GetScriptBit(lua_State* L) {
    auto* u = check_unit(L);
    if (!u) { lua_pushboolean(L, 0); return 1; }

    // Use lua_type() not lua_isstring/lua_isnumber — Lua 5.0 coerces numbers↔strings
    i32 bit = -1;
    if (lua_type(L, 2) == LUA_TNUMBER) {
        bit = static_cast<i32>(lua_tonumber(L, 2));
    } else if (lua_type(L, 2) == LUA_TSTRING) {
        bit = toggle_cap_to_bit(lua_tostring(L, 2));
    }

    lua_pushboolean(L, u->get_script_bit(bit) ? 1 : 0);
    return 1;
}

// ToggleScriptBit(self, bit)
static int unit_ToggleScriptBit(lua_State* L) {
    auto* u = check_unit(L);
    if (!u) return 0;

    // Use lua_type() — Lua 5.0 coerces numbers↔strings
    i32 bit = -1;
    if (lua_type(L, 2) == LUA_TNUMBER) {
        bit = static_cast<i32>(lua_tonumber(L, 2));
    } else if (lua_type(L, 2) == LUA_TSTRING) {
        bit = toggle_cap_to_bit(lua_tostring(L, 2));
    }
    if (bit < 0 || bit > 8) return 0;

    u->toggle_script_bit(bit);
    bool new_value = u->get_script_bit(bit);
    fire_script_bit_callback(L, u, bit, new_value);
    return 0;
}

// AddToggleCap(self, capName)
static int unit_AddToggleCap(lua_State* L) {
    auto* u = check_unit(L);
    if (u && lua_type(L, 2) == LUA_TSTRING) {
        u->add_toggle_cap(lua_tostring(L, 2));
    }
    return 0;
}

// RemoveToggleCap(self, capName)
static int unit_RemoveToggleCap(lua_State* L) {
    auto* u = check_unit(L);
    if (u && lua_type(L, 2) == LUA_TSTRING) {
        u->remove_toggle_cap(lua_tostring(L, 2));
    }
    return 0;
}

// TestToggleCaps(self, capName)
static int unit_TestToggleCaps(lua_State* L) {
    auto* u = check_unit(L);
    if (!u || lua_type(L, 2) != LUA_TSTRING) {
        lua_pushboolean(L, 0);
        return 1;
    }
    lua_pushboolean(L, u->has_toggle_cap(lua_tostring(L, 2)) ? 1 : 0);
    return 1;
}

// --- Enhancement methods ---

// HasEnhancement(self, enhancementName) → bool
static int unit_HasEnhancement(lua_State* L) {
    auto* u = check_unit(L);
    if (!u) { lua_pushboolean(L, 0); return 1; }
    const char* name = luaL_checkstring(L, 2);
    lua_pushboolean(L, u->has_enhancement(name) ? 1 : 0);
    return 1;
}

// CreateEnhancement(self, enhancementName)
// moho fallback — FA Lua's CreateEnhancement method handles bones + AddUnitEnhancement
static int unit_CreateEnhancement(lua_State* L) {
    auto* u = check_unit(L);
    if (!u) return 0;
    const char* name = luaL_checkstring(L, 2);

    // Read Slot from self.Blueprint.Enhancements[name]
    if (u->lua_table_ref() >= 0) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, u->lua_table_ref());
        int self_tbl = lua_gettop(L);
        lua_pushstring(L, "Blueprint");
        lua_rawget(L, self_tbl);
        if (lua_istable(L, -1)) {
            lua_pushstring(L, "Enhancements");
            lua_gettable(L, -2);
            if (lua_istable(L, -1)) {
                lua_pushstring(L, name);
                lua_gettable(L, -2);
                if (lua_istable(L, -1)) {
                    lua_pushstring(L, "Slot");
                    lua_gettable(L, -2);
                    if (lua_type(L, -1) == LUA_TSTRING) {
                        std::string slot = lua_tostring(L, -1);
                        u->add_enhancement(slot, name);
                    }
                    lua_pop(L, 1); // Slot
                }
                lua_pop(L, 1); // enh entry
            }
            lua_pop(L, 1); // Enhancements
        }
        lua_pop(L, 1); // Blueprint
        lua_pop(L, 1); // self_tbl
    }
    return 0;
}

// RemoveSpecifiedEnhancement(self, enhancementName)
static int unit_RemoveSpecifiedEnhancement(lua_State* L) {
    auto* u = check_unit(L);
    if (!u) return 0;
    const char* name = luaL_checkstring(L, 2);
    u->remove_enhancement(name);
    return 0;
}

// GetResourceConsumed(self) → {mass_fraction, energy_fraction}
// Returns 1.0 for both (economy stalling not yet implemented)
static int unit_GetResourceConsumed(lua_State* L) {
    lua_newtable(L);
    lua_pushnumber(L, 1);
    lua_pushnumber(L, 1.0);
    lua_rawset(L, -3);
    lua_pushnumber(L, 2);
    lua_pushnumber(L, 1.0);
    lua_rawset(L, -3);
    return 1;
}

// SetImmobile(self, bool)
static int unit_SetImmobile(lua_State* L) {
    auto* u = check_unit(L);
    if (u) u->set_immobile(lua_toboolean(L, 2) != 0);
    return 0;
}

// IsMobile(self) → bool
static int unit_IsMobile(lua_State* L) {
    auto* u = check_unit(L);
    bool result = false;
    if (u) result = !u->immobile() && u->max_speed() > 0;
    lua_pushboolean(L, result ? 1 : 0);
    return 1;
}

// SetUnitState(self, stateName, bool)
static int unit_SetUnitState(lua_State* L) {
    auto* u = check_unit(L);
    if (!u) return 0;
    const char* state = luaL_checkstring(L, 2);
    bool value = lua_toboolean(L, 3) != 0;
    u->set_unit_state(state, value);
    return 0;
}

// ---------------------------------------------------------------------------
// Intel system
// ---------------------------------------------------------------------------

static int unit_InitIntel(lua_State* L) {
    auto* u = check_unit(L);
    if (!u) return 0;
    // Args: self, army (ignored — unit knows its army), intel_type, radius
    const char* intel_type = luaL_checkstring(L, 3);
    f32 radius = static_cast<f32>(luaL_checknumber(L, 4));
    u->init_intel(intel_type, radius);
    return 0;
}

static int unit_IsIntelEnabled(lua_State* L) {
    auto* u = check_unit(L);
    if (!u) { lua_pushboolean(L, 0); return 1; }
    const char* intel_type = luaL_checkstring(L, 2);
    lua_pushboolean(L, u->is_intel_enabled(intel_type) ? 1 : 0);
    return 1;
}

static int unit_GetIntelRadius(lua_State* L) {
    auto* u = check_unit(L);
    if (!u) { lua_pushnumber(L, 0); return 1; }
    const char* intel_type = luaL_checkstring(L, 2);
    lua_pushnumber(L, u->get_intel_radius(intel_type));
    return 1;
}

static int unit_SetIntelRadius(lua_State* L) {
    auto* u = check_unit(L);
    if (!u) return 0;
    const char* intel_type = luaL_checkstring(L, 2);
    f32 radius = static_cast<f32>(luaL_checknumber(L, 3));
    u->set_intel_radius(intel_type, radius);
    return 0;
}

static int unit_EnableIntel(lua_State* L) {
    auto* u = check_unit(L);
    if (!u) return 0;
    const char* intel_type = luaL_checkstring(L, 2);
    u->enable_intel(intel_type);
    return 0;
}

static int unit_DisableIntel(lua_State* L) {
    auto* u = check_unit(L);
    if (!u) return 0;
    const char* intel_type = luaL_checkstring(L, 2);
    u->disable_intel(intel_type);
    return 0;
}

static const MethodEntry unit_methods[] = {
    // Real implementations
    {"GetUnitId",           unit_GetUnitId},
    {"GetBuildRate",        unit_GetBuildRate},
    {"GetWeaponCount",      unit_GetWeaponCount},
    {"GetWeapon",           unit_GetWeapon},
    {"GetCurrentLayer",     unit_GetCurrentLayer},
    {"IsBeingBuilt",        unit_IsBeingBuilt},
    // Economy — real implementations
    {"SetConsumptionPerSecondEnergy",   unit_SetConsumptionPerSecondEnergy},
    {"SetConsumptionPerSecondMass",     unit_SetConsumptionPerSecondMass},
    {"SetProductionPerSecondEnergy",    unit_SetProductionPerSecondEnergy},
    {"SetProductionPerSecondMass",      unit_SetProductionPerSecondMass},
    {"GetConsumptionPerSecondEnergy",   unit_GetConsumptionPerSecondEnergy},
    {"GetConsumptionPerSecondMass",     unit_GetConsumptionPerSecondMass},
    {"GetProductionPerSecondEnergy",    unit_GetProductionPerSecondEnergy},
    {"GetProductionPerSecondMass",      unit_GetProductionPerSecondMass},
    {"SetConsumptionActive",            unit_SetConsumptionActive},
    {"SetProductionActive",             unit_SetProductionActive},
    {"SetMaintenanceConsumptionActive", unit_SetMaintenanceConsumptionActive},
    {"SetMaintenanceConsumptionInactive", unit_SetMaintenanceConsumptionInactive},
    {"SetEnergyMaintenanceConsumptionOverride", unit_SetEnergyMaintenanceConsumptionOverride},
    {"SetBuildRate",                    unit_SetBuildRate},
    // Stubs — state
    {"IsUnitState",                 unit_IsUnitState},
    {"SetUnitState",                unit_SetUnitState},
    {"IsIdleState",                 unit_IsIdleState},
    {"GetFireState",                unit_GetFireState},
    {"SetFireState",                unit_SetFireState},
    {"ToggleFireState",             stub_noop},
    {"SetPaused",                   stub_noop},
    {"IsPaused",                    stub_return_false},
    // Stubs — bones / visual
    {"ShowBone",                    stub_noop},
    {"HideBone",                    stub_noop},
    {"SetMesh",                     stub_noop},
    {"IsValidBone",                 stub_return_true},
    {"GetBoneDirection",            entity_GetPosition},
    // Stubs — build / command
    {"GetCommandQueue",             unit_GetCommandQueue},
    {"CanBuild",                    stub_return_true},
    {"AddCommandCap",               stub_noop},
    {"RemoveCommandCap",            stub_noop},
    {"RestoreCommandCaps",          stub_noop},
    {"HasValidTeleportDest",        stub_return_false},
    {"GetWorkProgress",             unit_GetWorkProgress},
    {"SetWorkProgress",             unit_SetWorkProgress},
    // Intel — real implementations
    // NOTE: EnableUnitIntel/DisableUnitIntel (FA Lua IntelComponent methods)
    // are NOT listed here — they would cause ClassUnit ambiguity.
    // These are the moho engine methods (EnableIntel/DisableIntel etc.).
    {"InitIntel",                   unit_InitIntel},
    {"IsIntelEnabled",              unit_IsIntelEnabled},
    {"GetIntelRadius",              unit_GetIntelRadius},
    {"SetIntelRadius",              unit_SetIntelRadius},
    {"EnableIntel",                 unit_EnableIntel},
    {"DisableIntel",                unit_DisableIntel},
    // Stubs — shield
    {"EnableShield",                stub_noop},
    {"DisableShield",               stub_noop},
    {"ShieldIsOn",                  stub_return_false},
    {"GetShieldRatio",              stub_return_one},
    {"SetShieldRatio",              stub_noop},
    // Stubs — collision
    {"SetCollisionShape",           stub_noop},
    {"RevertCollisionShape",        stub_noop},
    {"RevertElevation",             stub_noop},
    {"SetElevation",                stub_noop},
    // Stubs — movement
    {"IsMobile",                    unit_IsMobile},
    {"IsMoving",                    unit_IsMoving},
    {"GetNavigator",                unit_GetNavigator},
    {"SetSpeedMult",                stub_noop},
    {"SetAccMult",                  stub_noop},
    {"SetTurnMult",                 stub_noop},
    {"SetBreakOffDistanceMult",     stub_noop},
    {"SetBreakOffTriggerMult",      stub_noop},
    {"GetCurrentMoveLocation",      entity_GetPosition},
    {"GetHeading",                  stub_return_zero},
    // Stubs — transport / cargo
    {"GetCargo",                    stub_return_empty_table},
    {"TransportHasSpaceFor",        stub_return_false},
    {"AddUnitToStorage",            stub_noop},
    {"TransportDetachAllUnits",     stub_noop},
    // Stubs — missiles
    {"GetNukeSiloAmmoCount",        stub_return_zero},
    {"GetTacticalSiloAmmoCount",    stub_return_zero},
    {"GiveNukeSiloAmmo",            stub_noop},
    {"GiveTacticalSiloAmmo",        stub_noop},
    {"RemoveNukeSiloAmmo",          stub_noop},
    {"RemoveTacticalSiloAmmo",      stub_noop},
    // Stubs — armor
    {"GetArmorMult",                stub_return_one},
    {"AlterArmor",                  stub_noop},
    // Stubs — fuel
    {"GetFuelRatio",                stub_return_one},
    {"SetFuelRatio",                stub_noop},
    {"GetFuelUseTime",              stub_return_zero},
    {"SetFuelUseTime",              stub_noop},
    // Stubs — misc
    {"IsValidTarget",               stub_return_true},
    {"SetIsValidTarget",            stub_noop},
    {"SetScriptBit",                unit_SetScriptBit},
    {"GetScriptBit",                unit_GetScriptBit},
    {"AddBuildRestriction",         stub_noop},
    {"RemoveBuildRestriction",      stub_noop},
    {"AddOnGivenCallback",          stub_noop},
    {"PlayUnitSound",               stub_noop},
    {"PlayUnitAmbientSound",        stub_noop},
    {"StopUnitAmbientSound",        stub_noop},
    {"SetDoNotTarget",              stub_noop},
    {"GetGuards",                   unit_GetGuards},
    {"UpdateStat",                  stub_noop},
    {"GetStat",                     stub_return_empty_table},
    {"SetStat",                     stub_noop},
    {"CanPathTo",                   stub_return_true},
    {"CanPathToCell",               stub_return_true},
    {"GetAttacker",                 stub_return_nil},
    {"SetReclaimable",              stub_noop},
    {"SetCapturable",               unit_SetCapturable},
    {"IsCapturable",                unit_IsCapturable},
    {"GetParent",                   unit_GetParent},
    {"SetCanTakeDamage",            stub_noop},
    {"SetCanBeKilled",              stub_noop},
    {"SetUnSelectable",             stub_noop},
    {"SetAutoOvercharge",           stub_noop},
    {"ToggleScriptBit",             unit_ToggleScriptBit},
    {"GetAutoOvercharge",           stub_return_false},
    {"SetOverchargePaused",         stub_noop},
    {"RemoveSpecifiedEnhancement",  unit_RemoveSpecifiedEnhancement},
    {"HasEnhancement",              unit_HasEnhancement},
    {"CreateEnhancement",           unit_CreateEnhancement},
    {"GetResourceConsumed",         unit_GetResourceConsumed},
    {"SetImmobile",                 unit_SetImmobile},
    {"GetNumBuildOrders",           unit_GetNumBuildOrders},
    {"SetBuildingUnit",             stub_noop},
    {"GetUnitBeingBuilt",           unit_GetUnitBeingBuilt},
    {"Stop",                        stub_noop},
    {"Kill",                        entity_Destroy},
    {"GetFocusUnit",                unit_GetFocusUnit},
    {"RestoreBuildRestrictions",    stub_noop},
    {"SetCreator",                  stub_noop},
    {"OccupyGround",               stub_return_true},
    {"ResetSpeedAndAccel",          stub_noop},
    {"AddToggleCap",                unit_AddToggleCap},
    {"RemoveToggleCap",             unit_RemoveToggleCap},
    {"TestToggleCaps",              unit_TestToggleCaps},
    {"SetBlockCommandQueue",        unit_SetBlockCommandQueue},
    {"PlayCommanderWarpInEffect",   stub_noop},
    {"SetAmbientSound",             stub_noop},
    {"GetRallyPoint",                unit_GetRallyPoint},
    {"SetRallyPoint",                unit_SetRallyPoint},
    {"SetBusy",                      unit_SetBusy},
    {"SetRotation",                  stub_noop},
    {"GetGuardedUnit",               unit_GetGuardedUnit},
    {"PlayFxRollOffEnd",             stub_noop},
    {"SetupBuildBones",              stub_noop},
    {"GetBlip",                      unit_GetBlip},
    {nullptr, nullptr},
};

// ====================================================================
// projectile_methods — real implementations
// ====================================================================

static int proj_GetLauncher(lua_State* L) {
    auto* p = check_projectile(L);
    if (!p || p->launcher_id == 0) { lua_pushnil(L); return 1; }
    auto* sim = get_sim(L);
    if (!sim) { lua_pushnil(L); return 1; }
    auto* launcher = sim->entity_registry().find(p->launcher_id);
    if (!launcher || launcher->destroyed() || launcher->lua_table_ref() < 0) {
        lua_pushnil(L);
        return 1;
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, launcher->lua_table_ref());
    return 1;
}

static int proj_GetCurrentSpeed(lua_State* L) {
    auto* p = check_projectile(L);
    if (!p) { lua_pushnumber(L, 0); return 1; }
    f32 speed = std::sqrt(p->velocity.x * p->velocity.x +
                          p->velocity.y * p->velocity.y +
                          p->velocity.z * p->velocity.z);
    lua_pushnumber(L, speed);
    return 1;
}

static int proj_GetVelocity(lua_State* L) {
    auto* p = check_projectile(L);
    if (!p) {
        lua_pushnumber(L, 0);
        lua_pushnumber(L, 0);
        lua_pushnumber(L, 0);
        return 3;
    }
    lua_pushnumber(L, p->velocity.x);
    lua_pushnumber(L, p->velocity.y);
    lua_pushnumber(L, p->velocity.z);
    return 3;
}

static int proj_SetVelocity(lua_State* L) {
    auto* p = check_projectile(L);
    if (!p) { lua_pushvalue(L, 1); return 1; }
    p->velocity.x = static_cast<f32>(luaL_checknumber(L, 2));
    p->velocity.y = static_cast<f32>(luaL_checknumber(L, 3));
    p->velocity.z = static_cast<f32>(luaL_checknumber(L, 4));
    lua_pushvalue(L, 1);
    return 1;
}

static int proj_SetLifetime(lua_State* L) {
    auto* p = check_projectile(L);
    if (p) p->lifetime = static_cast<f32>(luaL_checknumber(L, 2));
    lua_pushvalue(L, 1);
    return 1;
}

static int proj_SetNewTarget(lua_State* L) {
    auto* p = check_projectile(L);
    if (!p) return 0;
    auto* target = check_entity(L, 2);
    if (target && !target->destroyed()) {
        p->target_entity_id = target->entity_id();
        p->target_position = target->position();
    }
    return 0;
}

static int proj_SetNewTargetGround(lua_State* L) {
    auto* p = check_projectile(L);
    if (!p) return 0;
    if (lua_istable(L, 2)) {
        lua_rawgeti(L, 2, 1);
        p->target_position.x = static_cast<f32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        lua_rawgeti(L, 2, 2);
        p->target_position.y = static_cast<f32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        lua_rawgeti(L, 2, 3);
        p->target_position.z = static_cast<f32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        p->target_entity_id = 0; // ground target
    }
    return 0;
}

static const MethodEntry projectile_methods[] = {
    // Real implementations
    {"GetLauncher",                 proj_GetLauncher},
    {"GetCurrentSpeed",             proj_GetCurrentSpeed},
    {"GetVelocity",                 proj_GetVelocity},
    {"SetVelocity",                 proj_SetVelocity},
    {"SetLifetime",                 proj_SetLifetime},
    {"SetNewTarget",                proj_SetNewTarget},
    {"SetNewTargetGround",          proj_SetNewTargetGround},
    // Stubs — return self for chaining
    {"GetTrackingTarget",           stub_return_nil},
    {"GetCurrentTargetPosition",    stub_return_nil},
    {"GetCurrentTargetPositionXYZ", stub_return_zero},
    {"TrackTarget",                 stub_return_false},
    {"SetScaleVelocity",            stub_return_self},
    {"SetMaxSpeed",                 stub_noop},
    {"SetAcceleration",             stub_return_self},
    {"SetBallisticAcceleration",    stub_return_self},
    {"SetCollideEntity",            stub_return_self},
    {"SetCollideSurface",           stub_return_self},
    {"SetCollision",                stub_return_self},
    {"SetDestroyOnWater",           stub_noop},
    {"SetLocalAngularVelocity",     stub_return_self},
    {"SetTurnRate",                 stub_return_self},
    {"SetStayUpright",              stub_noop},
    {"SetVelocityAlign",            stub_noop},
    {"StayUnderwater",              stub_return_false},
    {"CreateChildProjectile",       stub_return_nil},
    {"ChangeMaxZigZag",             stub_noop},
    {"ChangeZigZagFrequency",       stub_noop},
    {"GetMaxZigZag",                stub_return_zero},
    {"GetZigZagFrequency",          stub_return_zero},
    {"ChangeDetonateAboveHeight",   stub_noop},
    {"ChangeDetonateBelowHeight",   stub_noop},
    {"SetTurnRateByDist",           stub_noop},
    {nullptr, nullptr},
};

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
    if (!w || w->target_entity_id == 0) { lua_pushnil(L); return 1; }
    auto* sim = get_sim(L);
    if (!sim) { lua_pushnil(L); return 1; }
    auto* target = sim->entity_registry().find(w->target_entity_id);
    if (!target || target->destroyed()) { lua_pushnil(L); return 1; }
    push_vector3(L, target->position());
    return 1;
}

static int weapon_WeaponHasTarget(lua_State* L) {
    auto* w = check_weapon(L);
    lua_pushboolean(L, w && w->target_entity_id > 0);
    return 1;
}

static int weapon_CanFire(lua_State* L) {
    auto* w = check_weapon(L);
    lua_pushboolean(L, w && w->fire_cooldown <= 0 && w->enabled);
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
    w->target_entity_id = (target && !target->destroyed())
                              ? target->entity_id()
                              : 0;
    return 0;
}

static int weapon_ResetTarget(lua_State* L) {
    auto* w = check_weapon(L);
    if (w) w->target_entity_id = 0;
    return 0;
}

static int weapon_GetFireClockPct(lua_State* L) {
    auto* w = check_weapon(L);
    if (!w || w->rate_of_fire <= 0) { lua_pushnumber(L, 1); return 1; }
    f32 period = 1.0f / w->rate_of_fire;
    f32 pct = 1.0f - (w->fire_cooldown / period);
    if (pct < 0) pct = 0;
    if (pct > 1) pct = 1;
    lua_pushnumber(L, pct);
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

    auto proj = std::make_unique<sim::Projectile>();
    proj->set_position(unit->position());
    proj->set_army(unit->army());
    proj->launcher_id = unit->entity_id();
    proj->damage_amount = w->damage;
    proj->damage_radius = w->damage_radius;
    proj->damage_type = w->damage_type;
    proj->lifetime = 10.0f;

    // If target exists, aim toward it
    if (w->target_entity_id > 0) {
        auto* target = sim->entity_registry().find(w->target_entity_id);
        if (target && !target->destroyed()) {
            proj->target_entity_id = w->target_entity_id;
            proj->target_position = target->position();
            f32 dx = target->position().x - unit->position().x;
            f32 dz = target->position().z - unit->position().z;
            f32 dist = std::sqrt(dx * dx + dz * dz);
            if (dist > 0.001f) {
                f32 inv = 1.0f / dist;
                proj->velocity.x = dx * inv * w->muzzle_velocity;
                proj->velocity.z = dz * inv * w->muzzle_velocity;
            }
            proj->lifetime = (dist / w->muzzle_velocity) + 2.0f;
        }
    }

    u32 proj_id = sim->entity_registry().register_entity(std::move(proj));
    auto* proj_ptr = static_cast<sim::Projectile*>(
        sim->entity_registry().find(proj_id));

    // Create Lua table with projectile metatable
    lua_newtable(L);
    lua_pushstring(L, "_c_object");
    lua_pushlightuserdata(L, proj_ptr);
    lua_rawset(L, -3);

    // Set __osc_proj_mt metatable
    lua_pushstring(L, "__osc_proj_mt");
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        int mt_idx = lua_gettop(L);
        lua_pushstring(L, "__index");
        lua_pushvalue(L, mt_idx);
        lua_rawset(L, mt_idx);
        lua_pushstring(L, "moho");
        lua_rawget(L, LUA_GLOBALSINDEX);
        if (lua_istable(L, -1)) {
            lua_pushstring(L, "projectile_methods");
            lua_rawget(L, -2);
            if (lua_istable(L, -1)) {
                int src_idx = lua_gettop(L);
                lua_pushnil(L);
                while (lua_next(L, src_idx) != 0) {
                    lua_pushvalue(L, -2);
                    lua_pushvalue(L, -2);
                    lua_rawset(L, mt_idx);
                    lua_pop(L, 1);
                }
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
        lua_pushstring(L, "__osc_proj_mt");
        lua_pushvalue(L, mt_idx);
        lua_rawset(L, LUA_REGISTRYINDEX);
    }
    lua_setmetatable(L, -2);

    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    proj_ptr->set_lua_table_ref(ref);
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    return 1;
}

static const MethodEntry weapon_methods[] = {
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
    {"GetProjectileBlueprint",      stub_return_nil},
    {"FireWeapon",                  stub_return_false},
    {"DoInstaHit",                  stub_noop},
    {"SetTargetGround",             stub_noop},
    {"SetTargetingPriorities",      stub_noop},
    {"TransferTarget",              stub_noop},
    {"SetFireControl",              stub_noop},
    {"IsFireControl",               stub_return_false},
    {"SetFiringRandomness",         stub_noop},
    {"GetFiringRandomness",         stub_return_zero},
    {"SetFireTargetLayerCaps",      stub_noop},
    {"ChangeDamageRadius",          stub_noop},
    {"ChangeDamageType",            stub_noop},
    {"ChangeMaxHeightDiff",         stub_noop},
    {"ChangeFiringTolerance",       stub_noop},
    {"ChangeProjectileBlueprint",   stub_noop},
    {"BeenDestroyed",               stub_return_false},
    {"PlaySound",                   stub_noop},
    {"SetValidTargetsForCurrentLayer", stub_noop},
    {"SetWeaponPriorities",         stub_noop},
    {nullptr, nullptr},
};

static const MethodEntry prop_methods[] = {
    {"GetMaxHealth",                entity_GetMaxHealth},
    {"SetMaxHealth",                entity_SetMaxHealth},
    {"GetHealth",                   entity_GetHealth},
    {"SetHealth",                   entity_SetHealth},
    {"AdjustHealth",                entity_AdjustHealth},
    {"GetBlueprint",                entity_GetBlueprint},
    {"GetEntityId",                 entity_GetEntityId},
    {"GetArmy",                     entity_GetArmy},
    {"GetPosition",                 entity_GetPosition},
    {"SetPosition",                 entity_SetPosition},
    {"GetOrientation",              entity_GetOrientation},
    {"SetOrientation",              entity_SetOrientation},
    {"Destroy",                     entity_Destroy},
    {"BeenDestroyed",               entity_BeenDestroyed},
    {"AddBoundedProp",              stub_return_nil},
    {"SetCollisionShape",           stub_noop},
    {"SetMesh",                     stub_noop},
    {"SetScale",                    stub_noop},
    {"SetDrawScale",                stub_noop},
    {"SetVizToAllies",              stub_noop},
    {"SetVizToEnemies",             stub_noop},
    {"SetVizToFocusPlayer",         stub_noop},
    {"SetVizToNeutrals",            stub_noop},
    {"SetReclaimable",              stub_noop},
    {nullptr, nullptr},
};

// ====================================================================
// aibrain_methods — real implementations
// ====================================================================

static int brain_GetArmyIndex(lua_State* L) {
    auto* brain = check_brain(L);
    lua_pushnumber(L, brain ? brain->index() + 1 : 0); // 1-based for Lua
    return 1;
}

static int brain_GetFactionIndex(lua_State* L) {
    auto* brain = check_brain(L);
    lua_pushnumber(L, brain ? brain->faction() : 1);
    return 1;
}

static int brain_GetArmyStartPos(lua_State* L) {
    auto* brain = check_brain(L);
    if (!brain) {
        lua_pushnumber(L, 0);
        lua_pushnumber(L, 0);
        lua_pushnumber(L, 0);
        return 3;
    }
    const auto& pos = brain->start_position();
    lua_pushnumber(L, pos.x);
    lua_pushnumber(L, pos.y);
    lua_pushnumber(L, pos.z);
    return 3;
}

static int brain_GetEconomyIncome(lua_State* L) {
    auto* brain = check_brain(L);
    const char* res = luaL_checkstring(L, 2);
    lua_pushnumber(L, brain ? brain->get_economy_income(res) : 0.0);
    return 1;
}

static int brain_GetEconomyRequested(lua_State* L) {
    auto* brain = check_brain(L);
    const char* res = luaL_checkstring(L, 2);
    lua_pushnumber(L, brain ? brain->get_economy_requested(res) : 0.0);
    return 1;
}

// GetEconomyUsage = actual consumption (capped to income when stalling).
// For now without stalling logic, return same as requested.
static int brain_GetEconomyUsage(lua_State* L) {
    auto* brain = check_brain(L);
    const char* res = luaL_checkstring(L, 2);
    lua_pushnumber(L, brain ? brain->get_economy_requested(res) : 0.0);
    return 1;
}

static int brain_GetEconomyTrend(lua_State* L) {
    auto* brain = check_brain(L);
    const char* res = luaL_checkstring(L, 2);
    lua_pushnumber(L, brain ? brain->get_economy_trend(res) : 0.0);
    return 1;
}

static int brain_GetEconomyStored(lua_State* L) {
    auto* brain = check_brain(L);
    const char* res = luaL_checkstring(L, 2);
    lua_pushnumber(L, brain ? brain->get_economy_stored(res) : 0.0);
    return 1;
}

static int brain_GetEconomyStoredRatio(lua_State* L) {
    auto* brain = check_brain(L);
    const char* res = luaL_checkstring(L, 2);
    lua_pushnumber(L, brain ? brain->get_economy_stored_ratio(res) : 1.0);
    return 1;
}

static int brain_GiveResource(lua_State* L) {
    auto* brain = check_brain(L);
    if (!brain) return 0;
    const char* res = luaL_checkstring(L, 2);
    f64 amount = luaL_checknumber(L, 3);
    auto& econ = brain->economy();
    if (std::strcmp(res, "ENERGY") == 0 || std::strcmp(res, "Energy") == 0) {
        econ.energy.stored = std::min(econ.energy.stored + amount,
                                       econ.energy.max_storage);
    } else if (std::strcmp(res, "MASS") == 0 || std::strcmp(res, "Mass") == 0) {
        econ.mass.stored = std::min(econ.mass.stored + amount,
                                     econ.mass.max_storage);
    }
    return 0;
}

static int brain_IsDefeated(lua_State* L) {
    auto* brain = check_brain(L);
    lua_pushboolean(L, brain ? brain->is_defeated() : 0);
    return 1;
}

static int brain_GetCurrentUnits(lua_State* L) {
    auto* brain = check_brain(L);
    auto* sim = get_sim(L);
    if (brain && sim) {
        lua_pushnumber(L, brain->get_unit_cost_total(sim->entity_registry()));
    } else {
        lua_pushnumber(L, 0);
    }
    return 1;
}

static int brain_GetBrainStatus(lua_State* L) {
    auto* brain = check_brain(L);
    if (!brain) { lua_pushstring(L, ""); return 1; }
    switch (brain->state()) {
        case sim::BrainState::InProgress: lua_pushstring(L, "InProgress"); break;
        case sim::BrainState::Victory:    lua_pushstring(L, "Victory");    break;
        case sim::BrainState::Defeat:     lua_pushstring(L, "Defeat");     break;
        case sim::BrainState::Draw:       lua_pushstring(L, "Draw");       break;
        case sim::BrainState::Recalled:   lua_pushstring(L, "Recalled");   break;
    }
    return 1;
}

static int brain_GetAllianceToArmy(lua_State* L) {
    auto* brain = check_brain(L);
    i32 other = static_cast<i32>(luaL_checknumber(L, 2)) - 1; // 1→0-based
    if (!brain) { lua_pushstring(L, "Enemy"); return 1; }
    auto alliance = brain->get_alliance(other);
    switch (alliance) {
        case sim::Alliance::Ally:    lua_pushstring(L, "Ally");    break;
        case sim::Alliance::Enemy:   lua_pushstring(L, "Enemy");   break;
        case sim::Alliance::Neutral: lua_pushstring(L, "Neutral"); break;
    }
    return 1;
}

static int brain_GetListOfUnits(lua_State* L) {
    auto* brain = check_brain(L);
    auto* sim = get_sim(L);
    if (!brain || !sim) {
        lua_newtable(L);
        return 1;
    }

    // Find the category argument by scanning for the first table arg after self.
    // FA's AIBrain Lua wrapper may insert extra args (e.g., compound category).
    // Some callers pass (self, number, ...) where the number is an army index.
    int cat_idx = 0;
    int top = lua_gettop(L);
    for (int i = 2; i <= top; i++) {
        if (lua_istable(L, i)) {
            cat_idx = i;
            break;
        }
    }
    bool has_category = (cat_idx > 0);
    // needBuilt / needIdle is the first boolean AFTER the category.
    // If no category found, don't assume any filtering.
    int built_idx = has_category ? cat_idx + 1 : -1;
    bool need_built = (built_idx > 0) && lua_toboolean(L, built_idx) != 0;

    auto entities = brain->get_units(sim->entity_registry());

    lua_newtable(L);
    int idx = 1;
    for (auto* entity : entities) {
        if (!entity || entity->destroyed()) continue;
        if (entity->lua_table_ref() < 0) continue;
        if (!entity->is_unit()) continue;
        auto* unit = static_cast<sim::Unit*>(entity);
        if (need_built && unit->is_being_built()) continue;
        if (has_category &&
            !osc::lua::unit_matches_category(L, cat_idx, unit->categories()))
            continue;

        lua_pushnumber(L, idx++);
        lua_rawgeti(L, LUA_REGISTRYINDEX, entity->lua_table_ref());
        lua_rawset(L, -3);
    }
    return 1;
}

static int brain_GetUnitsAroundPoint(lua_State* L) {
    // brain:GetUnitsAroundPoint(category, position, radius, teamIndex)
    // FA wrapper may insert armyIndex at arg 2, shifting everything by 1.
    auto* brain = check_brain(L);
    auto* sim = get_sim(L);
    if (!brain || !sim) {
        lua_newtable(L);
        return 1;
    }

    // Find category arg: first table arg after self (may be arg 2 or 3).
    int cat_arg = 0;
    for (int i = 2; i <= lua_gettop(L); i++) {
        if (lua_istable(L, i)) { cat_arg = i; break; }
    }
    bool has_category = (cat_arg > 0);
    // Position table is next table after category
    int pos_arg = 0;
    if (cat_arg > 0) {
        for (int i = cat_arg + 1; i <= lua_gettop(L); i++) {
            if (lua_istable(L, i)) { pos_arg = i; break; }
        }
    } else {
        // No category — look for first table after any leading numbers
        for (int i = 2; i <= lua_gettop(L); i++) {
            if (lua_istable(L, i)) { pos_arg = i; break; }
        }
    }

    f32 px = 0, pz = 0;
    // Validate pos_arg is a real position (has numeric key 1), not a category tree
    if (pos_arg > 0) {
        lua_rawgeti(L, pos_arg, 1);
        bool is_position = lua_isnumber(L, -1);
        lua_pop(L, 1);
        if (!is_position) pos_arg = 0;
    }
    if (pos_arg > 0) {
        lua_rawgeti(L, pos_arg, 1);
        px = static_cast<f32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        lua_rawgeti(L, pos_arg, 3);
        pz = static_cast<f32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
    }

    // Radius is first number after position
    int radius_arg = (pos_arg > 0) ? pos_arg + 1 : 4;
    f32 radius = static_cast<f32>(lua_tonumber(L, radius_arg));
    if (radius <= 0) radius = 1.0f;

    // teamIndex: "Ally", "Enemy", or nil/empty for own army
    int team_arg = radius_arg + 1;
    const char* team_filter = lua_isstring(L, team_arg)
                                  ? lua_tostring(L, team_arg) : "";

    // Collect entities in radius
    auto ids = sim->entity_registry().collect_in_radius(px, pz, radius);

    lua_newtable(L);
    int idx = 1;
    for (u32 eid : ids) {
        auto* entity = sim->entity_registry().find(eid);
        if (!entity || !entity->is_unit() || entity->destroyed()) continue;
        auto* unit = static_cast<sim::Unit*>(entity);

        // Filter by team relationship
        i32 my_army = brain->index();
        i32 their_army = unit->army();
        if (std::strcmp(team_filter, "Enemy") == 0) {
            if (!sim->is_enemy(my_army, their_army)) continue;
        } else if (std::strcmp(team_filter, "Ally") == 0) {
            if (!sim->is_ally(my_army, their_army) &&
                their_army != my_army) continue;
        } else {
            // Default: own army only
            if (their_army != my_army) continue;
        }

        if (unit->lua_table_ref() < 0) continue;
        if (has_category &&
            !osc::lua::unit_matches_category(L, cat_arg, unit->categories()))
            continue;

        lua_pushnumber(L, idx++);
        lua_rawgeti(L, LUA_REGISTRYINDEX, unit->lua_table_ref());
        lua_rawset(L, -3);
    }
    return 1;
}

static int brain_GetArmyStat(lua_State* L) {
    // GetArmyStat(self, statName, defaultValue) -> { Value = defaultValue }
    f64 def_val = 0;
    if (lua_isnumber(L, 3)) {
        def_val = lua_tonumber(L, 3);
    }
    lua_newtable(L);
    lua_pushstring(L, "Value");
    lua_pushnumber(L, def_val);
    lua_rawset(L, -3);
    return 1;
}

static int brain_SetArmyStat(lua_State*) { return 0; }

// GetBlueprintStat(self, statName, category) -> number
// Unlike GetArmyStat (returns {Value=n}), this returns a plain number.
// score.lua does arithmetic on the result.
static int brain_GetBlueprintStat(lua_State* L) {
    lua_pushnumber(L, 0);
    return 1;
}

static int brain_GetEconomyOverTime(lua_State* L) {
    // Returns a table with income/spending averages over time
    // For now, return current values
    auto* brain = check_brain(L);
    lua_newtable(L);
    if (brain) {
        const auto& econ = brain->economy();
        lua_pushstring(L, "MassIncome");
        lua_pushnumber(L, econ.mass.income);
        lua_rawset(L, -3);
        lua_pushstring(L, "MassConsumed");
        lua_pushnumber(L, econ.mass.requested);
        lua_rawset(L, -3);
        lua_pushstring(L, "EnergyIncome");
        lua_pushnumber(L, econ.energy.income);
        lua_rawset(L, -3);
        lua_pushstring(L, "EnergyConsumed");
        lua_pushnumber(L, econ.energy.requested);
        lua_rawset(L, -3);
    }
    return 1;
}

static int brain_SetArmyColor(lua_State* L) {
    auto* brain = check_brain(L);
    if (brain) {
        u8 r = static_cast<u8>(luaL_checknumber(L, 2));
        u8 g = static_cast<u8>(luaL_checknumber(L, 3));
        u8 b = static_cast<u8>(luaL_checknumber(L, 4));
        brain->set_color(r, g, b);
    }
    return 0;
}

// ====================================================================
// Brain threat methods
// ====================================================================

// brain:GetThreatAtPosition(pos, rings, checkVis, threatType[, armyIdx])
// Returns total threat of enemies (or specific army) within radius.
static int brain_GetThreatAtPosition(lua_State* L) {
    auto* brain = check_brain(L);
    auto* sim = get_sim(L);
    if (!brain || !sim) {
        lua_pushnumber(L, 0);
        return 1;
    }

    // Extract position from arg 2
    f32 px = 0, pz = 0;
    if (lua_istable(L, 2)) {
        lua_rawgeti(L, 2, 1);
        px = static_cast<f32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        lua_rawgeti(L, 2, 3);
        pz = static_cast<f32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
    }

    // rings → radius (rings=0 means pinpoint query)
    i32 rings = static_cast<i32>(lua_tonumber(L, 3));
    f32 radius = rings <= 0 ? 1.0f : static_cast<f32>(rings) * 32.0f;

    // arg 4 = checkVis (ignored)
    const char* threat_type = lua_isstring(L, 5) ? lua_tostring(L, 5) : "Overall";

    // Optional armyIdx (arg 6) — filter to specific army instead of enemies
    bool filter_specific = lua_isnumber(L, 6);
    i32 specific_army = filter_specific
                            ? static_cast<i32>(lua_tonumber(L, 6)) - 1 // Lua 1-based
                            : -1;

    auto ids = sim->entity_registry().collect_in_radius(px, pz, radius);

    f32 total = 0;
    for (u32 eid : ids) {
        auto* entity = sim->entity_registry().find(eid);
        if (!entity || !entity->is_unit() || entity->destroyed()) continue;
        auto* unit = static_cast<sim::Unit*>(entity);

        if (filter_specific) {
            if (unit->army() != specific_army) continue;
        } else {
            if (!sim->is_enemy(brain->index(), unit->army())) continue;
        }

        total += get_unit_threat_for_type(unit, threat_type);
    }

    lua_pushnumber(L, total);
    return 1;
}

// brain:GetThreatsAroundPosition(pos, rings, checkVis, threatType)
// Returns table of {cellX, cellZ, threatValue} entries bucketed into 32x32 cells.
static int brain_GetThreatsAroundPosition(lua_State* L) {
    auto* brain = check_brain(L);
    auto* sim = get_sim(L);
    if (!brain || !sim) {
        lua_newtable(L);
        return 1;
    }

    f32 px = 0, pz = 0;
    if (lua_istable(L, 2)) {
        lua_rawgeti(L, 2, 1);
        px = static_cast<f32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        lua_rawgeti(L, 2, 3);
        pz = static_cast<f32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
    }

    i32 rings = static_cast<i32>(lua_tonumber(L, 3));
    f32 radius = rings <= 0 ? 1.0f : static_cast<f32>(rings) * 32.0f;
    const char* threat_type = lua_isstring(L, 5) ? lua_tostring(L, 5) : "Overall";

    // Optional armyIdx (arg 6) — filter to specific army instead of enemies
    bool filter_specific = lua_isnumber(L, 6);
    i32 specific_army = filter_specific
                            ? static_cast<i32>(lua_tonumber(L, 6)) - 1
                            : -1;

    auto ids = sim->entity_registry().collect_in_radius(px, pz, radius);

    // Bucket threats into 32x32 cells using a map keyed by (cellX, cellZ)
    constexpr f32 CELL_SIZE = 32.0f;
    struct CellKey {
        i32 cx, cz;
        bool operator==(const CellKey& o) const { return cx == o.cx && cz == o.cz; }
    };
    struct CellHash {
        size_t operator()(const CellKey& k) const {
            return std::hash<i64>()(static_cast<i64>(k.cx) << 32 | static_cast<u32>(k.cz));
        }
    };
    std::unordered_map<CellKey, f32, CellHash> cells;

    for (u32 eid : ids) {
        auto* entity = sim->entity_registry().find(eid);
        if (!entity || !entity->is_unit() || entity->destroyed()) continue;
        auto* unit = static_cast<sim::Unit*>(entity);

        if (filter_specific) {
            if (unit->army() != specific_army) continue;
        } else {
            if (!sim->is_enemy(brain->index(), unit->army())) continue;
        }

        f32 threat = get_unit_threat_for_type(unit, threat_type);
        if (threat <= 0) continue;

        CellKey key{
            static_cast<i32>(std::floor(unit->position().x / CELL_SIZE)),
            static_cast<i32>(std::floor(unit->position().z / CELL_SIZE))};
        cells[key] += threat;
    }

    // Return table of {cellX, cellZ, threatValue}
    lua_newtable(L);
    int idx = 1;
    for (const auto& [key, threat] : cells) {
        lua_pushnumber(L, idx++);
        lua_newtable(L);
        lua_pushnumber(L, 1);
        lua_pushnumber(L, key.cx * CELL_SIZE + CELL_SIZE * 0.5f);
        lua_rawset(L, -3);
        lua_pushnumber(L, 2);
        lua_pushnumber(L, key.cz * CELL_SIZE + CELL_SIZE * 0.5f);
        lua_rawset(L, -3);
        lua_pushnumber(L, 3);
        lua_pushnumber(L, threat);
        lua_rawset(L, -3);
        lua_rawset(L, -3); // result[idx] = entry
    }
    return 1;
}

// brain:GetHighestThreatPosition(rings, checkVis, threatType[, armyIdx])
// Returns position, threat (2 return values). Iterates ALL entities.
static int brain_GetHighestThreatPosition(lua_State* L) {
    auto* brain = check_brain(L);
    auto* sim = get_sim(L);
    if (!brain || !sim) {
        push_vector3(L, {0, 0, 0});
        lua_pushnumber(L, 0);
        return 2;
    }

    // arg 2 = rings (unused — we scan whole map)
    // arg 3 = checkVis (ignored)
    const char* threat_type = lua_isstring(L, 4) ? lua_tostring(L, 4) : "Overall";

    bool filter_specific = lua_isnumber(L, 5);
    i32 specific_army = filter_specific
                            ? static_cast<i32>(lua_tonumber(L, 5)) - 1
                            : -1;

    f32 best_threat = 0;
    sim::Vector3 best_pos{0, 0, 0};

    sim->entity_registry().for_each([&](const sim::Entity& e) {
        if (!e.is_unit() || e.destroyed()) return;
        auto* unit = static_cast<const sim::Unit*>(&e);

        if (filter_specific) {
            if (unit->army() != specific_army) return;
        } else {
            if (!sim->is_enemy(brain->index(), unit->army())) return;
        }

        f32 t = get_unit_threat_for_type(unit, threat_type);
        if (t > best_threat) {
            best_threat = t;
            best_pos = unit->position();
        }
    });

    push_vector3(L, best_pos);
    lua_pushnumber(L, best_threat);
    return 2;
}

// ====================================================================
// Brain enemy & counting methods
// ====================================================================

// brain:GetCurrentEnemy() → enemy brain Lua table or nil
static int brain_GetCurrentEnemy(lua_State* L) {
    auto* brain = check_brain(L);
    auto* sim = get_sim(L);
    if (!brain || !sim || brain->current_enemy_index() < 0) {
        lua_pushnil(L);
        return 1;
    }

    auto* enemy = sim->get_army(brain->current_enemy_index());
    if (!enemy || enemy->lua_table_ref() < 0) {
        lua_pushnil(L);
        return 1;
    }

    lua_rawgeti(L, LUA_REGISTRYINDEX, enemy->lua_table_ref());
    return 1;
}

// brain:SetCurrentEnemy(other_brain_or_nil)
static int brain_SetCurrentEnemy(lua_State* L) {
    auto* brain = check_brain(L);
    if (!brain) return 0;

    if (lua_isnil(L, 2) || !lua_istable(L, 2)) {
        brain->set_current_enemy_index(-1);
        return 0;
    }

    auto* enemy = check_brain(L, 2);
    if (enemy) {
        brain->set_current_enemy_index(enemy->index());
    } else {
        brain->set_current_enemy_index(-1);
    }
    return 0;
}

// brain:GetNumUnitsAroundPoint(cat, pos, radius, team) → number
// Same arg parsing as brain_GetUnitsAroundPoint but returns count.
static int brain_GetNumUnitsAroundPoint(lua_State* L) {
    auto* brain = check_brain(L);
    auto* sim = get_sim(L);
    if (!brain || !sim) {
        lua_pushnumber(L, 0);
        return 1;
    }

    // Find category arg: first table arg after self
    int cat_arg = 0;
    for (int i = 2; i <= lua_gettop(L); i++) {
        if (lua_istable(L, i)) { cat_arg = i; break; }
    }
    bool has_category = (cat_arg > 0);

    // Position table: next table after category
    int pos_arg = 0;
    if (cat_arg > 0) {
        for (int i = cat_arg + 1; i <= lua_gettop(L); i++) {
            if (lua_istable(L, i)) { pos_arg = i; break; }
        }
    } else {
        for (int i = 2; i <= lua_gettop(L); i++) {
            if (lua_istable(L, i)) { pos_arg = i; break; }
        }
    }

    f32 px = 0, pz = 0;
    if (pos_arg > 0) {
        lua_rawgeti(L, pos_arg, 1);
        bool is_position = lua_isnumber(L, -1);
        lua_pop(L, 1);
        if (!is_position) pos_arg = 0;
    }
    if (pos_arg > 0) {
        lua_rawgeti(L, pos_arg, 1);
        px = static_cast<f32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        lua_rawgeti(L, pos_arg, 3);
        pz = static_cast<f32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
    }

    int radius_arg = (pos_arg > 0) ? pos_arg + 1 : 4;
    f32 radius = static_cast<f32>(lua_tonumber(L, radius_arg));
    if (radius <= 0) radius = 1.0f;

    int team_arg = radius_arg + 1;
    const char* team_filter = lua_isstring(L, team_arg)
                                  ? lua_tostring(L, team_arg) : "";

    auto ids = sim->entity_registry().collect_in_radius(px, pz, radius);

    int count = 0;
    for (u32 eid : ids) {
        auto* entity = sim->entity_registry().find(eid);
        if (!entity || !entity->is_unit() || entity->destroyed()) continue;
        auto* unit = static_cast<sim::Unit*>(entity);

        i32 my_army = brain->index();
        i32 their_army = unit->army();
        if (std::strcmp(team_filter, "Enemy") == 0) {
            if (!sim->is_enemy(my_army, their_army)) continue;
        } else if (std::strcmp(team_filter, "Ally") == 0) {
            if (!sim->is_ally(my_army, their_army) &&
                their_army != my_army) continue;
        } else {
            if (their_army != my_army) continue;
        }

        if (unit->lua_table_ref() < 0) continue;

        if (has_category &&
            !osc::lua::unit_matches_category(L, cat_arg, unit->categories()))
            continue;

        count++;
    }

    lua_pushnumber(L, count);
    return 1;
}

// brain:GetPlatoonsList() → table of platoon Lua tables
static int brain_GetPlatoonsList(lua_State* L) {
    auto* brain = check_brain(L);
    if (!brain) {
        lua_newtable(L);
        return 1;
    }

    lua_newtable(L);
    int idx = 1;
    for (size_t i = 0; i < brain->platoon_count(); i++) {
        auto* p = brain->platoon_at(i);
        if (!p || p->destroyed() || p->lua_table_ref() < 0) continue;
        lua_pushnumber(L, idx++);
        lua_rawgeti(L, LUA_REGISTRYINDEX, p->lua_table_ref());
        lua_rawset(L, -3);
    }
    return 1;
}

// FindPlaceToBuild(type, structureName, buildingTypes, relative, builder,
//                  optIgnoreAlliance, optOverridePosX, optOverridePosZ, optIgnoreThreatOver)
// Returns {x, z, dist} or false. Uses a grid offset from the reference
// position to avoid stacking structures (no real obstruction checking yet).
static int brain_FindPlaceToBuild(lua_State* L) {
    auto* brain = check_brain(L);
    if (!brain) { lua_pushboolean(L, 0); return 1; }

    f32 x, z;
    // Args 7,8 are optional override position
    if (lua_isnumber(L, 7) && lua_isnumber(L, 8)) {
        x = static_cast<f32>(lua_tonumber(L, 7));
        z = static_cast<f32>(lua_tonumber(L, 8));
    } else {
        x = brain->start_position().x;
        z = brain->start_position().z;
    }

    // Grid offset: spread structures in a 5-wide grid, 8 ogrids apart
    i32 idx = brain->next_build_place_index();
    i32 col = idx % 5;
    i32 row = idx / 5;
    x += static_cast<f32>(col - 2) * 8.0f;
    z += static_cast<f32>(row + 1) * 8.0f;

    // Return {x, z, 0} (distance=0 since we placed it exactly)
    lua_newtable(L);
    lua_pushnumber(L, 1);
    lua_pushnumber(L, x);
    lua_rawset(L, -3);
    lua_pushnumber(L, 2);
    lua_pushnumber(L, z);
    lua_rawset(L, -3);
    lua_pushnumber(L, 3);
    lua_pushnumber(L, 0); // dist
    lua_rawset(L, -3);
    return 1;
}

// brain:BuildStructure(unit, whatToBuild, location, relative)
// unit = arg 2 (Lua table with _c_object), whatToBuild = arg 3 (bp string),
// location = arg 4 ({x, z, dist} from FindPlaceToBuild)
static int brain_BuildStructure(lua_State* L) {
    if (!lua_istable(L, 2)) return 0;
    lua_pushstring(L, "_c_object");
    lua_rawget(L, 2);
    auto* e = lua_isuserdata(L, -1)
                  ? static_cast<sim::Entity*>(lua_touserdata(L, -1))
                  : nullptr;
    lua_pop(L, 1);
    if (!e || !e->is_unit() || e->destroyed()) return 0;
    auto* unit = static_cast<sim::Unit*>(e);

    const char* bp_id = luaL_checkstring(L, 3);

    sim::Vector3 pos;
    if (lua_istable(L, 4)) {
        lua_rawgeti(L, 4, 1);
        pos.x = static_cast<f32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        lua_rawgeti(L, 4, 2);
        pos.z = static_cast<f32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
    }

    sim::UnitCommand cmd;
    cmd.type = sim::CommandType::BuildMobile;
    cmd.target_pos = pos;
    cmd.blueprint_id = bp_id;
    unit->push_command(cmd, false);
    return 0;
}

// brain:BuildUnit(factory, blueprintId)
// factory = arg 2 (Lua table with _c_object), blueprintId = arg 3 (string)
static int brain_BuildUnit(lua_State* L) {
    if (!lua_istable(L, 2)) return 0;
    lua_pushstring(L, "_c_object");
    lua_rawget(L, 2);
    auto* e = lua_isuserdata(L, -1)
                  ? static_cast<sim::Entity*>(lua_touserdata(L, -1))
                  : nullptr;
    lua_pop(L, 1);
    if (!e || !e->is_unit() || e->destroyed()) return 0;
    auto* unit = static_cast<sim::Unit*>(e);

    const char* bp_id = luaL_checkstring(L, 3);

    sim::UnitCommand cmd;
    cmd.type = sim::CommandType::BuildFactory;
    cmd.blueprint_id = bp_id;
    unit->push_command(cmd, false);
    return 0;
}

// brain:DecideWhatToBuild(builder, buildingType, template)
// builder = arg 2, buildingType = arg 3 (string),
// template = arg 4 (array of {typeName, bpId} pairs)
// Returns bpId string or nil.
static int brain_DecideWhatToBuild(lua_State* L) {
    const char* building_type = luaL_checkstring(L, 3);
    if (!lua_istable(L, 4)) { lua_pushnil(L); return 1; }

    // Iterate template array: each entry is {[1]=typeName, [2]=bpId}
    int len = luaL_getn(L, 4);
    for (int i = 1; i <= len; i++) {
        lua_rawgeti(L, 4, i);
        if (lua_istable(L, -1)) {
            lua_rawgeti(L, -1, 1); // typeName
            const char* tn = lua_tostring(L, -1);
            if (tn && std::strcmp(tn, building_type) == 0) {
                lua_pop(L, 1); // pop typeName
                lua_rawgeti(L, -1, 2); // bpId
                // Move result below the template entry then pop template entry
                lua_remove(L, -2);
                return 1;
            }
            lua_pop(L, 1); // pop typeName
        }
        lua_pop(L, 1); // pop entry
    }
    lua_pushnil(L);
    return 1;
}

// brain:GetUnitBlueprint(bpId) — returns blueprint table from __blueprints
static int brain_GetUnitBlueprint(lua_State* L) {
    const char* bp_id = luaL_checkstring(L, 2);
    lua_pushstring(L, "__blueprints");
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_pushnil(L);
        return 1;
    }
    lua_pushstring(L, bp_id);
    lua_rawget(L, -2);
    lua_remove(L, -2); // remove __blueprints table
    return 1;
}

// ====================================================================
// Brain platoon methods
// ====================================================================

static int brain_MakePlatoon(lua_State* L) {
    auto* brain = check_brain(L);
    auto* sim = get_sim(L);
    if (!brain || !sim) { lua_pushnil(L); return 1; }

    const char* name = lua_isstring(L, 2) ? lua_tostring(L, 2) : "";
    const char* plan = lua_isstring(L, 3) ? lua_tostring(L, 3) : "";

    auto* platoon = brain->create_platoon(name);
    platoon->set_plan_name(plan);

    // Create Lua table: { _c_object = lightuserdata(Platoon*) }
    lua_newtable(L);
    int plat_tbl = lua_gettop(L);

    lua_pushstring(L, "_c_object");
    lua_pushlightuserdata(L, platoon);
    lua_rawset(L, plat_tbl);

    // Set PlanName on the Lua table
    lua_pushstring(L, "PlanName");
    lua_pushstring(L, plan);
    lua_rawset(L, plat_tbl);

    // Set metatable: try __platoon_class, fallback to __osc_platoon_mt
    lua_pushstring(L, "__platoon_class");
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        // Build cached metatable from moho.platoon_methods
        lua_pushstring(L, "__osc_platoon_mt");
        lua_rawget(L, LUA_REGISTRYINDEX);
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            // Create it: { __index = methods_table }
            lua_newtable(L); // metatable
            lua_pushstring(L, "__index");
            // Get moho.platoon_methods
            lua_pushstring(L, "moho");
            lua_rawget(L, LUA_GLOBALSINDEX);
            if (lua_istable(L, -1)) {
                lua_pushstring(L, "platoon_methods");
                lua_rawget(L, -2);
                lua_remove(L, -2); // remove moho table
            } else {
                lua_pop(L, 1);     // pop the non-table
                lua_pushnil(L);    // explicit nil — no methods
            }
            lua_settable(L, -3); // metatable.__index = platoon_methods
            // Cache it
            lua_pushstring(L, "__osc_platoon_mt");
            lua_pushvalue(L, -2);
            lua_rawset(L, LUA_REGISTRYINDEX);
        }
    }
    lua_setmetatable(L, plat_tbl);

    // Store lua_table_ref
    lua_pushvalue(L, plat_tbl);
    platoon->set_lua_table_ref(luaL_ref(L, LUA_REGISTRYINDEX));

    // Call OnCreate(self, plan) if method exists
    lua_pushstring(L, "OnCreate");
    lua_gettable(L, plat_tbl);
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, plat_tbl); // self
        lua_pushstring(L, plan);
        if (lua_pcall(L, 2, 0, 0) != 0) {
            spdlog::warn("Platoon OnCreate error: {}", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    } else {
        lua_pop(L, 1);
    }

    spdlog::info("MakePlatoon: id={} army={} name='{}' plan='{}'",
                 platoon->platoon_id(), brain->index(), name, plan);

    // Return the Lua table
    lua_pushvalue(L, plat_tbl);
    return 1;
}

static int brain_AssignUnitsToPlatoon(lua_State* L) {
    auto* platoon = check_platoon(L, 2);
    if (!platoon) return 0;

    const char* squad = lua_isstring(L, 4) ? lua_tostring(L, 4) : "Unassigned";

    // Iterate units table (arg 3)
    if (!lua_istable(L, 3)) return 0;
    int units_tbl = 3;
    int n = luaL_getn(L, units_tbl);

    for (int i = 1; i <= n; i++) {
        lua_rawgeti(L, units_tbl, i);
        int unit_tbl = lua_gettop(L);

        lua_pushstring(L, "_c_object");
        lua_rawget(L, unit_tbl);
        auto* e = lua_isuserdata(L, -1)
                      ? static_cast<sim::Entity*>(lua_touserdata(L, -1))
                      : nullptr;
        lua_pop(L, 1);

        if (e && e->is_unit() && !e->destroyed()) {
            platoon->add_unit(e->entity_id());
            platoon->set_unit_squad(e->entity_id(), squad);

            // Set unit_lua_table.PlatoonHandle = platoon_lua_table
            lua_pushstring(L, "PlatoonHandle");
            lua_pushvalue(L, 2); // platoon Lua table
            lua_rawset(L, unit_tbl);
        }
        lua_pop(L, 1); // pop unit_tbl
    }

    // Call platoon:OnUnitsAddedToPlatoon() if it exists
    if (platoon->lua_table_ref() >= 0) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, platoon->lua_table_ref());
        int ptbl = lua_gettop(L);
        lua_pushstring(L, "OnUnitsAddedToPlatoon");
        lua_gettable(L, ptbl);
        if (lua_isfunction(L, -1)) {
            lua_pushvalue(L, ptbl);
            if (lua_pcall(L, 1, 0, 0) != 0) {
                spdlog::warn("OnUnitsAddedToPlatoon error: {}",
                             lua_tostring(L, -1));
                lua_pop(L, 1);
            }
        } else {
            lua_pop(L, 1);
        }
        lua_pop(L, 1); // ptbl
    }

    return 0;
}

static int brain_PlatoonExists(lua_State* L) {
    // FA semantics: true if platoon is valid and not destroyed.
    // check_platoon returns nullptr for destroyed or nil platoons.
    // Empty platoons (no units yet) are still considered existing.
    auto* platoon = check_platoon(L, 2);
    lua_pushboolean(L, platoon ? 1 : 0);
    return 1;
}

static int brain_DisbandPlatoon(lua_State* L) {
    auto* platoon = check_platoon(L, 2);
    if (!platoon) return 0;

    auto* sim = get_sim(L);

    // Clear PlatoonHandle on all units
    if (sim) {
        for (u32 id : platoon->unit_ids()) {
            auto* e = sim->entity_registry().find(id);
            if (e && !e->destroyed() && e->lua_table_ref() >= 0) {
                lua_rawgeti(L, LUA_REGISTRYINDEX, e->lua_table_ref());
                lua_pushstring(L, "PlatoonHandle");
                lua_pushnil(L);
                lua_rawset(L, -3);
                lua_pop(L, 1);
            }
        }
    }

    // Call platoon:OnDestroy() if exists
    if (platoon->lua_table_ref() >= 0) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, platoon->lua_table_ref());
        int ptbl = lua_gettop(L);
        lua_pushstring(L, "OnDestroy");
        lua_gettable(L, ptbl);
        if (lua_isfunction(L, -1)) {
            lua_pushvalue(L, ptbl);
            if (lua_pcall(L, 1, 0, 0) != 0) {
                spdlog::warn("Platoon OnDestroy error: {}", lua_tostring(L, -1));
                lua_pop(L, 1);
            }
        } else {
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }

    // Null out _c_object on the platoon Lua table
    if (lua_istable(L, 2)) {
        lua_pushstring(L, "_c_object");
        lua_pushnil(L);
        lua_rawset(L, 2);
    }

    // Unref and destroy
    if (platoon->lua_table_ref() >= 0) {
        luaL_unref(L, LUA_REGISTRYINDEX, platoon->lua_table_ref());
        platoon->set_lua_table_ref(-2);
    }

    auto* brain = check_brain(L);
    if (brain) brain->destroy_platoon(platoon);

    return 0;
}

static int brain_GetPlatoonUniquelyNamed(lua_State* L) {
    auto* brain = check_brain(L);
    if (!brain) { lua_pushnil(L); return 1; }

    const char* name = luaL_checkstring(L, 2);
    auto* platoon = brain->find_platoon_by_name(name);

    if (platoon && platoon->lua_table_ref() >= 0) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, platoon->lua_table_ref());
    } else {
        lua_pushnil(L);
    }
    return 1;
}

// ====================================================================
// Platoon instance methods
// ====================================================================

static int platoon_GetPlatoonUnits(lua_State* L) {
    auto* platoon = check_platoon(L);
    auto* sim = get_sim(L);
    lua_newtable(L);
    if (!platoon || !sim) return 1;

    int result = lua_gettop(L);
    int idx = 1;
    for (u32 id : platoon->unit_ids()) {
        auto* e = sim->entity_registry().find(id);
        if (e && !e->destroyed() && e->lua_table_ref() >= 0) {
            lua_pushnumber(L, idx++);
            lua_rawgeti(L, LUA_REGISTRYINDEX, e->lua_table_ref());
            lua_rawset(L, result);
        }
    }
    return 1;
}

static int platoon_GetSquadUnits(lua_State* L) {
    auto* platoon = check_platoon(L);
    auto* sim = get_sim(L);
    lua_newtable(L);
    if (!platoon || !sim) return 1;

    std::string squad = lua_isstring(L, 2) ? lua_tostring(L, 2) : "";
    int result = lua_gettop(L);
    int idx = 1;
    for (u32 id : platoon->unit_ids()) {
        auto* e = sim->entity_registry().find(id);
        if (!e || e->destroyed() || e->lua_table_ref() < 0) continue;
        if (!squad.empty() && platoon->get_unit_squad(id) != squad) continue;
        lua_pushnumber(L, idx++);
        lua_rawgeti(L, LUA_REGISTRYINDEX, e->lua_table_ref());
        lua_rawset(L, result);
    }
    return 1;
}

static int platoon_GetPlatoonPosition(lua_State* L) {
    auto* platoon = check_platoon(L);
    auto* sim = get_sim(L);
    if (!platoon || !sim) { lua_pushnil(L); return 1; }

    auto pos = platoon->get_position(sim->entity_registry());
    // Check if platoon has any units (position will be 0,0,0 if empty)
    bool has_units = false;
    for (u32 id : platoon->unit_ids()) {
        auto* e = sim->entity_registry().find(id);
        if (e && !e->destroyed()) { has_units = true; break; }
    }
    if (!has_units) { lua_pushnil(L); return 1; }

    push_vector3(L, pos);
    return 1;
}

static int platoon_GetBrain(lua_State* L) {
    auto* platoon = check_platoon(L);
    auto* sim = get_sim(L);
    if (!platoon || !sim) { lua_pushnil(L); return 1; }

    auto* brain = sim->get_army(platoon->army_index());
    if (!brain || brain->lua_table_ref() < 0) { lua_pushnil(L); return 1; }

    lua_rawgeti(L, LUA_REGISTRYINDEX, brain->lua_table_ref());
    return 1;
}

static int platoon_UniquelyNamePlatoon(lua_State* L) {
    auto* platoon = check_platoon(L);
    if (platoon && lua_isstring(L, 2))
        platoon->set_name(lua_tostring(L, 2));
    return 0;
}

static int platoon_Stop(lua_State* L) {
    auto* platoon = check_platoon(L);
    auto* sim = get_sim(L);
    if (!platoon || !sim) return 0;

    for (u32 id : platoon->unit_ids()) {
        auto* e = sim->entity_registry().find(id);
        if (e && !e->destroyed() && e->is_unit())
            static_cast<sim::Unit*>(e)->clear_commands();
    }
    return 0;
}

static int platoon_MoveToLocation(lua_State* L) {
    auto* platoon = check_platoon(L);
    auto* sim = get_sim(L);
    if (!platoon || !sim || !lua_istable(L, 2)) {
        lua_pushnil(L);
        return 1;
    }

    // Extract position from arg 2
    sim::Vector3 pos{};
    lua_rawgeti(L, 2, 1);
    if (lua_isnumber(L, -1)) pos.x = static_cast<f32>(lua_tonumber(L, -1));
    lua_pop(L, 1);
    lua_rawgeti(L, 2, 2);
    if (lua_isnumber(L, -1)) pos.y = static_cast<f32>(lua_tonumber(L, -1));
    lua_pop(L, 1);
    lua_rawgeti(L, 2, 3);
    if (lua_isnumber(L, -1)) pos.z = static_cast<f32>(lua_tonumber(L, -1));
    lua_pop(L, 1);

    u32 cmd_id = sim->next_command_id();
    sim::UnitCommand cmd;
    cmd.type = sim::CommandType::Move;
    cmd.target_pos = pos;
    cmd.command_id = cmd_id;

    for (u32 id : platoon->unit_ids()) {
        auto* e = sim->entity_registry().find(id);
        if (e && !e->destroyed() && e->is_unit())
            static_cast<sim::Unit*>(e)->push_command(cmd, true);
    }
    lua_pushnumber(L, cmd_id);
    return 1;
}

static int platoon_Patrol(lua_State* L) {
    auto* platoon = check_platoon(L);
    auto* sim = get_sim(L);
    if (!platoon || !sim || !lua_istable(L, 2)) {
        lua_pushnil(L);
        return 1;
    }

    sim::Vector3 pos{};
    lua_rawgeti(L, 2, 1);
    if (lua_isnumber(L, -1)) pos.x = static_cast<f32>(lua_tonumber(L, -1));
    lua_pop(L, 1);
    lua_rawgeti(L, 2, 2);
    if (lua_isnumber(L, -1)) pos.y = static_cast<f32>(lua_tonumber(L, -1));
    lua_pop(L, 1);
    lua_rawgeti(L, 2, 3);
    if (lua_isnumber(L, -1)) pos.z = static_cast<f32>(lua_tonumber(L, -1));
    lua_pop(L, 1);

    u32 cmd_id = sim->next_command_id();
    sim::UnitCommand cmd;
    cmd.type = sim::CommandType::Patrol;
    cmd.target_pos = pos;
    cmd.command_id = cmd_id;

    for (u32 id : platoon->unit_ids()) {
        auto* e = sim->entity_registry().find(id);
        if (e && !e->destroyed() && e->is_unit())
            static_cast<sim::Unit*>(e)->push_command(cmd, false); // append
    }
    lua_pushnumber(L, cmd_id);
    return 1;
}

static int platoon_AttackTarget(lua_State* L) {
    auto* platoon = check_platoon(L);
    auto* sim = get_sim(L);
    if (!platoon || !sim) { lua_pushnil(L); return 1; }

    // Extract target entity from arg 2
    auto* target = check_entity(L, 2);
    if (!target || target->destroyed()) { lua_pushnil(L); return 1; }

    u32 cmd_id = sim->next_command_id();
    sim::UnitCommand cmd;
    cmd.type = sim::CommandType::Attack;
    cmd.target_id = target->entity_id();
    cmd.target_pos = target->position();
    cmd.command_id = cmd_id;

    for (u32 id : platoon->unit_ids()) {
        auto* e = sim->entity_registry().find(id);
        if (e && !e->destroyed() && e->is_unit())
            static_cast<sim::Unit*>(e)->push_command(cmd, true);
    }
    lua_pushnumber(L, cmd_id);
    return 1;
}

static int platoon_GuardTarget(lua_State* L) {
    auto* platoon = check_platoon(L);
    auto* sim = get_sim(L);
    if (!platoon || !sim) { lua_pushnil(L); return 1; }

    auto* target = check_entity(L, 2);
    if (!target || target->destroyed()) { lua_pushnil(L); return 1; }

    u32 cmd_id = sim->next_command_id();
    sim::UnitCommand cmd;
    cmd.type = sim::CommandType::Guard;
    cmd.target_id = target->entity_id();
    cmd.command_id = cmd_id;

    for (u32 id : platoon->unit_ids()) {
        auto* e = sim->entity_registry().find(id);
        if (e && !e->destroyed() && e->is_unit())
            static_cast<sim::Unit*>(e)->push_command(cmd, true);
    }
    lua_pushnumber(L, cmd_id);
    return 1;
}

static int platoon_ForkThread(lua_State* L) {
    auto* sim = get_sim(L);
    if (!sim) { lua_pushnil(L); return 1; }
    // Stack: [1]=self, [2]=fn, [3..n]=extra args
    // fork_thread expects: [1]=fn, [2..n]=args
    // Swap: move fn to pos 1, self becomes first arg
    if (lua_gettop(L) < 2 || !lua_isfunction(L, 2)) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushvalue(L, 2); // copy fn to top
    lua_remove(L, 2);    // remove fn from pos 2
    lua_insert(L, 1);    // move fn from top to pos 1
    // Stack is now: [1]=fn, [2]=self, [3..n]=extra args
    return sim->thread_manager().fork_thread(L);
}

// platoon:SetAIPlan(planName)
static int platoon_SetAIPlan(lua_State* L) {
    auto* platoon = check_platoon(L);
    if (!platoon) return 0;

    const char* plan = lua_isstring(L, 2) ? lua_tostring(L, 2) : "";
    platoon->set_plan_name(plan);

    // Also set PlanName on the Lua table for FA compatibility
    if (lua_istable(L, 1)) {
        lua_pushstring(L, "PlanName");
        lua_pushstring(L, plan);
        lua_rawset(L, 1);
    }
    return 0;
}

// platoon:GetPlan() → plan name string or nil
static int platoon_GetPlan(lua_State* L) {
    auto* platoon = check_platoon(L);
    if (!platoon) {
        lua_pushnil(L);
        return 1;
    }

    // Try C++ first
    if (!platoon->plan_name().empty()) {
        lua_pushstring(L, platoon->plan_name().c_str());
        return 1;
    }

    // Fallback: check Lua table PlanName field
    if (lua_istable(L, 1)) {
        lua_pushstring(L, "PlanName");
        lua_rawget(L, 1);
        if (lua_isstring(L, -1)) return 1;
        lua_pop(L, 1);
    }

    lua_pushnil(L);
    return 1;
}

static int platoon_Destroy(lua_State* L) {
    auto* platoon = check_platoon(L);
    if (!platoon) return 0;

    auto* sim = get_sim(L);

    // Clear PlatoonHandle on all member units (same as DisbandPlatoon)
    if (sim) {
        for (u32 id : platoon->unit_ids()) {
            auto* e = sim->entity_registry().find(id);
            if (e && !e->destroyed() && e->lua_table_ref() >= 0) {
                lua_rawgeti(L, LUA_REGISTRYINDEX, e->lua_table_ref());
                lua_pushstring(L, "PlatoonHandle");
                lua_pushnil(L);
                lua_rawset(L, -3);
                lua_pop(L, 1);
            }
        }
    }

    // Call OnDestroy if exists
    if (platoon->lua_table_ref() >= 0) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, platoon->lua_table_ref());
        int ptbl = lua_gettop(L);
        lua_pushstring(L, "OnDestroy");
        lua_gettable(L, ptbl);
        if (lua_isfunction(L, -1)) {
            lua_pushvalue(L, ptbl);
            if (lua_pcall(L, 1, 0, 0) != 0) {
                spdlog::warn("Platoon OnDestroy error: {}", lua_tostring(L, -1));
                lua_pop(L, 1);
            }
        } else {
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }

    // Null out _c_object
    if (lua_istable(L, 1)) {
        lua_pushstring(L, "_c_object");
        lua_pushnil(L);
        lua_rawset(L, 1);
    }

    // Unref
    if (platoon->lua_table_ref() >= 0) {
        luaL_unref(L, LUA_REGISTRYINDEX, platoon->lua_table_ref());
        platoon->set_lua_table_ref(-2);
    }

    platoon->mark_destroyed();
    return 0;
}

// ====================================================================
// Platoon targeting / threat methods
// ====================================================================

// platoon:FindClosestUnit(squad, allyStatus, isUnit, category)
// Returns closest matching unit Lua table, or nil.
static int platoon_FindClosestUnit(lua_State* L) {
    auto* platoon = check_platoon(L);
    auto* sim = get_sim(L);
    if (!platoon || !sim) { lua_pushnil(L); return 1; }

    // arg 2 = squad (string, currently unused for position — we use platoon center)
    // arg 3 = allyStatus ("Enemy", "Ally", etc.)
    const char* ally_status = lua_isstring(L, 3) ? lua_tostring(L, 3) : "Enemy";
    // arg 4 = isUnit (boolean, ignored — we only have units)
    // arg 5 = category (Lua table)
    int cat_idx = lua_istable(L, 5) ? 5 : 0;

    // Get platoon center
    auto center = platoon->get_position(sim->entity_registry());

    // Find the brain for this platoon's army
    auto* brain = sim->get_army(platoon->army_index());
    if (!brain) { lua_pushnil(L); return 1; }
    i32 my_army = brain->index();

    f32 best_dist = 1e30f;
    sim::Entity* best = nullptr;

    sim->entity_registry().for_each([&](const sim::Entity& e) {
        if (!e.is_unit() || e.destroyed()) return;
        auto* unit = static_cast<const sim::Unit*>(&e);

        // Filter by team relationship
        if (std::strcmp(ally_status, "Enemy") == 0) {
            if (!sim->is_enemy(my_army, unit->army())) return;
        } else if (std::strcmp(ally_status, "Ally") == 0) {
            if (!sim->is_ally(my_army, unit->army()) &&
                unit->army() != my_army) return;
        } else {
            if (unit->army() != my_army) return;
        }

        // Filter by category
        if (cat_idx > 0 &&
            !osc::lua::unit_matches_category(L, cat_idx, unit->categories()))
            return;

        f32 dx = unit->position().x - center.x;
        f32 dz = unit->position().z - center.z;
        f32 dist = dx * dx + dz * dz;
        if (dist < best_dist) {
            best_dist = dist;
            best = const_cast<sim::Entity*>(&e);
        }
    });

    if (best && best->lua_table_ref() >= 0) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, best->lua_table_ref());
    } else {
        lua_pushnil(L);
    }
    return 1;
}

// platoon:FindPrioritizedUnit(squad, allyStatus, isUnit, pos, radius)
// Like FindClosestUnit but centered on given position within radius.
static int platoon_FindPrioritizedUnit(lua_State* L) {
    auto* platoon = check_platoon(L);
    auto* sim = get_sim(L);
    if (!platoon || !sim) { lua_pushnil(L); return 1; }

    const char* ally_status = lua_isstring(L, 3) ? lua_tostring(L, 3) : "Enemy";
    // arg 4 = isUnit (ignored)

    // arg 5 = position table
    f32 px = 0, pz = 0;
    if (lua_istable(L, 5)) {
        lua_rawgeti(L, 5, 1);
        px = static_cast<f32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        lua_rawgeti(L, 5, 3);
        pz = static_cast<f32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
    }

    // arg 6 = radius
    f32 radius = lua_isnumber(L, 6) ? static_cast<f32>(lua_tonumber(L, 6)) : 512.0f;

    auto* brain = sim->get_army(platoon->army_index());
    if (!brain) { lua_pushnil(L); return 1; }
    i32 my_army = brain->index();

    auto ids = sim->entity_registry().collect_in_radius(px, pz, radius);

    f32 best_dist = 1e30f;
    sim::Entity* best = nullptr;

    for (u32 eid : ids) {
        auto* entity = sim->entity_registry().find(eid);
        if (!entity || !entity->is_unit() || entity->destroyed()) continue;
        auto* unit = static_cast<sim::Unit*>(entity);

        if (std::strcmp(ally_status, "Enemy") == 0) {
            if (!sim->is_enemy(my_army, unit->army())) continue;
        } else if (std::strcmp(ally_status, "Ally") == 0) {
            if (!sim->is_ally(my_army, unit->army()) &&
                unit->army() != my_army) continue;
        } else {
            if (unit->army() != my_army) continue;
        }

        f32 dx = unit->position().x - px;
        f32 dz = unit->position().z - pz;
        f32 dist = dx * dx + dz * dz;
        if (dist < best_dist) {
            best_dist = dist;
            best = entity;
        }
    }

    if (best && best->lua_table_ref() >= 0) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, best->lua_table_ref());
    } else {
        lua_pushnil(L);
    }
    return 1;
}

// platoon:IsCommandsActive(cmdId)
// Returns true if any unit in the platoon still has a command with this ID.
static int platoon_IsCommandsActive(lua_State* L) {
    auto* platoon = check_platoon(L);
    auto* sim = get_sim(L);
    if (!platoon || !sim) {
        lua_pushboolean(L, 0);
        return 1;
    }

    u32 cmd_id = static_cast<u32>(lua_tonumber(L, 2));
    if (cmd_id == 0) {
        lua_pushboolean(L, 0);
        return 1;
    }

    for (u32 id : platoon->unit_ids()) {
        auto* e = sim->entity_registry().find(id);
        if (!e || e->destroyed() || !e->is_unit()) continue;
        auto* unit = static_cast<sim::Unit*>(e);

        for (const auto& cmd : unit->command_queue()) {
            if (cmd.command_id == cmd_id) {
                lua_pushboolean(L, 1);
                return 1;
            }
        }
    }

    lua_pushboolean(L, 0);
    return 1;
}

// platoon:CalculatePlatoonThreat(threatType, category)
// Sum threat of all platoon units matching category for given threat type.
static int platoon_CalculatePlatoonThreat(lua_State* L) {
    auto* platoon = check_platoon(L);
    auto* sim = get_sim(L);
    if (!platoon || !sim) {
        lua_pushnumber(L, 0);
        return 1;
    }

    const char* threat_type = lua_isstring(L, 2) ? lua_tostring(L, 2) : "Overall";
    int cat_idx = lua_istable(L, 3) ? 3 : 0;

    f32 total = 0;
    for (u32 id : platoon->unit_ids()) {
        auto* e = sim->entity_registry().find(id);
        if (!e || e->destroyed() || !e->is_unit()) continue;
        auto* unit = static_cast<sim::Unit*>(e);

        if (cat_idx > 0 &&
            !osc::lua::unit_matches_category(L, cat_idx, unit->categories()))
            continue;

        total += get_unit_threat_for_type(unit, threat_type);
    }

    lua_pushnumber(L, total);
    return 1;
}

// clang-format off
static const MethodEntry aibrain_methods[] = {
    // Real implementations
    {"GetArmyIndex",                brain_GetArmyIndex},
    {"GetFactionIndex",             brain_GetFactionIndex},
    {"GetListOfUnits",              brain_GetListOfUnits},
    {"GetUnitsAroundPoint",         brain_GetUnitsAroundPoint},
    {"GetArmyStartPos",             brain_GetArmyStartPos},
    {"GetEconomyIncome",            brain_GetEconomyIncome},
    {"GetEconomyRequested",         brain_GetEconomyRequested},
    {"GetEconomyTrend",             brain_GetEconomyTrend},
    {"GetEconomyStored",            brain_GetEconomyStored},
    {"GetEconomyStoredRatio",       brain_GetEconomyStoredRatio},
    {"GiveResource",                brain_GiveResource},
    {"IsDefeated",                  brain_IsDefeated},
    {"GetCurrentUnits",             brain_GetCurrentUnits},
    {"GetBrainStatus",              brain_GetBrainStatus},
    {"GetAllianceToArmy",           brain_GetAllianceToArmy},
    {"GetEconomyOverTime",          brain_GetEconomyOverTime},
    {"GetArmyStat",                 brain_GetArmyStat},
    {"SetArmyStat",                 brain_SetArmyStat},
    {"GetBlueprintStat",            brain_GetBlueprintStat},
    {"GetUnitBlueprint",            brain_GetUnitBlueprint},
    {"SetArmyStatsTrigger",         stub_noop},
    // AddUnitStat — defined in StatManagerBrainComponent (Lua)
    // AddEnergyDependingEntity — defined in EnergyManagerBrainComponent (Lua)
    {"GetEconomyUsage",             brain_GetEconomyUsage},
    // TrackJammer — defined in JammerManagerBrainComponent (Lua)
    {"RemoveArmyStatsTrigger",      stub_noop},
    {"RemoveEnergyDependingEntity", stub_noop},
    {"GiveStorage",                 stub_noop},
    {"OnUnitStopBeingBuilt",        stub_noop},
    {"OnVictory",                   stub_noop},
    {"OnDefeat",                    stub_noop},
    {"OnDraw",                      stub_noop},
    {"SetArmyColor",                brain_SetArmyColor},
    // Stubs (AI/platoon features not yet implemented)
    {"AssignUnitsToPlatoon",        brain_AssignUnitsToPlatoon},
    {"PlatoonExists",               brain_PlatoonExists},
    {"DisbandPlatoon",              brain_DisbandPlatoon},
    {"SetResourceSharing",          stub_noop},
    {"GetThreatAtPosition",         brain_GetThreatAtPosition},
    {"GetThreatsAroundPosition",    brain_GetThreatsAroundPosition},
    {"IsAnyEngineerBuilding",       stub_return_false},
    {"SetCurrentPlan",              stub_noop},
    {"PBMRemoveBuildLocation",      stub_noop},
    {"PBMAddBuildLocation",         stub_noop},
    {"SetUpAttackVectorsToArmy",    stub_noop},
    {"FindPlaceToBuild",            brain_FindPlaceToBuild},
    {"CanBuildStructureAt",         stub_return_true},
    {"BuildUnit",                   brain_BuildUnit},
    {"BuildStructure",              brain_BuildStructure},
    {"DecideWhatToBuild",           brain_DecideWhatToBuild},
    {"MakePlatoon",                 brain_MakePlatoon},
    {"GetPlatoonUniquelyNamed",     brain_GetPlatoonUniquelyNamed},
    {"GetHighestThreatPosition",    brain_GetHighestThreatPosition},
    {"SetGreaterOf",                stub_noop},
    {"GetArmySkinName",             stub_return_empty_string},
    {"GetCurrentEnemy",             brain_GetCurrentEnemy},
    {"SetCurrentEnemy",             brain_SetCurrentEnemy},
    {"GetNumUnitsAroundPoint",      brain_GetNumUnitsAroundPoint},
    {"GetPlatoonsList",             brain_GetPlatoonsList},
    {nullptr, nullptr},
};
// clang-format on

// Shield-specific methods only — GetHealth, SetHealth, GetMaxHealth, Destroy,
// BeenDestroyed are inherited from entity_methods via the base class chain.
// DO NOT re-declare them here or ClassShield(moho.shield_methods, Entity) will
// error with "field 'X' is ambiguous between the bases" because the flattened
// closures differ from the Entity class's copies.
static const MethodEntry shield_methods[] = {
    {"GetOmni",                     stub_return_false},
    {"TurnOn",                      stub_noop},
    {"TurnOff",                     stub_noop},
    {"IsOn",                        stub_return_false},
    {"GetType",                     stub_return_empty_string},
    {"SetSize",                     stub_noop},
    {nullptr, nullptr},
};

static const MethodEntry blip_methods[] = {
    {"GetSource",           stub_return_nil},
    {"IsOnRadar",           stub_return_false},
    {"IsOnSonar",           stub_return_false},
    {"IsOnOmni",            stub_return_false},
    {"IsSeenEver",          stub_return_false},
    {"IsSeenNow",           stub_return_false},
    {"GetBlueprint",        stub_return_nil},
    {"GetPosition",         entity_GetPosition},
    {"GetArmy",             stub_return_zero},
    {"BeenDestroyed",       stub_return_false},
    {"IsKnownFake",         stub_return_false},
    {"IsMaybeDead",         stub_return_false},
    {nullptr, nullptr},
};

static const MethodEntry platoon_methods[] = {
    {"Destroy",                     platoon_Destroy},
    {"GetPlatoonUnits",             platoon_GetPlatoonUnits},
    {"GetSquadUnits",               platoon_GetSquadUnits},
    {"GetBrain",                    platoon_GetBrain},
    {"UniquelyNamePlatoon",         platoon_UniquelyNamePlatoon},
    {"GetPlatoonPosition",          platoon_GetPlatoonPosition},
    {"ForkThread",                  platoon_ForkThread},
    {"SetAIPlan",                   platoon_SetAIPlan},
    {"GetPlan",                     platoon_GetPlan},
    {"SetPlatoonFormationOverride", stub_noop},
    {"Stop",                        platoon_Stop},
    {"MoveToLocation",              platoon_MoveToLocation},
    {"MoveToTarget",                platoon_MoveToLocation},
    {"Patrol",                      platoon_Patrol},
    {"AggressiveMoveToLocation",    platoon_MoveToLocation},
    {"AttackTarget",                platoon_AttackTarget},
    {"GuardTarget",                 platoon_GuardTarget},
    {"IsOpponentAIRunning",         stub_return_false},
    {"FindClosestUnit",             platoon_FindClosestUnit},
    {"FindPrioritizedUnit",         platoon_FindPrioritizedUnit},
    {"SetPrioritizedTargetList",    stub_noop},
    {"IsCommandsActive",            platoon_IsCommandsActive},
    {"CalculatePlatoonThreat",      platoon_CalculatePlatoonThreat},
    {nullptr, nullptr},
};

// Manipulator base methods
static const MethodEntry manipulator_methods[] = {
    {"Destroy",                 stub_noop},
    {"Enable",                  stub_noop},
    {"Disable",                 stub_noop},
    {"SetEnabled",              stub_noop},
    {"SetPrecedence",           stub_noop},
    {"IsEnabled",               stub_return_false},
    {nullptr, nullptr},
};

static const MethodEntry aim_manipulator_methods[] = {
    {"SetFiringArc",            stub_noop},
    {"SetHeadingPitch",         stub_noop},
    {"GetHeadingPitch",         stub_return_zero},
    {"OnTarget",                stub_return_false},
    {"SetEnabled",              stub_noop},
    {"SetResetPoseTime",        stub_noop},
    {"SetAimHeadingOffset",     stub_noop},
    {nullptr, nullptr},
};

static const MethodEntry animation_manipulator_methods[] = {
    {"PlayAnim",                stub_return_self},
    {"SetRate",                 stub_return_self},
    {"SetAnimationFraction",    stub_noop},
    {"GetAnimationFraction",    stub_return_zero},
    {"GetAnimationDuration",    stub_return_one},
    {"GetAnimationTime",        stub_return_zero},
    {"SetAnimationTime",        stub_noop},
    {"SetBoneEnabled",          stub_noop},
    {nullptr, nullptr},
};

static const MethodEntry rotate_manipulator_methods[] = {
    {"SetGoal",                 stub_return_self},
    {"SetSpeed",                stub_return_self},
    {"SetAccel",                stub_return_self},
    {"SetCurrentAngle",         stub_noop},
    {"GetCurrentAngle",         stub_return_zero},
    {"SetSpinDown",             stub_noop},
    {nullptr, nullptr},
};

static const MethodEntry slide_manipulator_methods[] = {
    {"SetGoal",                 stub_return_self},
    {"SetSpeed",                stub_return_self},
    {"SetAccel",                stub_return_self},
    {"SetWorldUnits",           stub_return_self},
    {nullptr, nullptr},
};

// IEffect — chainable methods return self
static const MethodEntry ieffect_methods[] = {
    {"ScaleEmitter",            stub_return_self},
    {"OffsetEmitter",           stub_return_self},
    {"SetEmitterParam",         stub_return_self},
    {"SetEmitterCurveParam",    stub_return_self},
    {"Destroy",                 stub_noop},
    {"BeenDestroyed",           stub_return_false},
    {nullptr, nullptr},
};

// navigator
static const MethodEntry navigator_methods[] = {
    {"GetCurrentTargetSpeed",   nav_GetCurrentTargetSpeed},
    {"SetSpeedThroughGoal",     stub_noop},
    {"SetGoal",                 nav_SetGoal},
    {"GetGoal",                 nav_GetGoal},
    {"AbortMove",               nav_AbortMove},
    {nullptr, nullptr},
};

// Minimal entries for other classes
static const MethodEntry empty_methods[] = {
    {nullptr, nullptr},
};

static const MethodEntry economy_event_methods[] = {
    {"Destroy",                 stub_noop},
    {nullptr, nullptr},
};

static const MethodEntry decal_handle_methods[] = {
    {"Destroy",                 stub_noop},
    {nullptr, nullptr},
};

static const MethodEntry entity_category_methods[] = {
    {nullptr, nullptr},
};

static const MethodEntry collision_beam_methods[] = {
    {"Enable",                  stub_noop},
    {"Disable",                 stub_noop},
    {"SetBeamFx",               stub_noop},
    {"IsEnabled",               stub_return_false},
    {"Destroy",                 stub_noop},
    {"BeenDestroyed",           stub_return_false},
    {nullptr, nullptr},
};
// clang-format on

// ====================================================================
// Registration
// ====================================================================

struct MohoClassDef {
    const char* name;
    const MethodEntry* methods;
    const char*
        base; // Name of base class (array entry [1]), or nullptr
};

// clang-format off
static const MohoClassDef moho_classes[] = {
    // Base classes (no inheritance)
    {"entity_methods",          entity_methods,          nullptr},
    {"manipulator_methods",     manipulator_methods,     nullptr},
    {"weapon_methods",          weapon_methods,          nullptr},
    {"blip_methods",            blip_methods,            nullptr},
    {"aibrain_methods",         aibrain_methods,         nullptr},
    {"platoon_methods",         platoon_methods,         nullptr},
    {"navigator_methods",       navigator_methods,       nullptr},
    {"aipersonality_methods",   empty_methods,           nullptr},
    {"CAiAttackerImpl_methods", empty_methods,           nullptr},
    {"ScriptTask_Methods",      empty_methods,           nullptr},
    {"sound_methods",           empty_methods,           nullptr},
    {"CDamage",                 empty_methods,           nullptr},
    {"CDecalHandle",            decal_handle_methods,    nullptr},
    {"EconomyEvent",            economy_event_methods,   nullptr},
    {"EntityCategory",          entity_category_methods, nullptr},
    {"CPrefetchSet",            empty_methods,           nullptr},
    {"MotorFallDown",           empty_methods,           nullptr},
    {"PathDebugger_methods",    empty_methods,           nullptr},

    // Inherit from entity_methods
    {"unit_methods",            unit_methods,           "entity_methods"},
    {"projectile_methods",      projectile_methods,     "entity_methods"},
    {"prop_methods",            prop_methods,           "entity_methods"},
    {"shield_methods",          shield_methods,         "entity_methods"},
    {"CollisionBeamEntity",     collision_beam_methods, "entity_methods"},

    // IEffect (no base)
    {"IEffect",                 ieffect_methods,         nullptr},

    // Manipulators (inherit from manipulator_methods)
    {"AimManipulator",          aim_manipulator_methods,        "manipulator_methods"},
    {"AnimationManipulator",    animation_manipulator_methods,  "manipulator_methods"},
    {"BuilderArmManipulator",   empty_methods,                  "manipulator_methods"},
    {"RotateManipulator",       rotate_manipulator_methods,     "manipulator_methods"},
    {"SlideManipulator",        slide_manipulator_methods,      "manipulator_methods"},
    {"SlaveManipulator",        empty_methods,                  "manipulator_methods"},
    {"ThrustManipulator",       empty_methods,                  "manipulator_methods"},
    {"BoneEntityManipulator",   empty_methods,                  "manipulator_methods"},
    {"StorageManipulator",      empty_methods,                  "manipulator_methods"},
    {"FootPlantManipulator",    empty_methods,                  "manipulator_methods"},
    {"CollisionManipulator",    empty_methods,                  "manipulator_methods"},

    // UI classes — empty stubs (sim doesn't use them, but globalInit iterates all)
    {"bitmap_methods",          empty_methods,  nullptr},
    {"border_methods",          empty_methods,  nullptr},
    {"control_methods",         empty_methods,  nullptr},
    {"cursor_methods",          empty_methods,  nullptr},
    {"discovery_service_methods", empty_methods, nullptr},
    {"dragger_methods",         empty_methods,  nullptr},
    {"edit_methods",            empty_methods,  nullptr},
    {"frame_methods",           empty_methods,  nullptr},
    {"group_methods",           empty_methods,  nullptr},
    {"histogram_methods",       empty_methods,  nullptr},
    {"item_list_methods",       empty_methods,  nullptr},
    {"lobby_methods",           empty_methods,  nullptr},
    {"mesh_methods",            empty_methods,  nullptr},
    {"movie_methods",           empty_methods,  nullptr},
    {"scrollbar_methods",       empty_methods,  nullptr},
    {"text_methods",            empty_methods,  nullptr},
    {"UIWorldView",             empty_methods,  nullptr},
    {"userDecal_methods",       empty_methods,  nullptr},
    {"WldUIProvider_methods",   empty_methods,  nullptr},
    {"world_mesh_methods",      empty_methods,  nullptr},

    {nullptr, nullptr, nullptr}, // sentinel
};
// clang-format on

void register_moho_bindings(LuaState& state, sim::SimState& sim) {
    lua_State* L = state.raw();

    // Store sim state pointer in registry
    lua_pushstring(L, "osc_sim_state");
    lua_pushlightuserdata(L, &sim);
    lua_rawset(L, LUA_REGISTRYINDEX);

    // Create the moho global table
    lua_newtable(L);

    // First pass: register all base classes (no inheritance)
    for (const auto* def = moho_classes; def->name != nullptr; def++) {
        if (def->base != nullptr) continue; // skip derived classes in first pass

        lua_pushstring(L, def->name);
        lua_newtable(L);

        for (const auto* m = def->methods; m->name != nullptr; m++) {
            lua_pushstring(L, m->name);
            lua_pushcfunction(L, m->func);
            lua_rawset(L, -3);
        }

        lua_rawset(L, -3); // moho[name] = class_table
    }

    // Second pass: register derived classes (with base reference as [1])
    for (const auto* def = moho_classes; def->name != nullptr; def++) {
        if (def->base == nullptr) continue; // skip base classes

        lua_pushstring(L, def->name);
        lua_newtable(L);

        // Set [1] = moho[base_name] for Flatten to find
        lua_pushstring(L, def->base);
        lua_rawget(L, -4); // get moho[base_name]
        lua_rawseti(L, -2, 1);

        // Add this class's own methods
        for (const auto* m = def->methods; m->name != nullptr; m++) {
            lua_pushstring(L, m->name);
            lua_pushcfunction(L, m->func);
            lua_rawset(L, -3);
        }

        lua_rawset(L, -3); // moho[name] = class_table
    }

    lua_setglobal(L, "moho"); // set the moho global

    spdlog::info("Registered moho bindings");
}

} // namespace osc::lua
