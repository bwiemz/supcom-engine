#include "lua/moho_bindings.hpp"
#include "lua/category_utils.hpp"
#include "lua/lua_state.hpp"
#include "sim/army_brain.hpp"
#include "sim/bone_data.hpp"
#include "sim/entity.hpp"
#include "sim/entity_registry.hpp"
#include "sim/manipulator.hpp"
#include "sim/sim_state.hpp"
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

#include <cmath>
#include <cstring>
#include <vector>
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
    if (!lua_istable(L, idx)) return nullptr;
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
    if (!lua_istable(L, idx)) return nullptr;
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

static sim::Shield* check_shield(lua_State* L, int idx = 1) {
    auto* e = check_entity(L, idx);
    if (e && e->is_shield())
        return static_cast<sim::Shield*>(e);
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

static audio::SoundManager* get_sound_mgr(lua_State* L) {
    lua_pushstring(L, "osc_sound_manager");
    lua_rawget(L, LUA_REGISTRYINDEX);
    auto* mgr = static_cast<audio::SoundManager*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return mgr;
}

/// Extract Bank and Cue strings from a sound table at the given stack index.
/// Returns false if the table is missing or lacks Bank/Cue keys.
static bool extract_sound_table(lua_State* L, int idx,
                                std::string& bank, std::string& cue) {
    if (!lua_istable(L, idx)) return false;

    lua_pushstring(L, "Bank");
    lua_rawget(L, idx);
    if (lua_type(L, -1) != LUA_TSTRING) { lua_pop(L, 1); return false; }
    bank = lua_tostring(L, -1);
    lua_pop(L, 1);

    lua_pushstring(L, "Cue");
    lua_rawget(L, idx);
    if (lua_type(L, -1) != LUA_TSTRING) { lua_pop(L, 1); return false; }
    cue = lua_tostring(L, -1);
    lua_pop(L, 1);

    return true;
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
// Sound methods
// ====================================================================

/// entity:PlaySound(soundTable) — play one-shot at entity position
static int entity_PlaySound(lua_State* L) {
    auto* mgr = get_sound_mgr(L);
    if (!mgr) return 0;

    auto* e = check_entity(L);
    if (!e || e->destroyed()) return 0;

    std::string bank, cue;
    if (!extract_sound_table(L, 2, bank, cue)) return 0;

    auto pos = e->position();
    mgr->play(bank, cue, &pos);
    return 0;
}

/// entity:SetAmbientSound(soundTable, nil) — start/stop looping ambient
/// SetAmbientSound(nil, nil) stops the current ambient sound.
static int entity_SetAmbientSound(lua_State* L) {
    auto* mgr = get_sound_mgr(L);
    if (!mgr) return 0;

    auto* e = check_entity(L);
    if (!e || e->destroyed()) return 0;

    // Stop any existing ambient loop
    if (e->ambient_sound_handle() != 0) {
        mgr->stop(e->ambient_sound_handle());
        e->set_ambient_sound_handle(0);
    }

    // If arg 2 is a sound table, start a new loop
    std::string bank, cue;
    if (extract_sound_table(L, 2, bank, cue)) {
        auto pos = e->position();
        auto handle = mgr->play_loop(bank, cue, &pos);
        e->set_ambient_sound_handle(handle);
    }

    return 0;
}

/// weapon:PlaySound(soundTable) — play one-shot at owning unit position
static int weapon_PlaySound(lua_State* L) {
    auto* mgr = get_sound_mgr(L);
    if (!mgr) return 0;

    auto* unit = check_weapon_unit(L);
    if (!unit || unit->destroyed()) return 0;

    std::string bank, cue;
    if (!extract_sound_table(L, 2, bank, cue)) return 0;

    auto pos = unit->position();
    mgr->play(bank, cue, &pos);
    return 0;
}

// --- Bone helper functions ---

/// Resolve bone argument (string name or integer index) → bone index.
/// Returns 0 (root) if not found.
static i32 resolve_bone_index(const sim::Entity* e, lua_State* L, int arg) {
    auto* bd = e->bone_data();
    if (!bd) return 0; // fallback root

    if (lua_type(L, arg) == LUA_TSTRING) {
        std::string name = lua_tostring(L, arg);
        i32 idx = bd->find_bone(name);
        return (idx >= 0) ? idx : 0; // fallback to root
    }
    if (lua_type(L, arg) == LUA_TNUMBER) {
        i32 idx = static_cast<i32>(lua_tonumber(L, arg));
        return bd->is_valid(idx) ? idx : 0;
    }
    return 0; // default to root
}

/// Compute world-space bone position for an entity.
static sim::Vector3 bone_world_position(const sim::Entity* e, i32 bone_idx) {
    auto* bd = e->bone_data();
    if (!bd || !bd->is_valid(bone_idx)) return e->position();

    auto& bone = bd->bones[static_cast<size_t>(bone_idx)];
    auto rotated = sim::quat_rotate(e->orientation(), bone.world_position);
    return {
        e->position().x + rotated.x,
        e->position().y + rotated.y,
        e->position().z + rotated.z
    };
}

// --- Bone query functions ---

static int entity_GetBoneCount(lua_State* L) {
    auto* e = check_entity(L);
    if (!e) { lua_pushnumber(L, 1); return 1; }
    auto* bd = e->bone_data();
    lua_pushnumber(L, bd ? bd->bone_count() : 1);
    return 1;
}

static int entity_GetBoneName(lua_State* L) {
    auto* e = check_entity(L);
    if (!e) { lua_pushstring(L, "root"); return 1; }
    auto* bd = e->bone_data();
    if (!bd) { lua_pushstring(L, "root"); return 1; }
    i32 idx = (lua_type(L, 2) == LUA_TNUMBER) ? static_cast<i32>(lua_tonumber(L, 2)) : 0;
    if (bd->is_valid(idx))
        lua_pushstring(L, bd->bones[static_cast<size_t>(idx)].name.c_str());
    else
        lua_pushstring(L, "root");
    return 1;
}

static int entity_IsValidBone(lua_State* L) {
    auto* e = check_entity(L);
    if (!e) { lua_pushboolean(L, 0); return 1; }
    auto* bd = e->bone_data();
    if (!bd) { lua_pushboolean(L, 0); return 1; }

    if (lua_type(L, 2) == LUA_TSTRING) {
        std::string name = lua_tostring(L, 2);
        lua_pushboolean(L, bd->find_bone(name) >= 0 ? 1 : 0);
    } else if (lua_type(L, 2) == LUA_TNUMBER) {
        i32 idx = static_cast<i32>(lua_tonumber(L, 2));
        lua_pushboolean(L, bd->is_valid(idx) ? 1 : 0);
    } else {
        lua_pushboolean(L, 0);
    }
    return 1;
}

static int entity_GetBoneDirection(lua_State* L) {
    auto* e = check_entity(L);
    if (!e) { push_vector3(L, {0, 0, 1}); return 1; }
    auto* bd = e->bone_data();
    if (!bd || lua_gettop(L) < 2) {
        // No bone data or no bone arg: return entity forward direction
        auto fwd = sim::quat_rotate(e->orientation(), {0, 0, 1});
        push_vector3(L, fwd);
        return 1;
    }
    i32 idx = resolve_bone_index(e, L, 2);
    auto& bone = bd->bones[static_cast<size_t>(idx)];
    auto bone_world_rot = sim::quat_multiply(e->orientation(), bone.local_rotation);
    auto dir = sim::quat_rotate(bone_world_rot, {0, 0, 1});
    push_vector3(L, dir);
    return 1;
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
    // Optional bone argument (arg 2): name or index
    if (lua_gettop(L) >= 2 && e->bone_data() &&
        (lua_type(L, 2) == LUA_TSTRING || lua_type(L, 2) == LUA_TNUMBER)) {
        i32 idx = resolve_bone_index(e, L, 2);
        push_vector3(L, bone_world_position(e, idx));
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

// entity:GetHeading() — extract yaw (Y-axis rotation) from quaternion
static int entity_GetHeading(lua_State* L) {
    auto* e = check_entity(L);
    if (!e) { lua_pushnumber(L, 0); return 1; }
    const auto& q = e->orientation();
    f32 heading = std::atan2(
        2.0f * (q.w * q.y + q.x * q.z),
        1.0f - 2.0f * (q.y * q.y + q.z * q.z));
    lua_pushnumber(L, heading);
    return 1;
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

    store->push_lua_table(*entry, L);
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
        // can_be_killed guard — check both C++ field and Lua field
        // (FA's SetCanBeKilled sets a Lua field, not the C++ field)
        if (e->is_unit()) {
            auto* u = static_cast<sim::Unit*>(e);
            if (!u->can_be_killed()) return 0;
            // Also check Lua-side CanBeKilled field
            lua_pushstring(L, "CanBeKilled");
            lua_rawget(L, 1);
            if (lua_isboolean(L, -1) && !lua_toboolean(L, -1)) {
                lua_pop(L, 1);
                return 0;
            }
            lua_pop(L, 1);
        }
        // Stop ambient sound before destruction
        if (e->ambient_sound_handle() != 0) {
            auto* mgr = get_sound_mgr(L);
            if (mgr) mgr->stop(e->ambient_sound_handle());
            e->set_ambient_sound_handle(0);
        }

        // Fire OnNotAdjacentTo for adjacent structures before destruction
        if (e->is_unit()) {
            auto* unit = static_cast<sim::Unit*>(e);
            if (!unit->adjacent_unit_ids().empty()) {
                auto* sim = get_sim(L);
                std::vector<u32> adj_snap(unit->adjacent_unit_ids().begin(),
                                          unit->adjacent_unit_ids().end());
                for (u32 adj_id : adj_snap) {
                    auto* adj_e = sim ? sim->entity_registry().find(adj_id) : nullptr;
                    if (!adj_e || adj_e->destroyed() || !adj_e->is_unit()) continue;
                    auto* adj_u = static_cast<sim::Unit*>(adj_e);
                    adj_u->remove_adjacent(e->entity_id());

                    // Fire OnNotAdjacentTo(self, neighbor) on dying unit
                    if (e->lua_table_ref() >= 0 && adj_e->lua_table_ref() >= 0) {
                        lua_rawgeti(L, LUA_REGISTRYINDEX, e->lua_table_ref());
                        int self_tbl = lua_gettop(L);
                        lua_pushstring(L, "OnNotAdjacentTo");
                        lua_gettable(L, self_tbl);
                        if (lua_isfunction(L, -1)) {
                            lua_pushvalue(L, self_tbl);
                            lua_rawgeti(L, LUA_REGISTRYINDEX, adj_e->lua_table_ref());
                            if (lua_pcall(L, 2, 0, 0) != 0) {
                                spdlog::warn("OnNotAdjacentTo(self) error: {}",
                                             lua_tostring(L, -1));
                                lua_pop(L, 1);
                            }
                        } else {
                            lua_pop(L, 1);
                        }
                        lua_pop(L, 1);
                    }

                    // Re-validate dying entity after pcall (could be recursively destroyed)
                    if (e->destroyed()) break;

                    // Re-validate neighbor after pcall
                    adj_e = sim ? sim->entity_registry().find(adj_id) : nullptr;
                    if (!adj_e || adj_e->destroyed()) continue;

                    // Fire OnNotAdjacentTo(neighbor, self) on neighbor
                    if (adj_e->lua_table_ref() >= 0 && e->lua_table_ref() >= 0) {
                        lua_rawgeti(L, LUA_REGISTRYINDEX, adj_e->lua_table_ref());
                        int nb_tbl = lua_gettop(L);
                        lua_pushstring(L, "OnNotAdjacentTo");
                        lua_gettable(L, nb_tbl);
                        if (lua_isfunction(L, -1)) {
                            lua_pushvalue(L, nb_tbl);
                            lua_rawgeti(L, LUA_REGISTRYINDEX, e->lua_table_ref());
                            if (lua_pcall(L, 2, 0, 0) != 0) {
                                spdlog::warn("OnNotAdjacentTo(neighbor) error: {}",
                                             lua_tostring(L, -1));
                                lua_pop(L, 1);
                            }
                        } else {
                            lua_pop(L, 1);
                        }
                        lua_pop(L, 1);
                    }
                }
                unit->clear_adjacents();
            }
        }

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
        else if (std::strcmp(state, "Paused") == 0)
            result = u->is_paused();
        else if (std::strcmp(state, "Attached") == 0)
            result = u->is_loaded();
        else if (std::strcmp(state, "TransportLoading") == 0)
            result = u->has_unit_state("TransportLoading");
        else if (std::strcmp(state, "TransportUnloading") == 0)
            result = u->has_unit_state("TransportUnloading");
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

// GetParent(): returns transport if loaded, otherwise self
static int unit_GetParent(lua_State* L) {
    auto* u = check_unit(L);
    if (!u || u->lua_table_ref() < 0) {
        lua_pushnil(L);
        return 1;
    }
    if (u->transport_id() != 0) {
        auto* sim = get_sim(L);
        if (sim) {
            auto* transport = sim->entity_registry().find(u->transport_id());
            if (transport && !transport->destroyed() &&
                transport->lua_table_ref() >= 0) {
                lua_rawgeti(L, LUA_REGISTRYINDEX, transport->lua_table_ref());
                return 1;
            }
        }
    }
    // Not loaded — return self
    lua_rawgeti(L, LUA_REGISTRYINDEX, u->lua_table_ref());
    return 1;
}

static int unit_GetCargo(lua_State* L) {
    auto* u = check_unit(L);
    auto* sim = get_sim(L);
    lua_newtable(L);
    if (!u || !sim) return 1;
    int idx = 1;
    for (u32 cargo_id : u->cargo_ids()) {
        auto* cargo = sim->entity_registry().find(cargo_id);
        if (cargo && !cargo->destroyed() && cargo->lua_table_ref() >= 0) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, cargo->lua_table_ref());
            lua_rawseti(L, -2, idx++);
        }
    }
    return 1;
}

static int unit_TransportHasSpaceFor(lua_State* L) {
    auto* u = check_unit(L);
    if (!u) { lua_pushboolean(L, 0); return 1; }
    // Simplified: check cargo count vs transport_capacity
    // (Full FA slot math with TransportClass can be added later)
    bool has_space = u->transport_capacity() > 0 &&
                     static_cast<i32>(u->cargo_ids().size()) < u->transport_capacity();
    lua_pushboolean(L, has_space ? 1 : 0);
    return 1;
}

static int unit_AddUnitToStorage(lua_State* L) {
    auto* u = check_unit(L);
    auto* cargo_entity = check_entity(L, 2);
    auto* sim = get_sim(L);
    if (!u || !cargo_entity || !cargo_entity->is_unit() || !sim) return 0;
    auto* cargo = static_cast<sim::Unit*>(cargo_entity);
    // Route through attach_to_transport for consistent behavior
    cargo->attach_to_transport(u, sim->entity_registry(), L);
    return 0;
}

static int unit_TransportDetachAllUnits(lua_State* L) {
    auto* u = check_unit(L);
    auto* sim = get_sim(L);
    if (!u || !sim) return 0;
    bool destroy = lua_toboolean(L, 2) != 0;
    if (destroy) {
        // Kill all cargo via Lua Kill pipeline (same as FA's KillCargo)
        std::vector<u32> snapshot = u->cargo_ids();
        u->clear_cargo();
        for (u32 id : snapshot) {
            auto* cargo = sim->entity_registry().find(id);
            if (!cargo || cargo->destroyed()) continue;
            if (cargo->is_unit())
                static_cast<sim::Unit*>(cargo)->set_transport_id(0);
            // Use Lua Kill for proper death pipeline (OnKilled → DeathThread)
            if (cargo->lua_table_ref() >= 0) {
                lua_rawgeti(L, LUA_REGISTRYINDEX, cargo->lua_table_ref());
                lua_pushstring(L, "Kill");
                lua_gettable(L, -2);
                if (lua_isfunction(L, -1)) {
                    lua_pushvalue(L, -2); // self
                    if (lua_pcall(L, 1, 0, 0) != 0) {
                        spdlog::warn("TransportDetachAllUnits Kill error: {}",
                                     lua_tostring(L, -1));
                        lua_pop(L, 1);
                    }
                } else {
                    lua_pop(L, 1); // non-function
                }
                lua_pop(L, 1); // cargo table
            } else {
                cargo->mark_destroyed(); // fallback if no Lua table
            }
        }
    } else {
        u->detach_all_cargo(sim->entity_registry(), L);
    }
    return 0;
}

static int unit_SetSpeedMult(lua_State* L) {
    auto* u = check_unit(L);
    if (!u) return 0;
    f32 mult = static_cast<f32>(luaL_checknumber(L, 2));
    u->set_speed_mult(mult);
    return 0;
}

// ====================================================================
// Method table definitions
// ====================================================================

struct MethodEntry {
    const char* name;
    lua_CFunction func;
};

// entity:CreateProjectile(bp, dx, dy, dz) -> projectile Lua table
// Creates a projectile at entity position with optional velocity direction
static int entity_CreateProjectile(lua_State* L) {
    auto* e = check_entity(L);
    if (!e || e->destroyed()) { lua_pushnil(L); return 1; }

    auto* sim = get_sim(L);
    if (!sim) { lua_pushnil(L); return 1; }

    auto proj = std::make_unique<sim::Projectile>();
    proj->set_position(e->position());
    proj->set_army(e->army());
    proj->launcher_id = e->entity_id();
    proj->lifetime = 10.0f;

    // Optional velocity direction (args 3,4,5 if bp is string; args 2,3,4 if no bp)
    int vel_start = 2;
    if (lua_type(L, 2) == LUA_TSTRING) {
        vel_start = 3; // bp string is arg 2, velocity starts at 3
    }
    if (lua_isnumber(L, vel_start) && lua_isnumber(L, vel_start + 1) &&
        lua_isnumber(L, vel_start + 2)) {
        proj->velocity.x = static_cast<f32>(lua_tonumber(L, vel_start));
        proj->velocity.y = static_cast<f32>(lua_tonumber(L, vel_start + 1));
        proj->velocity.z = static_cast<f32>(lua_tonumber(L, vel_start + 2));
    }

    u32 proj_id = sim->entity_registry().register_entity(std::move(proj));
    auto* proj_ptr = static_cast<sim::Projectile*>(
        sim->entity_registry().find(proj_id));

    // Create Lua table with projectile metatable (reuse __osc_proj_mt)
    lua_newtable(L);
    lua_pushstring(L, "_c_object");
    lua_pushlightuserdata(L, proj_ptr);
    lua_rawset(L, -3);

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

// entity:CreateProjectileAtBone(bone, bp, dx, dy, dz) -> projectile Lua table
static int entity_CreateProjectileAtBone(lua_State* L) {
    auto* e = check_entity(L);
    if (!e || e->destroyed()) { lua_pushnil(L); return 1; }

    auto* sim = get_sim(L);
    if (!sim) { lua_pushnil(L); return 1; }

    // arg 2 = bone (name or index)
    i32 bone_idx = resolve_bone_index(e, L, 2);
    auto spawn_pos = bone_world_position(e, bone_idx);

    auto proj = std::make_unique<sim::Projectile>();
    proj->set_position(spawn_pos);
    proj->set_army(e->army());
    proj->launcher_id = e->entity_id();
    proj->lifetime = 10.0f;

    // arg 3 = bp (optional string), args 4,5,6 = velocity (or 3,4,5 if no bp)
    int vel_start = 3;
    if (lua_type(L, 3) == LUA_TSTRING) {
        vel_start = 4; // bp string at arg 3
    }
    if (lua_isnumber(L, vel_start) && lua_isnumber(L, vel_start + 1) &&
        lua_isnumber(L, vel_start + 2)) {
        proj->velocity.x = static_cast<f32>(lua_tonumber(L, vel_start));
        proj->velocity.y = static_cast<f32>(lua_tonumber(L, vel_start + 1));
        proj->velocity.z = static_cast<f32>(lua_tonumber(L, vel_start + 2));
    }

    u32 proj_id = sim->entity_registry().register_entity(std::move(proj));
    auto* proj_ptr = static_cast<sim::Projectile*>(
        sim->entity_registry().find(proj_id));

    lua_newtable(L);
    lua_pushstring(L, "_c_object");
    lua_pushlightuserdata(L, proj_ptr);
    lua_rawset(L, -3);

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

// ShowBone(self, bone, recurse?)
static int unit_ShowBone(lua_State* L) {
    auto* u = check_unit(L);
    if (!u) return 0;
    i32 idx = resolve_bone_index(u, L, 2);
    u->show_bone(idx);
    // Optional arg 3: recurse to full subtree (not just direct children)
    if (lua_toboolean(L, 3) && u->bone_data()) {
        auto* bd = u->bone_data();
        std::vector<i32> to_process = {idx};
        while (!to_process.empty()) {
            i32 parent = to_process.back();
            to_process.pop_back();
            for (i32 i = 0; i < bd->bone_count(); i++) {
                if (bd->bones[static_cast<size_t>(i)].parent_index == parent) {
                    u->show_bone(i);
                    to_process.push_back(i);
                }
            }
        }
    }
    return 0;
}

// HideBone(self, bone, recurse?)
static int unit_HideBone(lua_State* L) {
    auto* u = check_unit(L);
    if (!u) return 0;
    i32 idx = resolve_bone_index(u, L, 2);
    u->hide_bone(idx);
    // Optional arg 3: recurse to full subtree (not just direct children)
    if (lua_toboolean(L, 3) && u->bone_data()) {
        auto* bd = u->bone_data();
        std::vector<i32> to_process = {idx};
        while (!to_process.empty()) {
            i32 parent = to_process.back();
            to_process.pop_back();
            for (i32 i = 0; i < bd->bone_count(); i++) {
                if (bd->bones[static_cast<size_t>(i)].parent_index == parent) {
                    u->hide_bone(i);
                    to_process.push_back(i);
                }
            }
        }
    }
    return 0;
}

static int entity_SetCustomName(lua_State* L) {
    auto* e = check_entity(L);
    if (e && lua_type(L, 2) == LUA_TSTRING)
        e->set_custom_name(lua_tostring(L, 2));
    return 0;
}

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
    {"IsValidBone",         entity_IsValidBone},
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
    {"GetBoneDirection",        entity_GetBoneDirection},
    {"CreateProjectile",        entity_CreateProjectile},
    {"CreateProjectileAtBone",  entity_CreateProjectileAtBone},
    {"PlaySound",               entity_PlaySound},
    {"SetFractionComplete",     entity_SetFractionComplete},
    {"AddManualScroller",       stub_noop},
    {"AddPingPongScroller",     stub_noop},
    {"AddThreadScroller",       stub_noop},
    {"RemoveScroller",          stub_noop},
    {"RequestRefreshUI",        stub_noop},
    {"SetCustomName",           entity_SetCustomName},
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
// Returns nil if the entity has never been seen by the requesting army.
/// Helper: set cached __osc_blip_mt metatable on a blip table at stack top.
static void set_blip_metatable(lua_State* L, int blip_tbl) {
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
}

/// Helper: build a blip table with _c_object, _c_entity_id, _c_req_army.
static void push_blip_table(lua_State* L, sim::Entity* e, osc::u32 entity_id,
                             osc::i32 req_army) {
    lua_newtable(L);
    int blip_tbl = lua_gettop(L);

    if (e) {
        lua_pushstring(L, "_c_object");
        lua_pushlightuserdata(L, e);
        lua_rawset(L, blip_tbl);
    }
    lua_pushstring(L, "_c_entity_id");
    lua_pushnumber(L, static_cast<lua_Number>(entity_id));
    lua_rawset(L, blip_tbl);
    lua_pushstring(L, "_c_req_army");
    lua_pushnumber(L, static_cast<lua_Number>(req_army));
    lua_rawset(L, blip_tbl);

    set_blip_metatable(L, blip_tbl);
}

static int unit_GetBlip(lua_State* L) {
    auto* e = check_entity(L);

    // Get requesting army (1-based Lua → 0-based C++)
    int req_army = -1;
    if (lua_isnumber(L, 2))
        req_army = static_cast<int>(lua_tonumber(L, 2)) - 1;

    // --- Case 1: entity alive and not destroyed ---
    if (e && !e->destroyed()) {
        // Own army always gets blip
        if (req_army < 0 || req_army == e->army()) {
            push_blip_table(L, e, e->entity_id(), req_army);
            return 1;
        }

        auto* sim = get_sim(L);
        auto ra = static_cast<osc::u32>(req_army);

        // Check if army has any current intel or blip cache entry
        if (sim && sim->visibility_grid()) {
            bool has_intel = sim->has_any_intel(e, ra);
            // Blip cache entry means this entity was previously visible
            // (dead-reckoning — blip methods return cached position)
            bool has_cache = sim->get_blip_snapshot(e->entity_id(), ra) != nullptr;

            if (has_intel || has_cache) {
                // Return blip — blip methods handle dead-reckoning position
                push_blip_table(L, e, e->entity_id(), req_army);
                return 1;
            }
        }

        lua_pushnil(L);
        return 1;
    }

    // --- Case 2: entity destroyed — check blip cache for dead-reckoning ---
    if (req_army < 0) { lua_pushnil(L); return 1; }

    // Read EntityId from the Lua table (arg 1) since C++ entity is gone
    osc::u32 entity_id = 0;
    if (lua_istable(L, 1)) {
        lua_pushstring(L, "EntityId");
        lua_rawget(L, 1);
        if (lua_isnumber(L, -1))
            entity_id = static_cast<osc::u32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
    }
    if (entity_id == 0) { lua_pushnil(L); return 1; }

    auto* sim = get_sim(L);
    if (!sim) { lua_pushnil(L); return 1; }

    auto* snap = sim->get_blip_snapshot(entity_id,
                                         static_cast<osc::u32>(req_army));
    if (!snap) { lua_pushnil(L); return 1; }

    // Return dead-reckoning blip (no live entity pointer)
    push_blip_table(L, nullptr, entity_id, req_army);
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

// GetResourceConsumed(self) → number
// Returns min(mass_efficiency, energy_efficiency) for the unit's army.
// FA Lua does arithmetic on this value (e.g. `obtained * SecondsPerTick()`).
static int unit_GetResourceConsumed(lua_State* L) {
    auto* u = check_unit(L);
    if (!u || u->army() < 0) {
        lua_pushnumber(L, 1.0);
        return 1;
    }
    auto* sim = get_sim(L);
    if (!sim) {
        lua_pushnumber(L, 1.0);
        return 1;
    }
    auto* brain = sim->get_army(u->army());
    if (!brain) {
        lua_pushnumber(L, 1.0);
        return 1;
    }
    f64 eff = std::min(brain->mass_efficiency(), brain->energy_efficiency());
    lua_pushnumber(L, eff);
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

// Shield ratio — real implementations (UpdateShieldRatio calls SetShieldRatio)
static int unit_GetShieldRatio(lua_State* L) {
    auto* u = check_unit(L);
    lua_pushnumber(L, u ? u->shield_ratio() : 1.0);
    return 1;
}
static int unit_SetShieldRatio(lua_State* L) {
    auto* u = check_unit(L);
    if (u) {
        f32 ratio = static_cast<f32>(luaL_checknumber(L, 2));
        u->set_shield_ratio(ratio);
    }
    return 0;
}

// unit:Stop() — clear command queue
static int unit_Stop(lua_State* L) {
    auto* u = check_unit(L);
    if (u) u->clear_commands();
    return 0;
}

// unit:SetPaused(bool) — set/clear pause flag + economy
static int unit_SetPaused(lua_State* L) {
    auto* u = check_unit(L);
    if (u) {
        bool paused = lua_toboolean(L, 2) != 0;
        u->set_paused(paused);
        // When pausing, zero economy activity (FA Lua re-enables on unpause)
        if (paused) {
            u->economy().production_active = false;
            u->economy().consumption_active = false;
        }
    }
    return 0;
}

// unit:IsPaused() -> bool
static int unit_IsPaused(lua_State* L) {
    auto* u = check_unit(L);
    lua_pushboolean(L, u && u->is_paused() ? 1 : 0);
    return 1;
}

// unit:CanBuild(bp_id) -> bool — minimal: builders return true
static int unit_CanBuild(lua_State* L) {
    auto* u = check_unit(L);
    if (!u) { lua_pushboolean(L, 0); return 1; }
    bool is_builder = u->has_category("ENGINEER") ||
                      u->has_category("FACTORY") ||
                      u->has_category("CONSTRUCTION") ||
                      u->has_category("COMMAND");
    lua_pushboolean(L, is_builder ? 1 : 0);
    return 1;
}

// unit:EnableShield() — enable shield on this unit's shield entity
static int unit_EnableShield(lua_State* L) {
    auto* u = check_unit(L);
    if (!u || u->shield_entity_id() == 0) return 0;
    auto* sim = get_sim(L);
    if (!sim) return 0;
    auto* e = sim->entity_registry().find(u->shield_entity_id());
    if (e && e->is_shield()) {
        static_cast<sim::Shield*>(e)->is_on = true;
    }
    return 0;
}

// unit:DisableShield() — disable shield on this unit's shield entity
static int unit_DisableShield(lua_State* L) {
    auto* u = check_unit(L);
    if (!u || u->shield_entity_id() == 0) return 0;
    auto* sim = get_sim(L);
    if (!sim) return 0;
    auto* e = sim->entity_registry().find(u->shield_entity_id());
    if (e && e->is_shield()) {
        static_cast<sim::Shield*>(e)->is_on = false;
    }
    return 0;
}

// unit:ShieldIsOn() -> bool
static int unit_ShieldIsOn(lua_State* L) {
    auto* u = check_unit(L);
    if (!u || u->shield_entity_id() == 0) {
        lua_pushboolean(L, 0);
        return 1;
    }
    auto* sim = get_sim(L);
    if (!sim) { lua_pushboolean(L, 0); return 1; }
    auto* e = sim->entity_registry().find(u->shield_entity_id());
    if (e && e->is_shield()) {
        lua_pushboolean(L, static_cast<sim::Shield*>(e)->is_on ? 1 : 0);
    } else {
        lua_pushboolean(L, 0);
    }
    return 1;
}

// unit:CanPathTo(destPos) -> bool
// Uses A* pathfinder to check if a path exists from unit to destination.
static int unit_CanPathTo(lua_State* L) {
    auto* unit = check_unit(L);
    auto* sim = get_sim(L);
    if (!unit || !sim || !sim->pathfinder()) {
        lua_pushboolean(L, 1); // fallback: allow
        return 1;
    }
    f32 dx = 0, dz = 0;
    if (lua_istable(L, 2)) {
        lua_rawgeti(L, 2, 1);
        dx = static_cast<f32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        lua_rawgeti(L, 2, 3);
        dz = static_cast<f32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
    }
    auto result = sim->pathfinder()->find_path(
        unit->position().x, unit->position().z,
        dx, dz, unit->layer());
    lua_pushboolean(L, result.found ? 1 : 0);
    return 1;
}

// unit:CanPathToCell(destPos) -> bool
// Similar to CanPathTo but intended to be more lenient (cell-level check).
// Currently uses the same A* approach.
static int unit_CanPathToCell(lua_State* L) {
    auto* unit = check_unit(L);
    auto* sim = get_sim(L);
    if (!unit || !sim || !sim->pathfinder()) {
        lua_pushboolean(L, 1);
        return 1;
    }
    f32 dx = 0, dz = 0;
    if (lua_istable(L, 2)) {
        lua_rawgeti(L, 2, 1);
        dx = static_cast<f32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        lua_rawgeti(L, 2, 3);
        dz = static_cast<f32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
    }
    auto result = sim->pathfinder()->find_path(
        unit->position().x, unit->position().z,
        dx, dz, unit->layer());
    lua_pushboolean(L, result.found ? 1 : 0);
    return 1;
}

// unit:GetArmorMult(damageType) → multiplier
static int unit_GetArmorMult(lua_State* L) {
    auto* u = check_unit(L);
    auto* sim = get_sim(L);
    if (!u || !sim) { lua_pushnumber(L, 1); return 1; }
    const char* dtype = (lua_type(L, 2) == LUA_TSTRING)
                        ? lua_tostring(L, 2) : "Normal";
    f32 mult = sim->armor_definition().get_multiplier(u->armor_type(), dtype);
    lua_pushnumber(L, mult);
    return 1;
}

// unit:AlterArmor(damageType, multiplier)
static int unit_AlterArmor(lua_State* L) {
    auto* u = check_unit(L);
    auto* sim = get_sim(L);
    if (!u || !sim) return 0;
    const char* dtype = lua_tostring(L, 2);
    f32 mult = static_cast<f32>(lua_tonumber(L, 3));
    if (dtype) {
        sim->armor_definition().set_multiplier(u->armor_type(), dtype, mult);
    }
    return 0;
}

// unit:SetRegenRate(rate) — sets HP/sec regeneration rate
static int unit_SetRegenRate(lua_State* L) {
    auto* e = check_entity(L);
    if (!e) return 0;
    e->set_regen_rate(static_cast<f32>(lua_tonumber(L, 2)));
    return 0;
}

// unit:RevertRegenRate() — resets regen to blueprint Defense.RegenRate
static int unit_RevertRegenRate(lua_State* L) {
    auto* u = check_unit(L);
    auto* sim = get_sim(L);
    if (!u || !sim) return 0;
    auto* store = sim->blueprint_store();
    if (!store) { u->set_regen_rate(0); return 0; }
    auto* entry = store->find(u->unit_id());
    if (!entry) { u->set_regen_rate(0); return 0; }

    f32 bp_regen = 0;
    store->push_lua_table(*entry, L);
    lua_pushstring(L, "Defense");
    lua_gettable(L, -2);
    if (lua_istable(L, -1)) {
        lua_pushstring(L, "RegenRate");
        lua_gettable(L, -2);
        if (lua_isnumber(L, -1))
            bp_regen = static_cast<f32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
    }
    lua_pop(L, 2); // Defense + bp table
    u->set_regen_rate(bp_regen);
    return 0;
}

// unit:SetStat(key, value) → returns boolean (true if stat was new)
static int unit_SetStat(lua_State* L) {
    auto* u = check_unit(L);
    if (!u) { lua_pushboolean(L, 0); return 1; }
    if (lua_type(L, 2) != LUA_TSTRING) { lua_pushboolean(L, 0); return 1; }
    std::string key = lua_tostring(L, 2);
    f64 value = lua_isnumber(L, 3) ? lua_tonumber(L, 3) : 0;
    bool is_new = !u->has_stat(key);
    u->set_stat(key, value);
    lua_pushboolean(L, is_new ? 1 : 0);
    return 1;
}

// unit:GetStat(key, [default]) → returns {Value = stored_or_default}
static int unit_GetStat(lua_State* L) {
    auto* u = check_unit(L);
    f64 val = 0;
    if (u && lua_type(L, 2) == LUA_TSTRING) {
        std::string key = lua_tostring(L, 2);
        f64 def = lua_isnumber(L, 3) ? lua_tonumber(L, 3) : 0;
        val = u->get_stat(key, def);
    }
    lua_newtable(L);
    lua_pushstring(L, "Value");
    lua_pushnumber(L, val);
    lua_rawset(L, -3);
    return 1;
}

// unit:UpdateStat(key, value) → fallback for units without full Lua class chain
static int unit_UpdateStat(lua_State* L) {
    auto* u = check_unit(L);
    if (!u) return 0;
    if (lua_type(L, 2) != LUA_TSTRING) return 0;
    std::string key = lua_tostring(L, 2);
    f64 value = lua_isnumber(L, 3) ? lua_tonumber(L, 3) : 0;
    u->set_stat(key, value);
    return 0;
}

// --- Silo ammo system ---

static int unit_GetNukeSiloAmmoCount(lua_State* L) {
    auto* u = check_unit(L);
    lua_pushnumber(L, u ? u->nuke_silo_ammo() : 0);
    return 1;
}

static int unit_GetTacticalSiloAmmoCount(lua_State* L) {
    auto* u = check_unit(L);
    lua_pushnumber(L, u ? u->tactical_silo_ammo() : 0);
    return 1;
}

static int unit_GiveNukeSiloAmmo(lua_State* L) {
    auto* u = check_unit(L);
    if (!u) return 0;
    i32 amount = (lua_type(L, 2) == LUA_TNUMBER) ? static_cast<i32>(lua_tonumber(L, 2)) : 1;
    if (amount > 0) u->give_nuke_silo_ammo(amount);
    return 0;
}

static int unit_GiveTacticalSiloAmmo(lua_State* L) {
    auto* u = check_unit(L);
    if (!u) return 0;
    i32 amount = (lua_type(L, 2) == LUA_TNUMBER) ? static_cast<i32>(lua_tonumber(L, 2)) : 1;
    if (amount > 0) u->give_tactical_silo_ammo(amount);
    return 0;
}

static int unit_RemoveNukeSiloAmmo(lua_State* L) {
    auto* u = check_unit(L);
    if (!u) return 0;
    i32 amount = (lua_type(L, 2) == LUA_TNUMBER) ? static_cast<i32>(lua_tonumber(L, 2)) : 1;
    if (amount > 0) u->remove_nuke_silo_ammo(amount);
    return 0;
}

static int unit_RemoveTacticalSiloAmmo(lua_State* L) {
    auto* u = check_unit(L);
    if (!u) return 0;
    i32 amount = (lua_type(L, 2) == LUA_TNUMBER) ? static_cast<i32>(lua_tonumber(L, 2)) : 1;
    if (amount > 0) u->remove_tactical_silo_ammo(amount);
    return 0;
}

// --- Targeting / reclaimable flags ---

static int entity_SetDoNotTarget(lua_State* L) {
    auto* e = check_entity(L);
    if (!e) return 0;
    bool val = lua_toboolean(L, 2) != 0;
    e->set_do_not_target(val);
    return 0;
}

static int entity_SetReclaimable(lua_State* L) {
    auto* e = check_entity(L);
    if (!e) return 0;
    bool val = lua_toboolean(L, 2) != 0;
    e->set_reclaimable(val);
    return 0;
}

static int unit_IsValidTarget(lua_State* L) {
    auto* u = check_unit(L);
    lua_pushboolean(L, (u && !u->do_not_target()) ? 1 : 0);
    return 1;
}

static int unit_SetIsValidTarget(lua_State* L) {
    auto* u = check_unit(L);
    if (!u) return 0;
    bool val = lua_toboolean(L, 2) != 0;
    u->set_do_not_target(!val);
    return 0;
}

// --- Movement multipliers ---

static int unit_SetAccMult(lua_State* L) {
    auto* u = check_unit(L);
    if (u) u->set_accel_mult(static_cast<f32>(luaL_checknumber(L, 2)));
    return 0;
}

static int unit_SetTurnMult(lua_State* L) {
    auto* u = check_unit(L);
    if (u) u->set_turn_mult(static_cast<f32>(luaL_checknumber(L, 2)));
    return 0;
}

static int unit_SetBreakOffDistanceMult(lua_State* L) {
    auto* u = check_unit(L);
    if (u) u->set_break_off_distance_mult(static_cast<f32>(luaL_checknumber(L, 2)));
    return 0;
}

static int unit_SetBreakOffTriggerMult(lua_State* L) {
    auto* u = check_unit(L);
    if (u) u->set_break_off_trigger_mult(static_cast<f32>(luaL_checknumber(L, 2)));
    return 0;
}

static int unit_ResetSpeedAndAccel(lua_State* L) {
    auto* u = check_unit(L);
    if (u) u->reset_speed_and_accel();
    return 0;
}

// --- Fuel system ---

static int unit_GetFuelRatio(lua_State* L) {
    auto* u = check_unit(L);
    lua_pushnumber(L, u ? u->fuel_ratio() : -1);
    return 1;
}

static int unit_SetFuelRatio(lua_State* L) {
    auto* u = check_unit(L);
    if (u) u->set_fuel_ratio(static_cast<f32>(luaL_checknumber(L, 2)));
    return 0;
}

static int unit_GetFuelUseTime(lua_State* L) {
    auto* u = check_unit(L);
    lua_pushnumber(L, u ? u->fuel_use_time() : 0);
    return 1;
}

static int unit_SetFuelUseTime(lua_State* L) {
    auto* u = check_unit(L);
    if (u) u->set_fuel_use_time(static_cast<f32>(luaL_checknumber(L, 2)));
    return 0;
}

// --- Misc flags ---

static int unit_SetCreator(lua_State* L) {
    auto* u = check_unit(L);
    if (!u) return 0;
    auto* creator = check_entity(L, 2);
    u->set_creator_id(creator ? creator->entity_id() : 0);
    return 0;
}

static int unit_SetAutoOvercharge(lua_State* L) {
    auto* u = check_unit(L);
    if (u) u->set_auto_overcharge(lua_toboolean(L, 2) != 0);
    return 0;
}

static int unit_GetAutoOvercharge(lua_State* L) {
    auto* u = check_unit(L);
    lua_pushboolean(L, (u && u->auto_overcharge()) ? 1 : 0);
    return 1;
}

static int unit_SetOverchargePaused(lua_State* L) {
    auto* u = check_unit(L);
    if (u) u->set_overcharge_paused(lua_toboolean(L, 2) != 0);
    return 0;
}

static int unit_SetFocusEntity(lua_State* L) {
    auto* u = check_unit(L);
    if (!u) return 0;
    auto* target = check_entity(L, 2);
    u->set_focus_entity_id(target ? target->entity_id() : 0);
    return 0;
}

static int unit_ClearFocusEntity(lua_State* L) {
    auto* u = check_unit(L);
    if (u) u->set_focus_entity_id(0);
    return 0;
}

// --- ToggleFireState ---

static int unit_ToggleFireState(lua_State* L) {
    auto* u = check_unit(L);
    if (u) u->set_fire_state((u->fire_state() + 1) % 3);
    return 0;
}

// --- Damage/kill flags + attacker ---

static int unit_SetCanTakeDamage(lua_State* L) {
    auto* u = check_unit(L);
    if (u) u->set_can_take_damage(lua_toboolean(L, 2) != 0);
    return 0;
}

static int unit_SetCanBeKilled(lua_State* L) {
    auto* u = check_unit(L);
    if (u) u->set_can_be_killed(lua_toboolean(L, 2) != 0);
    return 0;
}

static int unit_GetAttacker(lua_State* L) {
    auto* u = check_unit(L);
    if (!u || u->last_attacker_id() == 0) { lua_pushnil(L); return 1; }
    auto* sim = get_sim(L);
    if (!sim) { lua_pushnil(L); return 1; }
    auto* attacker = sim->entity_registry().find(u->last_attacker_id());
    if (!attacker || attacker->lua_table_ref() < 0) { lua_pushnil(L); return 1; }
    lua_rawgeti(L, LUA_REGISTRYINDEX, attacker->lua_table_ref());
    return 1;
}

// --- SetRotation ---

static int unit_SetRotation(lua_State* L) {
    auto* e = check_entity(L);
    if (!e) return 0;
    // Accept quaternion (4 numbers) or single yaw angle
    if (lua_isnumber(L, 3)) {
        // 4-arg: SetRotation(x, y, z, w)
        f32 x = static_cast<f32>(lua_tonumber(L, 2));
        f32 y = static_cast<f32>(lua_tonumber(L, 3));
        f32 z = static_cast<f32>(lua_tonumber(L, 4));
        f32 w = static_cast<f32>(lua_tonumber(L, 5));
        e->set_orientation({x, y, z, w});
    } else {
        // 1-arg: SetRotation(yaw_radians) — Y-axis rotation
        f32 yaw = static_cast<f32>(lua_tonumber(L, 2));
        f32 half = yaw * 0.5f;
        e->set_orientation({0, std::sin(half), 0, std::cos(half)});
    }
    return 0;
}

// --- SetBuildingUnit ---

static int unit_SetBuildingUnit(lua_State* L) {
    auto* u = check_unit(L);
    if (!u) return 0;
    // FA sig: SetBuildingUnit(self, bool, unit) — arg2=bool, arg3=entity
    // Also handle legacy: SetBuildingUnit(self, unit)
    int entity_idx = lua_isboolean(L, 2) ? 3 : 2;
    if (lua_istable(L, entity_idx)) {
        auto* target = check_entity(L, entity_idx);
        u->set_build_target_id(target ? target->entity_id() : 0);
    } else {
        u->set_build_target_id(0);
    }
    return 0;
}

// --- Command caps ---

static int unit_AddCommandCap(lua_State* L) {
    auto* u = check_unit(L);
    if (u && lua_type(L, 2) == LUA_TSTRING)
        u->add_command_cap(lua_tostring(L, 2));
    return 0;
}

static int unit_RemoveCommandCap(lua_State* L) {
    auto* u = check_unit(L);
    if (u && lua_type(L, 2) == LUA_TSTRING)
        u->remove_command_cap(lua_tostring(L, 2));
    return 0;
}

static int unit_RestoreCommandCaps(lua_State* L) {
    auto* u = check_unit(L);
    if (u) u->restore_command_caps();
    return 0;
}

// --- Build restrictions ---

static int unit_AddBuildRestriction(lua_State* L) {
    auto* u = check_unit(L);
    if (u && lua_type(L, 2) == LUA_TSTRING)
        u->add_build_restriction(lua_tostring(L, 2));
    return 0;
}

static int unit_RemoveBuildRestriction(lua_State* L) {
    auto* u = check_unit(L);
    if (u && lua_type(L, 2) == LUA_TSTRING)
        u->remove_build_restriction(lua_tostring(L, 2));
    return 0;
}

static int unit_RestoreBuildRestrictions(lua_State* L) {
    auto* u = check_unit(L);
    if (u) u->restore_build_restrictions();
    return 0;
}

// --- Elevation ---

static int unit_SetElevation(lua_State* L) {
    auto* u = check_unit(L);
    if (u) u->set_elevation_override(static_cast<f32>(luaL_checknumber(L, 2)));
    return 0;
}

static int unit_RevertElevation(lua_State* L) {
    auto* u = check_unit(L);
    if (u) u->clear_elevation_override();
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
    {"ToggleFireState",             unit_ToggleFireState},
    {"SetPaused",                   unit_SetPaused},
    {"IsPaused",                    unit_IsPaused},
    // Bones / visual
    {"ShowBone",                    unit_ShowBone},
    {"HideBone",                    unit_HideBone},
    {"SetMesh",                     stub_noop},
    {"IsValidBone",                 entity_IsValidBone},
    {"GetBoneDirection",            entity_GetBoneDirection},
    // Stubs — build / command
    {"GetCommandQueue",             unit_GetCommandQueue},
    {"CanBuild",                    unit_CanBuild},
    {"AddCommandCap",               unit_AddCommandCap},
    {"RemoveCommandCap",            unit_RemoveCommandCap},
    {"RestoreCommandCaps",          unit_RestoreCommandCaps},
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
    // Shield — EnableShield/DisableShield/ShieldIsOn are FA Lua overrides (stubs fine)
    {"EnableShield",                unit_EnableShield},
    {"DisableShield",               unit_DisableShield},
    {"ShieldIsOn",                  unit_ShieldIsOn},
    {"GetShieldRatio",              unit_GetShieldRatio},
    {"SetShieldRatio",              unit_SetShieldRatio},
    {"SetFocusEntity",              unit_SetFocusEntity},
    {"ClearFocusEntity",            unit_ClearFocusEntity},
    // Stubs — collision
    {"SetCollisionShape",           stub_noop},
    {"RevertCollisionShape",        stub_noop},
    {"RevertElevation",             unit_RevertElevation},
    {"SetElevation",                unit_SetElevation},
    // Stubs — movement
    {"IsMobile",                    unit_IsMobile},
    {"IsMoving",                    unit_IsMoving},
    {"GetNavigator",                unit_GetNavigator},
    {"SetSpeedMult",                unit_SetSpeedMult},
    {"SetAccMult",                  unit_SetAccMult},
    {"SetTurnMult",                 unit_SetTurnMult},
    {"SetBreakOffDistanceMult",     unit_SetBreakOffDistanceMult},
    {"SetBreakOffTriggerMult",      unit_SetBreakOffTriggerMult},
    {"GetCurrentMoveLocation",      entity_GetPosition},
    {"GetHeading",                  entity_GetHeading},
    // Stubs — transport / cargo
    {"GetCargo",                    unit_GetCargo},
    {"TransportHasSpaceFor",        unit_TransportHasSpaceFor},
    {"AddUnitToStorage",            unit_AddUnitToStorage},
    {"TransportDetachAllUnits",     unit_TransportDetachAllUnits},
    // Stubs — missiles
    {"GetNukeSiloAmmoCount",        unit_GetNukeSiloAmmoCount},
    {"GetTacticalSiloAmmoCount",    unit_GetTacticalSiloAmmoCount},
    {"GiveNukeSiloAmmo",            unit_GiveNukeSiloAmmo},
    {"GiveTacticalSiloAmmo",        unit_GiveTacticalSiloAmmo},
    {"RemoveNukeSiloAmmo",          unit_RemoveNukeSiloAmmo},
    {"RemoveTacticalSiloAmmo",      unit_RemoveTacticalSiloAmmo},
    // Armor
    {"GetArmorMult",                unit_GetArmorMult},
    {"AlterArmor",                  unit_AlterArmor},
    // Regen
    {"SetRegenRate",                unit_SetRegenRate},
    {"RevertRegenRate",             unit_RevertRegenRate},
    // Stubs — fuel
    {"GetFuelRatio",                unit_GetFuelRatio},
    {"SetFuelRatio",                unit_SetFuelRatio},
    {"GetFuelUseTime",              unit_GetFuelUseTime},
    {"SetFuelUseTime",              unit_SetFuelUseTime},
    // Stubs — misc
    {"IsValidTarget",               unit_IsValidTarget},
    {"SetIsValidTarget",            unit_SetIsValidTarget},
    {"SetScriptBit",                unit_SetScriptBit},
    {"GetScriptBit",                unit_GetScriptBit},
    {"AddBuildRestriction",         unit_AddBuildRestriction},
    {"RemoveBuildRestriction",      unit_RemoveBuildRestriction},
    {"AddOnGivenCallback",          stub_noop},
    {"PlayUnitSound",               stub_noop},
    {"PlayUnitAmbientSound",        stub_noop},
    {"StopUnitAmbientSound",        stub_noop},
    {"SetDoNotTarget",              entity_SetDoNotTarget},
    {"GetGuards",                   unit_GetGuards},
    {"UpdateStat",                  unit_UpdateStat},
    {"GetStat",                     unit_GetStat},
    {"SetStat",                     unit_SetStat},
    {"CanPathTo",                   unit_CanPathTo},
    {"CanPathToCell",               unit_CanPathToCell},
    {"GetAttacker",                 unit_GetAttacker},
    {"SetReclaimable",              entity_SetReclaimable},
    {"SetCapturable",               unit_SetCapturable},
    {"IsCapturable",                unit_IsCapturable},
    {"GetParent",                   unit_GetParent},
    {"SetCanTakeDamage",            unit_SetCanTakeDamage},
    {"SetCanBeKilled",              unit_SetCanBeKilled},
    {"SetUnSelectable",             stub_noop},
    {"SetAutoOvercharge",           unit_SetAutoOvercharge},
    {"ToggleScriptBit",             unit_ToggleScriptBit},
    {"GetAutoOvercharge",           unit_GetAutoOvercharge},
    {"SetOverchargePaused",         unit_SetOverchargePaused},
    {"RemoveSpecifiedEnhancement",  unit_RemoveSpecifiedEnhancement},
    {"HasEnhancement",              unit_HasEnhancement},
    {"CreateEnhancement",           unit_CreateEnhancement},
    {"GetResourceConsumed",         unit_GetResourceConsumed},
    {"SetImmobile",                 unit_SetImmobile},
    {"GetNumBuildOrders",           unit_GetNumBuildOrders},
    {"SetBuildingUnit",             unit_SetBuildingUnit},
    {"GetUnitBeingBuilt",           unit_GetUnitBeingBuilt},
    {"Stop",                        unit_Stop},
    {"Kill",                        entity_Destroy},
    {"GetFocusUnit",                unit_GetFocusUnit},
    {"RestoreBuildRestrictions",    unit_RestoreBuildRestrictions},
    {"SetCreator",                  unit_SetCreator},
    {"OccupyGround",               stub_return_true},
    {"ResetSpeedAndAccel",          unit_ResetSpeedAndAccel},
    {"AddToggleCap",                unit_AddToggleCap},
    {"RemoveToggleCap",             unit_RemoveToggleCap},
    {"TestToggleCaps",              unit_TestToggleCaps},
    {"SetBlockCommandQueue",        unit_SetBlockCommandQueue},
    {"PlayCommanderWarpInEffect",   stub_noop},
    {"SetAmbientSound",             entity_SetAmbientSound},
    {"GetRallyPoint",                unit_GetRallyPoint},
    {"SetRallyPoint",                unit_SetRallyPoint},
    {"SetBusy",                      unit_SetBusy},
    {"SetRotation",                  unit_SetRotation},
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

// proj:SetMaxSpeed(speed) — set max speed, return self for chaining
static int proj_SetMaxSpeed(lua_State* L) {
    auto* p = check_projectile(L);
    if (p) p->max_speed = static_cast<f32>(luaL_checknumber(L, 2));
    lua_pushvalue(L, 1);
    return 1;
}

// proj:SetAcceleration(accel) — set linear acceleration, return self
static int proj_SetAcceleration(lua_State* L) {
    auto* p = check_projectile(L);
    if (p) p->acceleration = static_cast<f32>(luaL_checknumber(L, 2));
    lua_pushvalue(L, 1);
    return 1;
}

// proj:SetBallisticAcceleration(accel) — set vertical gravity, return self
static int proj_SetBallisticAcceleration(lua_State* L) {
    auto* p = check_projectile(L);
    if (p) p->ballistic_accel = static_cast<f32>(luaL_checknumber(L, 2));
    lua_pushvalue(L, 1);
    return 1;
}

// proj:SetTurnRate(rate) — store turn rate (no heading-based flight yet)
static int proj_SetTurnRate(lua_State* L) {
    auto* p = check_projectile(L);
    if (p) p->turn_rate = static_cast<f32>(luaL_checknumber(L, 2));
    lua_pushvalue(L, 1);
    return 1;
}

// proj:SetTurnRateByDist(rate) — same as SetTurnRate for now, return self
static int proj_SetTurnRateByDist(lua_State* L) {
    auto* p = check_projectile(L);
    if (p) p->turn_rate = static_cast<f32>(luaL_checknumber(L, 2));
    lua_pushvalue(L, 1);
    return 1;
}

// --- Projectile target + guidance ---

static int proj_GetCurrentTargetPosition(lua_State* L) {
    auto* p = check_projectile(L);
    if (!p) { lua_pushnil(L); return 1; }
    lua_newtable(L);
    lua_pushnumber(L, 1); lua_pushnumber(L, p->target_position.x); lua_rawset(L, -3);
    lua_pushnumber(L, 2); lua_pushnumber(L, p->target_position.y); lua_rawset(L, -3);
    lua_pushnumber(L, 3); lua_pushnumber(L, p->target_position.z); lua_rawset(L, -3);
    return 1;
}

static int proj_GetCurrentTargetPositionXYZ(lua_State* L) {
    auto* p = check_projectile(L);
    if (!p) { lua_pushnumber(L, 0); lua_pushnumber(L, 0); lua_pushnumber(L, 0); return 3; }
    lua_pushnumber(L, p->target_position.x);
    lua_pushnumber(L, p->target_position.y);
    lua_pushnumber(L, p->target_position.z);
    return 3;
}

static int proj_GetTrackingTarget(lua_State* L) {
    auto* p = check_projectile(L);
    if (!p || p->target_entity_id == 0) { lua_pushnil(L); return 1; }
    auto* sim = get_sim(L);
    if (!sim) { lua_pushnil(L); return 1; }
    auto* target = sim->entity_registry().find(p->target_entity_id);
    if (!target || target->lua_table_ref() < 0) { lua_pushnil(L); return 1; }
    lua_rawgeti(L, LUA_REGISTRYINDEX, target->lua_table_ref());
    return 1;
}

static int proj_TrackTarget(lua_State* L) {
    auto* p = check_projectile(L);
    if (p) p->tracking = (lua_toboolean(L, 2) != 0);
    return 0;
}

static int proj_ChangeMaxZigZag(lua_State* L) {
    auto* p = check_projectile(L);
    if (p) p->max_zig_zag = static_cast<f32>(luaL_checknumber(L, 2));
    return 0;
}

static int proj_ChangeZigZagFrequency(lua_State* L) {
    auto* p = check_projectile(L);
    if (p) p->zig_zag_freq = static_cast<f32>(luaL_checknumber(L, 2));
    return 0;
}

static int proj_GetMaxZigZag(lua_State* L) {
    auto* p = check_projectile(L);
    lua_pushnumber(L, p ? p->max_zig_zag : 0);
    return 1;
}

static int proj_GetZigZagFrequency(lua_State* L) {
    auto* p = check_projectile(L);
    lua_pushnumber(L, p ? p->zig_zag_freq : 0);
    return 1;
}

static int proj_ChangeDetonateAboveHeight(lua_State* L) {
    auto* p = check_projectile(L);
    if (p) p->detonate_above_height = static_cast<f32>(luaL_checknumber(L, 2));
    return 0;
}

static int proj_ChangeDetonateBelowHeight(lua_State* L) {
    auto* p = check_projectile(L);
    if (p) p->detonate_below_height = static_cast<f32>(luaL_checknumber(L, 2));
    return 0;
}

// --- Projectile physics flags ---

static int proj_SetDestroyOnWater(lua_State* L) {
    auto* p = check_projectile(L);
    if (p) p->destroy_on_water = (lua_toboolean(L, 2) != 0);
    return 0;
}

static int proj_SetStayUpright(lua_State* L) {
    auto* p = check_projectile(L);
    if (p) p->stay_upright = (lua_toboolean(L, 2) != 0);
    return 0;
}

static int proj_SetVelocityAlign(lua_State* L) {
    auto* p = check_projectile(L);
    if (p) p->velocity_align = (lua_toboolean(L, 2) != 0);
    return 0;
}

static int proj_SetScaleVelocity(lua_State* L) {
    auto* p = check_projectile(L);
    if (p) {
        f32 s = static_cast<f32>(luaL_checknumber(L, 2));
        p->velocity.x *= s;
        p->velocity.y *= s;
        p->velocity.z *= s;
    }
    lua_pushvalue(L, 1); // return self for chaining
    return 1;
}

static int proj_SetLocalAngularVelocity(lua_State* L) {
    auto* p = check_projectile(L);
    if (p) {
        p->angular_velocity.x = static_cast<f32>(luaL_checknumber(L, 2));
        p->angular_velocity.y = static_cast<f32>(luaL_checknumber(L, 3));
        p->angular_velocity.z = static_cast<f32>(luaL_checknumber(L, 4));
    }
    lua_pushvalue(L, 1); // return self for chaining
    return 1;
}

// --- Projectile collision + CreateChildProjectile (M51) ---

static int proj_SetCollision(lua_State* L) {
    auto* p = check_projectile(L);
    if (p) p->collision_enabled = (lua_toboolean(L, 2) != 0);
    lua_pushvalue(L, 1); return 1; // return self for chaining
}

static int proj_SetCollideEntity(lua_State* L) {
    // No-op + chaining (collision system not fully implemented)
    lua_pushvalue(L, 1); return 1;
}

static int proj_SetCollideSurface(lua_State* L) {
    auto* p = check_projectile(L);
    if (p) p->collide_surface = (lua_toboolean(L, 2) != 0);
    lua_pushvalue(L, 1); return 1;
}

static int proj_StayUnderwater(lua_State* L) {
    auto* p = check_projectile(L);
    if (p) p->stay_underwater = (lua_toboolean(L, 2) != 0);
    lua_pushvalue(L, 1); // return self for chaining
    return 1;
}

static int proj_CreateChildProjectile(lua_State* L) {
    auto* parent = check_projectile(L);
    if (!parent || parent->destroyed()) { lua_pushnil(L); return 1; }

    auto* sim = get_sim(L);
    if (!sim) { lua_pushnil(L); return 1; }

    auto child = std::make_unique<sim::Projectile>();
    child->set_position(parent->position());
    child->set_army(parent->army());
    child->launcher_id = parent->launcher_id;
    child->velocity = parent->velocity;
    child->lifetime = 10.0f;

    // Optional bp_id arg
    if (lua_type(L, 2) == LUA_TSTRING) {
        // bp_id provided but we don't parse projectile blueprints yet
    }

    u32 child_id = sim->entity_registry().register_entity(std::move(child));
    auto* child_ptr = static_cast<sim::Projectile*>(
        sim->entity_registry().find(child_id));

    // Create Lua table with projectile metatable
    lua_newtable(L);
    lua_pushstring(L, "_c_object");
    lua_pushlightuserdata(L, child_ptr);
    lua_rawset(L, -3);

    // Set __osc_proj_mt metatable (cached in registry)
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
    child_ptr->set_lua_table_ref(ref);
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    return 1;
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
    {"GetTrackingTarget",           proj_GetTrackingTarget},
    {"GetCurrentTargetPosition",    proj_GetCurrentTargetPosition},
    {"GetCurrentTargetPositionXYZ", proj_GetCurrentTargetPositionXYZ},
    {"TrackTarget",                 proj_TrackTarget},
    {"SetScaleVelocity",            proj_SetScaleVelocity},
    {"SetMaxSpeed",                 proj_SetMaxSpeed},
    {"SetAcceleration",             proj_SetAcceleration},
    {"SetBallisticAcceleration",    proj_SetBallisticAcceleration},
    {"SetCollideEntity",            proj_SetCollideEntity},
    {"SetCollideSurface",           proj_SetCollideSurface},
    {"SetCollision",                proj_SetCollision},
    {"SetDestroyOnWater",           proj_SetDestroyOnWater},
    {"SetLocalAngularVelocity",     proj_SetLocalAngularVelocity},
    {"SetTurnRate",                 proj_SetTurnRate},
    {"SetStayUpright",              proj_SetStayUpright},
    {"SetVelocityAlign",            proj_SetVelocityAlign},
    {"StayUnderwater",              proj_StayUnderwater},
    {"CreateChildProjectile",       proj_CreateChildProjectile},
    {"ChangeMaxZigZag",             proj_ChangeMaxZigZag},
    {"ChangeZigZagFrequency",       proj_ChangeZigZagFrequency},
    {"GetMaxZigZag",                proj_GetMaxZigZag},
    {"GetZigZagFrequency",          proj_GetZigZagFrequency},
    {"ChangeDetonateAboveHeight",   proj_ChangeDetonateAboveHeight},
    {"ChangeDetonateBelowHeight",   proj_ChangeDetonateBelowHeight},
    {"SetTurnRateByDist",           proj_SetTurnRateByDist},
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

static int weapon_SetTargetGround(lua_State* L) {
    auto* w = check_weapon(L);
    if (w) w->target_ground = (lua_toboolean(L, 2) != 0);
    return 0;
}

static int weapon_SetFireControl(lua_State* L) {
    auto* w = check_weapon(L);
    if (w) w->fire_control = (lua_toboolean(L, 2) != 0);
    return 0;
}

static int weapon_IsFireControl(lua_State* L) {
    auto* w = check_weapon(L);
    lua_pushboolean(L, (w && w->fire_control) ? 1 : 0);
    return 1;
}

static int weapon_TransferTarget(lua_State* L) {
    auto* w = check_weapon(L);
    auto* src = check_weapon(L, 2);
    if (w && src) w->target_entity_id = src->target_entity_id;
    return 0;
}

static int weapon_SetTargetingPriorities(lua_State* L) {
    auto* w = check_weapon(L);
    if (!w) return 0;
    if (w->targeting_priorities_ref >= 0)
        luaL_unref(L, LUA_REGISTRYINDEX, w->targeting_priorities_ref);
    if (lua_istable(L, 2)) {
        lua_pushvalue(L, 2);
        w->targeting_priorities_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    } else {
        w->targeting_priorities_ref = -2;
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
            target_unit->armor_type(), w->damage_type.c_str());
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
        lua_pcall(L, 5, 0, 0);
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

// prop:SetMaxReclaimValues(time, mass, energy)
// Sets reclaim fields on the Lua table (read by progress_reclaim)
static int prop_SetMaxReclaimValues(lua_State* L) {
    if (!lua_istable(L, 1)) return 0;
    f64 time   = lua_tonumber(L, 2);
    f64 mass   = lua_tonumber(L, 3);
    f64 energy = lua_tonumber(L, 4);

    lua_pushstring(L, "MaxMassReclaim");
    lua_pushnumber(L, mass);
    lua_rawset(L, 1);

    lua_pushstring(L, "MaxEnergyReclaim");
    lua_pushnumber(L, energy);
    lua_rawset(L, 1);

    lua_pushstring(L, "TimeReclaim");
    lua_pushnumber(L, time);
    lua_rawset(L, 1);

    lua_pushstring(L, "ReclaimLeft");
    lua_pushnumber(L, 1.0);
    lua_rawset(L, 1);

    return 0;
}

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
    {"SetReclaimable",              entity_SetReclaimable},
    {"SetMaxReclaimValues",          prop_SetMaxReclaimValues},
    {"SetPropCollision",             stub_noop},
    {"GetHeading",                   entity_GetHeading},
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

// GetEconomyUsage = actual consumption (capped by available resources).
static int brain_GetEconomyUsage(lua_State* L) {
    auto* brain = check_brain(L);
    const char* res = luaL_checkstring(L, 2);
    lua_pushnumber(L, brain ? brain->get_economy_usage(res) : 0.0);
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

// brain:GetThreatBetweenPositions(pos1, pos2, checkVis, threatType)
// Returns the maximum threat at any sample point along the line from pos1 to pos2.
// Samples every 32 world units (one threat ring). Useful for evaluating path danger.
static int brain_GetThreatBetweenPositions(lua_State* L) {
    auto* brain = check_brain(L);
    auto* sim = get_sim(L);
    if (!brain || !sim) {
        lua_pushnumber(L, 0);
        return 1;
    }

    // arg 2: pos1
    f32 x1 = 0, z1 = 0;
    if (lua_istable(L, 2)) {
        lua_rawgeti(L, 2, 1);
        x1 = static_cast<f32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        lua_rawgeti(L, 2, 3);
        z1 = static_cast<f32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
    }
    // arg 3: pos2
    f32 x2 = 0, z2 = 0;
    if (lua_istable(L, 3)) {
        lua_rawgeti(L, 3, 1);
        x2 = static_cast<f32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        lua_rawgeti(L, 3, 3);
        z2 = static_cast<f32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
    }
    // arg 4: checkVis (ignored)
    const char* threat_type =
        (lua_type(L, 5) == LUA_TSTRING) ? lua_tostring(L, 5) : "Overall";

    // Sample along line every 32 units (one threat ring)
    f32 dx = x2 - x1, dz = z2 - z1;
    f32 dist = std::sqrt(dx * dx + dz * dz);
    constexpr f32 SAMPLE_SPACING = 32.0f;
    i32 samples = std::max(1, static_cast<i32>(dist / SAMPLE_SPACING));

    f32 max_threat = 0;
    for (i32 i = 0; i <= samples; ++i) {
        f32 t = static_cast<f32>(i) / static_cast<f32>(samples);
        f32 px = x1 + dx * t;
        f32 pz = z1 + dz * t;

        auto ids = sim->entity_registry().collect_in_radius(
            px, pz, SAMPLE_SPACING);
        f32 sample_threat = 0;
        for (u32 eid : ids) {
            auto* entity = sim->entity_registry().find(eid);
            if (!entity || !entity->is_unit() || entity->destroyed()) continue;
            auto* unit = static_cast<sim::Unit*>(entity);
            if (!sim->is_enemy(brain->index(), unit->army())) continue;
            sample_threat += get_unit_threat_for_type(unit, threat_type);
        }
        max_threat = std::max(max_threat, sample_threat);
    }

    lua_pushnumber(L, max_threat);
    return 1;
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

// brain:CanBuildStructureAt(bp_id, position) -> bool
// Check if footprint fits at position (no impassable/obstacle cells)
static int brain_CanBuildStructureAt(lua_State* L) {
    auto* sim = get_sim(L);
    if (!sim) { lua_pushboolean(L, 1); return 1; }

    auto* grid = sim->pathfinding_grid();
    if (!grid) { lua_pushboolean(L, 1); return 1; } // no grid → allow

    // arg 2: bp_id string (optional — read footprint from blueprint)
    f32 size_x = 1.0f, size_z = 1.0f;
    if (lua_isstring(L, 2)) {
        const char* bp_id = lua_tostring(L, 2);
        lua_pushstring(L, "__blueprints");
        lua_rawget(L, LUA_GLOBALSINDEX);
        if (lua_istable(L, -1)) {
            lua_pushstring(L, bp_id);
            lua_rawget(L, -2);
            if (lua_istable(L, -1)) {
                int bp = lua_gettop(L);
                lua_pushstring(L, "Footprint");
                lua_rawget(L, bp);
                if (lua_istable(L, -1)) {
                    int fp = lua_gettop(L);
                    lua_pushstring(L, "SizeX");
                    lua_rawget(L, fp);
                    if (lua_isnumber(L, -1)) size_x = static_cast<f32>(lua_tonumber(L, -1));
                    lua_pop(L, 1);
                    lua_pushstring(L, "SizeZ");
                    lua_rawget(L, fp);
                    if (lua_isnumber(L, -1)) size_z = static_cast<f32>(lua_tonumber(L, -1));
                    lua_pop(L, 1);
                }
                lua_pop(L, 1); // Footprint
            }
            lua_pop(L, 1); // bp table
        }
        lua_pop(L, 1); // __blueprints
    }

    // arg 3: position table — FindPlaceToBuild returns {[1]=x, [2]=z, [3]=dist}
    // Standard Vector3 is {[1]=x, [2]=y, [3]=z}. Read [2] as Z to match
    // FindPlaceToBuild (primary caller); for Vector3 this gets y but building
    // placement only needs X/Z and y=elevation is irrelevant for grid checks.
    f32 wx = 0, wz = 0;
    if (lua_istable(L, 3)) {
        lua_pushnumber(L, 1);
        lua_rawget(L, 3);
        wx = static_cast<f32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        lua_pushnumber(L, 2);
        lua_rawget(L, 3);
        wz = static_cast<f32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
    }

    // Check all cells in footprint area
    f32 half_x = size_x * 0.5f;
    f32 half_z = size_z * 0.5f;
    u32 gx0, gz0, gx1, gz1;
    grid->world_to_grid(wx - half_x, wz - half_z, gx0, gz0);
    grid->world_to_grid(wx + half_x, wz + half_z, gx1, gz1);

    for (u32 gz = gz0; gz <= gz1; ++gz) {
        for (u32 gx = gx0; gx <= gx1; ++gx) {
            auto cell = grid->get(gx, gz);
            if (cell == map::CellPassability::Impassable ||
                cell == map::CellPassability::Obstacle) {
                lua_pushboolean(L, 0);
                return 1;
            }
        }
    }
    lua_pushboolean(L, 1);
    return 1;
}

// brain:IsAnyEngineerBuilding(category) -> bool
// Check if any engineer/commander in this army is building a unit matching category
static int brain_IsAnyEngineerBuilding(lua_State* L) {
    auto* brain = check_brain(L);
    auto* sim = get_sim(L);
    if (!brain || !sim) { lua_pushboolean(L, 0); return 1; }

    int cat_idx = lua_istable(L, 2) ? 2 : 0;
    i32 army = brain->index();

    bool found = false;
    sim->entity_registry().for_each([&](sim::Entity& e) {
        if (found) return;
        if (e.destroyed() || !e.is_unit()) return;
        auto& unit = static_cast<sim::Unit&>(e);
        if (unit.army() != army) return;
        if (!unit.has_category("ENGINEER") && !unit.has_category("COMMAND"))
            return;
        if (!unit.is_building()) return;

        // Check if the unit being built matches the category
        if (cat_idx > 0) {
            auto* target = sim->entity_registry().find(unit.build_target_id());
            if (!target || target->destroyed() || !target->is_unit()) return;
            auto* target_unit = static_cast<sim::Unit*>(target);
            if (!osc::lua::unit_matches_category(L, cat_idx, target_unit->categories()))
                return;
        }
        found = true;
    });

    lua_pushboolean(L, found ? 1 : 0);
    return 1;
}

// --- Brain event callbacks + utility (M51) ---

static int brain_OnVictory(lua_State* L) {
    auto* brain = check_brain(L);
    if (brain) brain->set_state(sim::BrainState::Victory);
    return 0;
}

static int brain_OnDefeat(lua_State* L) {
    auto* brain = check_brain(L);
    if (brain) brain->set_state(sim::BrainState::Defeat);
    return 0;
}

static int brain_OnDraw(lua_State* L) {
    auto* brain = check_brain(L);
    if (brain) brain->set_state(sim::BrainState::Draw);
    return 0;
}

static int brain_OnUnitStopBeingBuilt(lua_State* L) {
    // Lua→C++ event; no-op bookkeeping placeholder
    (void)L;
    return 0;
}

static int brain_SetCurrentPlan(lua_State* L) {
    auto* brain = check_brain(L);
    if (brain && lua_type(L, 2) == LUA_TSTRING)
        brain->set_current_plan(lua_tostring(L, 2));
    return 0;
}

static int brain_GiveStorage(lua_State* L) {
    auto* brain = check_brain(L);
    if (!brain) return 0;
    if (lua_type(L, 2) != LUA_TSTRING) return 0;
    std::string type = lua_tostring(L, 2);
    f64 amount = lua_tonumber(L, 3);
    if (amount <= 0) return 0; // guard: storage can only increase via GiveStorage
    if (type == "MASS" || type == "Mass")
        brain->economy().mass.max_storage += amount;
    else if (type == "ENERGY" || type == "Energy")
        brain->economy().energy.max_storage += amount;
    return 0;
}

static int brain_SetResourceSharing(lua_State* L) {
    auto* brain = check_brain(L);
    if (brain) brain->set_resource_sharing(lua_toboolean(L, 2) != 0);
    return 0;
}

static int brain_GetArmySkinName(lua_State* L) {
    auto* brain = check_brain(L);
    if (brain && !brain->skin_name().empty())
        lua_pushstring(L, brain->skin_name().c_str());
    else
        lua_pushstring(L, "");
    return 1;
}

// Not called in FA — named no-ops to replace generic stubs
static int brain_SetArmyStatsTrigger(lua_State*) { return 0; }
static int brain_RemoveArmyStatsTrigger(lua_State*) { return 0; }
static int brain_RemoveEnergyDependingEntity(lua_State*) { return 0; }
static int brain_PBMAddBuildLocation(lua_State*) { return 0; }
static int brain_PBMRemoveBuildLocation(lua_State*) { return 0; }
static int brain_SetUpAttackVectorsToArmy(lua_State*) { return 0; }
static int brain_SetGreaterOf(lua_State*) { return 0; }

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
    {"SetArmyStatsTrigger",         brain_SetArmyStatsTrigger},
    // AddUnitStat — defined in StatManagerBrainComponent (Lua)
    // AddEnergyDependingEntity — defined in EnergyManagerBrainComponent (Lua)
    {"GetEconomyUsage",             brain_GetEconomyUsage},
    // TrackJammer — defined in JammerManagerBrainComponent (Lua)
    {"RemoveArmyStatsTrigger",      brain_RemoveArmyStatsTrigger},
    {"RemoveEnergyDependingEntity", brain_RemoveEnergyDependingEntity},
    {"GiveStorage",                 brain_GiveStorage},
    {"OnUnitStopBeingBuilt",        brain_OnUnitStopBeingBuilt},
    {"OnVictory",                   brain_OnVictory},
    {"OnDefeat",                    brain_OnDefeat},
    {"OnDraw",                      brain_OnDraw},
    {"SetArmyColor",                brain_SetArmyColor},
    // Stubs (AI/platoon features not yet implemented)
    {"AssignUnitsToPlatoon",        brain_AssignUnitsToPlatoon},
    {"PlatoonExists",               brain_PlatoonExists},
    {"DisbandPlatoon",              brain_DisbandPlatoon},
    {"SetResourceSharing",          brain_SetResourceSharing},
    {"GetThreatAtPosition",         brain_GetThreatAtPosition},
    {"GetThreatsAroundPosition",    brain_GetThreatsAroundPosition},
    {"GetThreatBetweenPositions",   brain_GetThreatBetweenPositions},
    {"IsAnyEngineerBuilding",       brain_IsAnyEngineerBuilding},
    {"SetCurrentPlan",              brain_SetCurrentPlan},
    {"PBMRemoveBuildLocation",      brain_PBMRemoveBuildLocation},
    {"PBMAddBuildLocation",         brain_PBMAddBuildLocation},
    {"SetUpAttackVectorsToArmy",    brain_SetUpAttackVectorsToArmy},
    {"FindPlaceToBuild",            brain_FindPlaceToBuild},
    {"CanBuildStructureAt",         brain_CanBuildStructureAt},
    {"BuildUnit",                   brain_BuildUnit},
    {"BuildStructure",              brain_BuildStructure},
    {"DecideWhatToBuild",           brain_DecideWhatToBuild},
    {"MakePlatoon",                 brain_MakePlatoon},
    {"GetPlatoonUniquelyNamed",     brain_GetPlatoonUniquelyNamed},
    {"GetHighestThreatPosition",    brain_GetHighestThreatPosition},
    {"SetGreaterOf",                brain_SetGreaterOf},
    {"GetArmySkinName",             brain_GetArmySkinName},
    {"GetCurrentEnemy",             brain_GetCurrentEnemy},
    {"SetCurrentEnemy",             brain_SetCurrentEnemy},
    {"GetNumUnitsAroundPoint",      brain_GetNumUnitsAroundPoint},
    {"GetPlatoonsList",             brain_GetPlatoonsList},
    {nullptr, nullptr},
};
// clang-format on

// ====================================================================
// Shield methods — real implementations
// ====================================================================
// Shield-specific methods only — GetHealth, SetHealth, GetMaxHealth, Destroy,
// BeenDestroyed are inherited from entity_methods via the base class chain.
// DO NOT re-declare them here or ClassShield(moho.shield_methods, Entity) will
// error with "field 'X' is ambiguous between the bases" because the flattened
// closures differ from the Entity class's copies.

static int shield_TurnOn(lua_State* L) {
    auto* s = check_shield(L);
    if (s) s->is_on = true;
    return 0;
}
static int shield_TurnOff(lua_State* L) {
    auto* s = check_shield(L);
    if (s) s->is_on = false;
    return 0;
}
static int shield_IsOn(lua_State* L) {
    auto* s = check_shield(L);
    lua_pushboolean(L, (s && s->is_on) ? 1 : 0);
    return 1;
}
static int shield_GetOmni(lua_State* L) {
    // No fog of war / omni detection yet
    lua_pushboolean(L, 0);
    return 1;
}
static int shield_GetType(lua_State* L) {
    auto* s = check_shield(L);
    if (s && !s->shield_type.empty()) {
        lua_pushstring(L, s->shield_type.c_str());
    } else {
        lua_pushstring(L, "");
    }
    return 1;
}
static int shield_SetSize(lua_State* L) {
    auto* s = check_shield(L);
    if (s) {
        s->size = static_cast<f32>(luaL_checknumber(L, 2));
    }
    return 0;
}

static const MethodEntry shield_methods[] = {
    {"GetOmni",                     shield_GetOmni},
    {"TurnOn",                      shield_TurnOn},
    {"TurnOff",                     shield_TurnOff},
    {"IsOn",                        shield_IsOn},
    {"GetType",                     shield_GetType},
    {"SetSize",                     shield_SetSize},
    {nullptr, nullptr},
};

// ---------------------------------------------------------------------------
// Blip methods — real implementations using visibility grid
// ---------------------------------------------------------------------------

// Helper: extract entity from blip table (same _c_object pattern as check_entity)
static sim::Entity* check_blip_entity(lua_State* L) {
    if (!lua_istable(L, 1)) return nullptr;
    lua_pushstring(L, "_c_object");
    lua_rawget(L, 1);
    auto* entity = lua_isuserdata(L, -1)
                       ? static_cast<sim::Entity*>(lua_touserdata(L, -1))
                       : nullptr;
    lua_pop(L, 1);
    return entity;
}

/// Read _c_entity_id from blip table (arg 1).
static u32 get_blip_entity_id(lua_State* L) {
    if (!lua_istable(L, 1)) return 0;
    lua_pushstring(L, "_c_entity_id");
    lua_rawget(L, 1);
    u32 id = lua_isnumber(L, -1) ? static_cast<u32>(lua_tonumber(L, -1)) : 0;
    lua_pop(L, 1);
    return id;
}

/// Read _c_req_army from blip table (arg 1). Returns 0-based army, -1 if absent.
static i32 get_blip_req_army(lua_State* L) {
    if (!lua_istable(L, 1)) return -1;
    lua_pushstring(L, "_c_req_army");
    lua_rawget(L, 1);
    i32 army = lua_isnumber(L, -1) ? static_cast<i32>(lua_tonumber(L, -1)) : -1;
    lua_pop(L, 1);
    return army;
}

// --- Blip methods (dead-reckoning + stealth-aware) ---

static int blip_GetPosition(lua_State* L) {
    auto* e = check_blip_entity(L);
    if (e && !e->destroyed()) {
        // Entity alive — check if requesting army has current intel
        i32 req_army = get_blip_req_army(L);
        if (req_army >= 0 && req_army != e->army()) {
            auto* sim = get_sim(L);
            if (sim && sim->has_any_intel(e, static_cast<u32>(req_army))) {
                push_vector3(L, e->position());
            } else {
                // Dead-reckoning: return cached position
                u32 eid = e->entity_id();
                auto* snap = sim ? sim->get_blip_snapshot(
                    eid, static_cast<u32>(req_army)) : nullptr;
                if (snap)
                    push_vector3(L, snap->last_known_position);
                else
                    push_vector3(L, e->position()); // fallback
            }
        } else {
            push_vector3(L, e->position()); // own army sees real position
        }
    } else {
        // Entity destroyed — use blip cache
        u32 eid = get_blip_entity_id(L);
        i32 req_army = get_blip_req_army(L);
        auto* sim = get_sim(L);
        if (sim && req_army >= 0) {
            auto* snap = sim->get_blip_snapshot(eid,
                                                 static_cast<u32>(req_army));
            if (snap)
                push_vector3(L, snap->last_known_position);
            else
                push_vector3(L, {0, 0, 0});
        } else {
            push_vector3(L, {0, 0, 0});
        }
    }
    return 1;
}

static int blip_IsSeenNow(lua_State* L) {
    auto* e = check_blip_entity(L);
    if (!e || e->destroyed()) { lua_pushboolean(L, 0); return 1; }
    i32 army = lua_isnumber(L, 2) ? static_cast<i32>(lua_tonumber(L, 2)) - 1
                                  : -1;
    auto* sim = get_sim(L);
    if (sim && sim->visibility_grid() && army >= 0) {
        auto& pos = e->position();
        lua_pushboolean(
            L, sim->visibility_grid()->has_vision(pos.x, pos.z,
                                                  static_cast<u32>(army)) ? 1 : 0);
    } else {
        lua_pushboolean(L, 0);
    }
    return 1;
}

static int blip_IsOnRadar(lua_State* L) {
    auto* e = check_blip_entity(L);
    if (!e || e->destroyed()) { lua_pushboolean(L, 0); return 1; }
    i32 army = lua_isnumber(L, 2) ? static_cast<i32>(lua_tonumber(L, 2)) - 1
                                  : -1;
    auto* sim = get_sim(L);
    if (sim && army >= 0) {
        // Stealth-aware: RadarStealth negates radar unless observer has Omni
        lua_pushboolean(
            L, sim->has_effective_radar(e, static_cast<u32>(army)) ? 1 : 0);
    } else {
        lua_pushboolean(L, 0);
    }
    return 1;
}

static int blip_IsOnSonar(lua_State* L) {
    auto* e = check_blip_entity(L);
    if (!e || e->destroyed()) { lua_pushboolean(L, 0); return 1; }
    i32 army = lua_isnumber(L, 2) ? static_cast<i32>(lua_tonumber(L, 2)) - 1
                                  : -1;
    auto* sim = get_sim(L);
    if (sim && army >= 0) {
        // Stealth-aware: SonarStealth negates sonar unless observer has Omni
        lua_pushboolean(
            L, sim->has_effective_sonar(e, static_cast<u32>(army)) ? 1 : 0);
    } else {
        lua_pushboolean(L, 0);
    }
    return 1;
}

static int blip_IsOnOmni(lua_State* L) {
    auto* e = check_blip_entity(L);
    if (!e || e->destroyed()) { lua_pushboolean(L, 0); return 1; }
    i32 army = lua_isnumber(L, 2) ? static_cast<i32>(lua_tonumber(L, 2)) - 1
                                  : -1;
    auto* sim = get_sim(L);
    if (sim && sim->visibility_grid() && army >= 0) {
        auto& pos = e->position();
        lua_pushboolean(
            L, sim->visibility_grid()->has_omni(pos.x, pos.z,
                                                static_cast<u32>(army)) ? 1 : 0);
    } else {
        lua_pushboolean(L, 0);
    }
    return 1;
}

static int blip_IsSeenEver(lua_State* L) {
    auto* e = check_blip_entity(L);
    if (!e || e->destroyed()) {
        // Destroyed entity — if we have a blip cache entry, it was seen
        u32 eid = get_blip_entity_id(L);
        i32 army = lua_isnumber(L, 2) ? static_cast<i32>(lua_tonumber(L, 2)) - 1
                                      : -1;
        auto* sim = get_sim(L);
        if (sim && army >= 0) {
            auto* snap = sim->get_blip_snapshot(eid, static_cast<u32>(army));
            lua_pushboolean(L, snap ? 1 : 0);
        } else {
            lua_pushboolean(L, 0);
        }
        return 1;
    }
    i32 army = lua_isnumber(L, 2) ? static_cast<i32>(lua_tonumber(L, 2)) - 1
                                  : -1;
    auto* sim = get_sim(L);
    if (sim && sim->visibility_grid() && army >= 0) {
        auto& pos = e->position();
        lua_pushboolean(
            L, sim->visibility_grid()->ever_seen(pos.x, pos.z,
                                                 static_cast<u32>(army)) ? 1 : 0);
    } else {
        lua_pushboolean(L, 0);
    }
    return 1;
}

static int blip_IsMaybeDead(lua_State* L) {
    auto* e = check_blip_entity(L);
    if (!e || e->destroyed()) {
        lua_pushboolean(L, 1); // entity dead → definitely maybe dead
        return 1;
    }
    // Entity alive — check if requesting army has current intel
    i32 army = lua_isnumber(L, 2) ? static_cast<i32>(lua_tonumber(L, 2)) - 1
                                  : -1;
    if (army >= 0 && army == e->army()) {
        lua_pushboolean(L, 0); // own army always knows
        return 1;
    }
    auto* sim = get_sim(L);
    if (sim && army >= 0) {
        bool has_intel = sim->has_any_intel(e, static_cast<u32>(army));
        lua_pushboolean(L, has_intel ? 0 : 1);
    } else {
        lua_pushboolean(L, 1);
    }
    return 1;
}

static int blip_IsKnownFake(lua_State* L) {
    auto* e = check_blip_entity(L);
    if (!e || e->destroyed() || !e->is_unit()) {
        lua_pushboolean(L, 0);
        return 1;
    }
    // A unit is "known fake" if it has Jammer intel enabled AND the
    // requesting army has Omni coverage at the unit's position
    auto* unit = static_cast<sim::Unit*>(e);
    if (!unit->is_intel_enabled("Jammer")) {
        lua_pushboolean(L, 0);
        return 1;
    }
    i32 army = lua_isnumber(L, 2) ? static_cast<i32>(lua_tonumber(L, 2)) - 1
                                  : -1;
    auto* sim = get_sim(L);
    if (sim && sim->visibility_grid() && army >= 0) {
        auto& pos = e->position();
        lua_pushboolean(
            L, sim->visibility_grid()->has_omni(pos.x, pos.z,
                                                static_cast<u32>(army)) ? 1 : 0);
    } else {
        lua_pushboolean(L, 0);
    }
    return 1;
}

static int blip_GetSource(lua_State* L) {
    auto* e = check_blip_entity(L);
    if (!e || e->destroyed() || e->lua_table_ref() < 0) {
        lua_pushnil(L);
        return 1;
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, e->lua_table_ref());
    return 1;
}

static int blip_GetBlueprint(lua_State* L) {
    auto* e = check_blip_entity(L);
    std::string bp_id;
    if (e && !e->destroyed()) {
        bp_id = e->blueprint_id();
    } else {
        // Entity destroyed — use blip cache
        u32 eid = get_blip_entity_id(L);
        i32 req_army = get_blip_req_army(L);
        auto* sim = get_sim(L);
        if (sim && req_army >= 0) {
            auto* snap = sim->get_blip_snapshot(eid,
                                                 static_cast<u32>(req_army));
            if (snap) bp_id = snap->blueprint_id;
        }
    }
    if (bp_id.empty()) { lua_pushnil(L); return 1; }
    lua_pushstring(L, "__blueprints");
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (lua_istable(L, -1)) {
        lua_pushstring(L, bp_id.c_str());
        lua_rawget(L, -2);
        lua_remove(L, -2); // remove __blueprints table
    } else {
        lua_pop(L, 1);
        lua_pushnil(L);
    }
    return 1;
}

static int blip_GetArmy(lua_State* L) {
    auto* e = check_blip_entity(L);
    if (e && !e->destroyed()) {
        lua_pushnumber(L, e->army() >= 0 ? e->army() + 1 : -1);
        return 1;
    }
    // Entity destroyed — use blip cache
    u32 eid = get_blip_entity_id(L);
    i32 req_army = get_blip_req_army(L);
    auto* sim = get_sim(L);
    if (sim && req_army >= 0) {
        auto* snap = sim->get_blip_snapshot(eid, static_cast<u32>(req_army));
        if (snap && snap->entity_army >= 0) {
            lua_pushnumber(L, snap->entity_army + 1);
            return 1;
        }
    }
    lua_pushnumber(L, -1);
    return 1;
}

static int blip_GetAIBrain(lua_State* L) {
    auto* e = check_blip_entity(L);
    i32 army = -1;
    if (e && !e->destroyed()) {
        army = e->army();
    } else {
        u32 eid = get_blip_entity_id(L);
        i32 req_army = get_blip_req_army(L);
        auto* sim = get_sim(L);
        if (sim && req_army >= 0) {
            auto* snap = sim->get_blip_snapshot(eid,
                                                 static_cast<u32>(req_army));
            if (snap) army = snap->entity_army;
        }
    }
    if (army < 0) { lua_pushnil(L); return 1; }
    auto* sim = get_sim(L);
    if (!sim) { lua_pushnil(L); return 1; }
    auto* brain = sim->get_army(army);
    if (!brain || brain->lua_table_ref() < 0) { lua_pushnil(L); return 1; }
    lua_rawgeti(L, LUA_REGISTRYINDEX, brain->lua_table_ref());
    return 1;
}

static int blip_BeenDestroyed(lua_State* L) {
    auto* e = check_blip_entity(L);
    if (e && !e->destroyed()) {
        lua_pushboolean(L, 0);
        return 1;
    }
    // Entity pointer gone — check blip cache
    u32 eid = get_blip_entity_id(L);
    i32 req_army = get_blip_req_army(L);
    auto* sim = get_sim(L);
    if (sim && req_army >= 0) {
        auto* snap = sim->get_blip_snapshot(eid, static_cast<u32>(req_army));
        lua_pushboolean(L, (snap && snap->entity_dead) ? 1 : 0);
    } else {
        lua_pushboolean(L, 1); // no data → assume dead
    }
    return 1;
}

static const MethodEntry blip_methods[] = {
    {"GetSource",        blip_GetSource},
    {"IsOnRadar",        blip_IsOnRadar},
    {"IsOnSonar",        blip_IsOnSonar},
    {"IsOnOmni",         blip_IsOnOmni},
    {"IsSeenEver",       blip_IsSeenEver},
    {"IsSeenNow",        blip_IsSeenNow},
    {"GetBlueprint",     blip_GetBlueprint},
    {"GetPosition",      blip_GetPosition},
    {"GetArmy",          blip_GetArmy},
    {"GetAIBrain",       blip_GetAIBrain},
    {"BeenDestroyed",    blip_BeenDestroyed},
    {"IsKnownFake",      blip_IsKnownFake},
    {"IsMaybeDead",      blip_IsMaybeDead},
    {nullptr, nullptr},
};

// ---- M51: platoon bindings ----

// platoon:SetPlatoonFormationOverride(formation)
static int platoon_SetPlatoonFormationOverride(lua_State* L) {
    auto* p = check_platoon(L);
    if (!p) return 0;
    const char* f = (lua_type(L, 2) == LUA_TSTRING) ? lua_tostring(L, 2) : "";
    p->set_formation_override(f);
    return 0;
}

// platoon:IsOpponentAIRunning() — true if any non-defeated, non-civilian enemy brain exists
static int platoon_IsOpponentAIRunning(lua_State* L) {
    auto* p = check_platoon(L);
    auto* sim = get_sim(L);
    if (!p || !sim) { lua_pushboolean(L, 0); return 1; }
    i32 my_army = p->army_index();
    for (size_t i = 0; i < sim->army_count(); ++i) {
        if (static_cast<i32>(i) == my_army) continue;
        auto* brain = sim->get_army(static_cast<i32>(i));
        if (!brain) continue;
        if (brain->is_civilian()) continue;
        if (brain->is_defeated()) continue;
        lua_pushboolean(L, 1);
        return 1;
    }
    lua_pushboolean(L, 0);
    return 1;
}

// platoon:SetPrioritizedTargetList(category, table)
static int platoon_SetPrioritizedTargetList(lua_State* L) {
    auto* p = check_platoon(L);
    if (!p) return 0;
    // Free old ref if any
    if (p->priority_targets_ref() >= 0)
        luaL_unref(L, LUA_REGISTRYINDEX, p->priority_targets_ref());
    // arg2 = category string (e.g. "Attack"), arg3 = table of categories
    // Some FA code passes (self, string, table), some (self, table)
    int tbl_idx = lua_istable(L, 3) ? 3 : (lua_istable(L, 2) ? 2 : 0);
    if (tbl_idx > 0) {
        lua_pushvalue(L, tbl_idx);
        p->set_priority_targets_ref(luaL_ref(L, LUA_REGISTRYINDEX));
    } else {
        p->set_priority_targets_ref(-2);
    }
    return 0;
}

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
    {"SetPlatoonFormationOverride", platoon_SetPlatoonFormationOverride},
    {"Stop",                        platoon_Stop},
    {"MoveToLocation",              platoon_MoveToLocation},
    {"MoveToTarget",                platoon_MoveToLocation},
    {"Patrol",                      platoon_Patrol},
    {"AggressiveMoveToLocation",    platoon_MoveToLocation},
    {"AttackTarget",                platoon_AttackTarget},
    {"GuardTarget",                 platoon_GuardTarget},
    {"IsOpponentAIRunning",         platoon_IsOpponentAIRunning},
    {"FindClosestUnit",             platoon_FindClosestUnit},
    {"FindPrioritizedUnit",         platoon_FindPrioritizedUnit},
    {"SetPrioritizedTargetList",    platoon_SetPrioritizedTargetList},
    {"IsCommandsActive",            platoon_IsCommandsActive},
    {"CalculatePlatoonThreat",      platoon_CalculatePlatoonThreat},
    {nullptr, nullptr},
};

// ====================================================================
// Manipulator method implementations
// ====================================================================

/// Extract Manipulator* from self table's _c_object.
static sim::Manipulator* check_manip_base(lua_State* L) {
    if (!lua_istable(L, 1)) return nullptr;
    lua_pushstring(L, "_c_object");
    lua_rawget(L, 1);
    auto* m = lua_isuserdata(L, -1)
                  ? static_cast<sim::Manipulator*>(lua_touserdata(L, -1))
                  : nullptr;
    lua_pop(L, 1);
    return (m && !m->is_destroyed()) ? m : nullptr;
}

// --- Base manipulator methods ---

static int manip_Destroy(lua_State* L) {
    if (!lua_istable(L, 1)) return 0;
    lua_pushstring(L, "_c_object");
    lua_rawget(L, 1);
    auto* m = lua_isuserdata(L, -1)
                  ? static_cast<sim::Manipulator*>(lua_touserdata(L, -1))
                  : nullptr;
    lua_pop(L, 1);
    if (m) {
        // If a thread is WaitFor-ing on this manipulator, wake it so it
        // doesn't sleep forever at INT32_MAX.
        int waiter = m->waiting_thread_ref();
        m->set_waiting_thread_ref(-2);
        m->mark_destroyed();

        if (waiter >= 0) {
            lua_pushstring(L, "osc_thread_mgr");
            lua_rawget(L, LUA_REGISTRYINDEX);
            auto* mgr = lua_isuserdata(L, -1)
                ? static_cast<sim::ThreadManager*>(lua_touserdata(L, -1))
                : nullptr;
            lua_pop(L, 1);
            if (mgr) {
                lua_pushstring(L, "osc_sim_state");
                lua_rawget(L, LUA_REGISTRYINDEX);
                auto* ss = lua_isuserdata(L, -1)
                    ? static_cast<sim::SimState*>(lua_touserdata(L, -1))
                    : nullptr;
                lua_pop(L, 1);
                u32 tick = ss ? ss->tick_count() : 0;
                mgr->wake_thread(waiter, tick);
            }
        }
    }
    return 0;
}

static int manip_Enable(lua_State* L) {
    auto* m = check_manip_base(L);
    if (m) m->set_enabled(true);
    return 0;
}

static int manip_Disable(lua_State* L) {
    auto* m = check_manip_base(L);
    if (m) m->set_enabled(false);
    return 0;
}

static int manip_SetEnabled(lua_State* L) {
    auto* m = check_manip_base(L);
    if (m) m->set_enabled(lua_toboolean(L, 2) != 0);
    return 0;
}

static int manip_SetPrecedence(lua_State* L) {
    auto* m = check_manip_base(L);
    if (m) m->set_precedence(static_cast<i32>(lua_tonumber(L, 2)));
    return 0;
}

static int manip_IsEnabled(lua_State* L) {
    auto* m = check_manip_base(L);
    lua_pushboolean(L, m && m->enabled() ? 1 : 0);
    return 1;
}

// --- RotateManipulator methods ---

static int rotate_SetGoal(lua_State* L) {
    auto* m = check_manip_base(L);
    if (m) {
        static_cast<sim::RotateManipulator*>(m)->set_goal(
            static_cast<f32>(lua_tonumber(L, 2)));
    }
    lua_pushvalue(L, 1);
    return 1;
}

static int rotate_SetSpeed(lua_State* L) {
    auto* m = check_manip_base(L);
    if (m) {
        static_cast<sim::RotateManipulator*>(m)->set_speed(
            static_cast<f32>(lua_tonumber(L, 2)));
    }
    lua_pushvalue(L, 1);
    return 1;
}

static int rotate_SetAccel(lua_State* L) {
    auto* m = check_manip_base(L);
    if (m) {
        static_cast<sim::RotateManipulator*>(m)->set_accel(
            static_cast<f32>(lua_tonumber(L, 2)));
    }
    lua_pushvalue(L, 1);
    return 1;
}

static int rotate_SetCurrentAngle(lua_State* L) {
    auto* m = check_manip_base(L);
    if (m) {
        static_cast<sim::RotateManipulator*>(m)->set_current_angle(
            static_cast<f32>(lua_tonumber(L, 2)));
    }
    return 0;
}

static int rotate_GetCurrentAngle(lua_State* L) {
    auto* m = check_manip_base(L);
    if (m) {
        lua_pushnumber(L, static_cast<sim::RotateManipulator*>(m)->current_angle());
    } else {
        lua_pushnumber(L, 0);
    }
    return 1;
}

static int rotate_SetSpinDown(lua_State* L) {
    auto* m = check_manip_base(L);
    if (m) {
        static_cast<sim::RotateManipulator*>(m)->set_spin_down(
            lua_toboolean(L, 2) != 0);
    }
    return 0;
}

static int rotate_SetTargetSpeed(lua_State* L) {
    auto* m = check_manip_base(L);
    if (m) {
        static_cast<sim::RotateManipulator*>(m)->set_target_speed(
            static_cast<f32>(lua_tonumber(L, 2)));
    }
    return 0;
}

static int rotate_ClearGoal(lua_State* L) {
    auto* m = check_manip_base(L);
    if (m) {
        static_cast<sim::RotateManipulator*>(m)->clear_goal();
    }
    return 0;
}

// --- AnimationManipulator methods ---

static int anim_PlayAnim(lua_State* L) {
    auto* m = check_manip_base(L);
    if (m) {
        const char* path = lua_type(L, 2) == LUA_TSTRING
                               ? lua_tostring(L, 2) : "";
        bool loop = lua_toboolean(L, 3) != 0;
        static_cast<sim::AnimManipulator*>(m)->play_anim(path, loop);
    }
    lua_pushvalue(L, 1); // return self for chaining
    return 1;
}

static int anim_SetRate(lua_State* L) {
    auto* m = check_manip_base(L);
    if (m) {
        static_cast<sim::AnimManipulator*>(m)->set_rate(
            static_cast<f32>(lua_tonumber(L, 2)));
    }
    lua_pushvalue(L, 1);
    return 1;
}

static int anim_SetAnimationFraction(lua_State* L) {
    auto* m = check_manip_base(L);
    if (m) {
        static_cast<sim::AnimManipulator*>(m)->set_animation_fraction(
            static_cast<f32>(lua_tonumber(L, 2)));
    }
    return 0;
}

static int anim_GetAnimationFraction(lua_State* L) {
    auto* m = check_manip_base(L);
    if (m) {
        lua_pushnumber(L, static_cast<sim::AnimManipulator*>(m)->animation_fraction());
    } else {
        lua_pushnumber(L, 0);
    }
    return 1;
}

static int anim_GetAnimationDuration(lua_State* L) {
    auto* m = check_manip_base(L);
    if (m) {
        lua_pushnumber(L, static_cast<sim::AnimManipulator*>(m)->animation_duration());
    } else {
        lua_pushnumber(L, 1);
    }
    return 1;
}

static int anim_GetAnimationTime(lua_State* L) {
    auto* m = check_manip_base(L);
    if (m) {
        lua_pushnumber(L, static_cast<sim::AnimManipulator*>(m)->animation_time());
    } else {
        lua_pushnumber(L, 0);
    }
    return 1;
}

static int anim_SetAnimationTime(lua_State* L) {
    auto* m = check_manip_base(L);
    if (m) {
        static_cast<sim::AnimManipulator*>(m)->set_animation_time(
            static_cast<f32>(lua_tonumber(L, 2)));
    }
    return 0;
}

// --- SlideManipulator methods ---

static int slide_SetGoal(lua_State* L) {
    auto* m = check_manip_base(L);
    if (m) {
        static_cast<sim::SlideManipulator*>(m)->set_goal(
            static_cast<f32>(lua_tonumber(L, 2)),
            static_cast<f32>(lua_tonumber(L, 3)),
            static_cast<f32>(lua_tonumber(L, 4)));
    }
    lua_pushvalue(L, 1);
    return 1;
}

static int slide_SetSpeed(lua_State* L) {
    auto* m = check_manip_base(L);
    if (m) {
        static_cast<sim::SlideManipulator*>(m)->set_speed(
            static_cast<f32>(lua_tonumber(L, 2)));
    }
    lua_pushvalue(L, 1);
    return 1;
}

static int slide_SetAccel(lua_State* L) {
    auto* m = check_manip_base(L);
    if (m) {
        static_cast<sim::SlideManipulator*>(m)->set_accel(
            static_cast<f32>(lua_tonumber(L, 2)));
    }
    lua_pushvalue(L, 1);
    return 1;
}

static int slide_SetWorldUnits(lua_State* L) {
    auto* m = check_manip_base(L);
    if (m) {
        static_cast<sim::SlideManipulator*>(m)->set_world_units(
            lua_toboolean(L, 2) != 0);
    }
    lua_pushvalue(L, 1);
    return 1;
}

// --- AimManipulator methods ---

static int aim_SetFiringArc(lua_State* L) {
    auto* m = check_manip_base(L);
    if (m) {
        static_cast<sim::AimManipulator*>(m)->set_firing_arc(
            static_cast<f32>(lua_tonumber(L, 2)),
            static_cast<f32>(lua_tonumber(L, 3)),
            static_cast<f32>(lua_tonumber(L, 4)),
            static_cast<f32>(lua_tonumber(L, 5)),
            static_cast<f32>(lua_tonumber(L, 6)),
            static_cast<f32>(lua_tonumber(L, 7)));
    }
    return 0;
}

static int aim_SetHeadingPitch(lua_State* L) {
    auto* m = check_manip_base(L);
    if (m) {
        static_cast<sim::AimManipulator*>(m)->set_heading_pitch(
            static_cast<f32>(lua_tonumber(L, 2)),
            static_cast<f32>(lua_tonumber(L, 3)));
    }
    return 0;
}

static int aim_GetHeadingPitch(lua_State* L) {
    auto* m = check_manip_base(L);
    if (m) {
        auto* aim = static_cast<sim::AimManipulator*>(m);
        lua_pushnumber(L, aim->heading());
        lua_pushnumber(L, aim->pitch());
    } else {
        lua_pushnumber(L, 0);
        lua_pushnumber(L, 0);
    }
    return 2;
}

static int aim_OnTarget(lua_State* L) {
    auto* m = check_manip_base(L);
    lua_pushboolean(L, m && static_cast<sim::AimManipulator*>(m)->on_target() ? 1 : 0);
    return 1;
}

static int aim_SetResetPoseTime(lua_State* L) {
    auto* m = check_manip_base(L);
    if (m) {
        static_cast<sim::AimManipulator*>(m)->set_reset_pose_time(
            static_cast<f32>(lua_tonumber(L, 2)));
    }
    return 0;
}

static int aim_SetAimHeadingOffset(lua_State* L) {
    auto* m = check_manip_base(L);
    if (m) {
        static_cast<sim::AimManipulator*>(m)->set_aim_heading_offset(
            static_cast<f32>(lua_tonumber(L, 2)));
    }
    return 0;
}

// --- Method tables ---

static const MethodEntry manipulator_methods[] = {
    {"Destroy",                 manip_Destroy},
    {"Enable",                  manip_Enable},
    {"Disable",                 manip_Disable},
    {"SetEnabled",              manip_SetEnabled},
    {"SetPrecedence",           manip_SetPrecedence},
    {"IsEnabled",               manip_IsEnabled},
    {nullptr, nullptr},
};

static const MethodEntry aim_manipulator_methods[] = {
    {"SetFiringArc",            aim_SetFiringArc},
    {"SetHeadingPitch",         aim_SetHeadingPitch},
    {"GetHeadingPitch",         aim_GetHeadingPitch},
    {"OnTarget",                aim_OnTarget},
    {"SetEnabled",              manip_SetEnabled},
    {"SetResetPoseTime",        aim_SetResetPoseTime},
    {"SetAimHeadingOffset",     aim_SetAimHeadingOffset},
    {nullptr, nullptr},
};

static const MethodEntry animation_manipulator_methods[] = {
    {"PlayAnim",                anim_PlayAnim},
    {"SetRate",                 anim_SetRate},
    {"SetAnimationFraction",    anim_SetAnimationFraction},
    {"GetAnimationFraction",    anim_GetAnimationFraction},
    {"GetAnimationDuration",    anim_GetAnimationDuration},
    {"GetAnimationTime",        anim_GetAnimationTime},
    {"SetAnimationTime",        anim_SetAnimationTime},
    {"SetBoneEnabled",          stub_noop},
    {nullptr, nullptr},
};

static const MethodEntry rotate_manipulator_methods[] = {
    {"SetGoal",                 rotate_SetGoal},
    {"SetSpeed",                rotate_SetSpeed},
    {"SetAccel",                rotate_SetAccel},
    {"SetCurrentAngle",         rotate_SetCurrentAngle},
    {"GetCurrentAngle",         rotate_GetCurrentAngle},
    {"SetSpinDown",             rotate_SetSpinDown},
    {"SetTargetSpeed",          rotate_SetTargetSpeed},
    {"ClearGoal",               rotate_ClearGoal},
    {nullptr, nullptr},
};

static const MethodEntry slide_manipulator_methods[] = {
    {"SetGoal",                 slide_SetGoal},
    {"SetSpeed",                slide_SetSpeed},
    {"SetAccel",                slide_SetAccel},
    {"SetWorldUnits",           slide_SetWorldUnits},
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

static int nav_SetSpeedThroughGoal(lua_State* L) {
    auto* nav = check_navigator(L);
    if (nav) nav->set_speed_through_goal(lua_toboolean(L, 2) != 0);
    return 0;
}

// navigator
static const MethodEntry navigator_methods[] = {
    {"GetCurrentTargetSpeed",   nav_GetCurrentTargetSpeed},
    {"SetSpeedThroughGoal",     nav_SetSpeedThroughGoal},
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
