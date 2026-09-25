// The sim's entity bindings: moho.entity_methods, props' and entity sounds.
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

static int (*const stub_noop)(lua_State*) = lua_stubs::noop;

static int (*const stub_return_nil)(lua_State*) = lua_stubs::return_nil;

/// entity:PlaySound(sound) -- a one-shot at the entity
static int entity_PlaySound(lua_State* L) {
    auto* mgr = get_sound_mgr(L);
    if (!mgr) return 0;

    auto* e = check_entity(L);
    if (!e || e->destroyed()) return 0;

    std::string bank, cue, lod;
    if (!extract_sound_table(L, 2, bank, cue, &lod)) return 0;

    auto pos = e->position();
    mgr->play(bank, cue, &pos, lod);
    return 0;
}

/// entity:SetAmbientSound(detail, rumble) -- the entity's two ambient loop
/// slots; nil stops a slot.
static int entity_SetAmbientSound(lua_State* L) {
    auto* mgr = get_sound_mgr(L);
    auto* e = check_entity(L);
    if (!e || e->destroyed()) return 0;
    const char* slots[2] = {"__ambient", "__rumble"};
    for (int i = 0; i < 2; ++i) {
        stop_ambient(mgr, e, slots[i]);
        std::string bank, cue;
        if (mgr && extract_sound_table(L, 2 + i, bank, cue)) {
            auto pos = e->position();
            e->set_ambient_sound(slots[i], mgr->play_loop(bank, cue, &pos));
        }
    }
    return 0;
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

int entity_IsValidBone(lua_State* L) {
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

int entity_GetBoneDirection(lua_State* L) {
    auto* e = check_entity(L);
    if (!e) { push_vector3(L, {0, 0, 1}); return 1; }
    auto* bd = e->bone_data();
    if (!bd || lua_gettop(L) < 2) {
        // No bone data or no bone arg: return entity forward direction
        auto fwd = sim::quat_rotate(e->orientation(), {0, 0, 1});
        push_vector3(L, fwd);
        return 1;
    }
    // The bone's forward (+Z) in the world: a unit's as posed (turrets,
    // rotators), anything else's in bind pose.
    i32 idx = resolve_bone_index(e, L, 2);
    const sim::Quaternion model_rot =
        e->is_unit() ? static_cast<const sim::Unit*>(e)->bone_pose(idx).rotation
                     : bd->bones[static_cast<size_t>(idx)].world_rotation;
    auto bone_world_rot = sim::quat_multiply(e->orientation(), model_rot);
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
    // A collision beam's end 1 is where its last check reached; end 0 its
    // muzzle (its position).
    if (e->is_collision_beam() && lua_type(L, 2) == LUA_TNUMBER && lua_tonumber(L, 2) == 1) {
        push_vector3(L, e->beam_endpoint());
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
int entity_GetHeading(lua_State* L) {
    auto* e = check_entity(L);
    if (!e) { lua_pushnumber(L, 0); return 1; }
    lua_pushnumber(L, sim::quat_yaw(e->orientation()));
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
        // SetPosition(pos, immediate): an immediate move is a teleport, which
        // the renderer shows as a jump, not a slide.
        if (lua_toboolean(L, 3)) e->note_snap();
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
    // The vector metatable, as Moho's quaternions carry it: FAF multiplies
    // them (EulerToQuaternion(...) * unit:GetOrientation()).
    push_vector_metatable(L);
    lua_setmetatable(L, -2);
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
    if (!push_entity_blueprint(L, check_entity(L))) lua_pushnil(L);
    return 1;
}

static int entity_GetFractionComplete(lua_State* L) {
    auto* e = check_entity(L);
    lua_pushnumber(L, e ? e->fraction_complete() : 1.0);
    return 1;
}

// A unit's death, counted once: the loss for its army (Moho's Units_Killed),
// veterancy for those that damaged it, and the kill for the army that did
// the most. Kill counts it when the unit dies; Destroy, for a unit removed
// without being killed.
static void record_unit_death(lua_State* L, sim::Unit* dying_unit) {
    auto* sim_ptr = get_sim(L);

    // A loss for its army (Moho's Units_Killed)
    if (sim_ptr) {
        auto* victim_brain = sim_ptr->get_army(dying_unit->army());
        if (victim_brain) {
            victim_brain->record_unit_lost(dying_unit->blueprint_id(),
                                           dying_unit->build_cost_mass(),
                                           dying_unit->build_cost_energy());
        }
    }

    // Distribute veterancy XP to attackers
    f32 xp_value = dying_unit->xp_value();
    if (xp_value > 0 && !dying_unit->damage_contributions().empty()) {
        f32 total_damage = 0;
        for (const auto& [aid, dmg] : dying_unit->damage_contributions()) {
            total_damage += dmg;
        }
        if (total_damage > 0 && sim_ptr) {
            for (const auto& [aid, dmg] : dying_unit->damage_contributions()) {
                auto* attacker = sim_ptr->entity_registry().find(aid);
                if (attacker && !attacker->destroyed() && attacker->is_unit()) {
                    f32 xp_share = xp_value * (dmg / total_damage);
                    static_cast<sim::Unit*>(attacker)->add_xp(xp_share, L,
                                                              sim_ptr->entity_registry());
                }
            }
        }
    }

    // Credit kill to army that dealt the most damage
    if (sim_ptr && !dying_unit->damage_contributions().empty()) {
        i32 victim_army = dying_unit->army();
        i32 killer_army = -1;
        f32 max_dmg = 0;
        for (const auto& [attacker_id, dmg] : dying_unit->damage_contributions()) {
            if (dmg > max_dmg) {
                auto* attacker = sim_ptr->entity_registry().find(attacker_id);
                if (attacker && !attacker->destroyed()) {
                    max_dmg = dmg;
                    killer_army = attacker->army();
                }
            }
        }
        if (killer_army >= 0 && killer_army != victim_army) {
            auto* killer_brain = sim_ptr->get_army(killer_army);
            if (killer_brain) {
                killer_brain->record_enemy_killed(
                    dying_unit->blueprint_id(), dying_unit->build_cost_mass(),
                    dying_unit->build_cost_energy(), dying_unit->has_category("COMMAND"));
            }
        }
    }

    dying_unit->clear_damage_contributions();
}

int entity_Destroy(lua_State* L) {
    auto* e = check_entity(L);
    if (e && e->destroyed()) return 0; // re-entry from its own OnDestroy
    if (e) {
        // Its ambient loops end with it.
        stop_ambient(get_sound_mgr(L), e, nullptr);

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

        // Guard against recursive destruction from OnNotAdjacentTo callbacks
        if (e->destroyed()) return 0;

        // A unit destroyed without being killed is lost all the same (a
        // killed one was counted when it died).
        if (e->is_unit() && !static_cast<sim::Unit*>(e)->is_dying())
            record_unit_death(L, static_cast<sim::Unit*>(e));

        u32 id = e->entity_id();
        int lua_ref = e->lua_table_ref();

        // Fire death event for renderer explosion VFX (units only)
        if (e->is_unit()) {
            auto pos = e->position();
            f32 scale = 1.0f;
            if (e->footprint_size_x() > 0) scale = e->footprint_size_x() * 0.5f;
            auto* sim = get_sim(L);
            if (sim)
                sim->add_death_event(pos.x, pos.y, pos.z, scale, e->army());
        }

        // If dying unit was capturing, clear being_captured on its target
        if (e->is_unit()) {
            auto* dying_unit = static_cast<sim::Unit*>(e);
            if (dying_unit->is_capturing()) {
                auto* sim = get_sim(L);
                if (sim)
                    dying_unit->stop_capturing(L, sim->entity_registry(), true);
            }
        }

        // Moho calls the script's OnDestroy while the object is still live.
        if (auto* sim = get_sim(L)) {
            sim->notify_script_destroy(*e);
            if (e->destroyed()) return 0; // its OnDestroy finished the job
        }

        e->mark_destroyed();

        // If this is a unit, detach its weapons' Lua tables before freeing
        if (e->is_unit()) static_cast<sim::Unit*>(e)->release_weapon_scripts(L);

        // Null out _c_object in the Lua table to prevent use-after-free
        lua_pushstring(L, "_c_object");
        lua_pushlightuserdata(L, nullptr);
        lua_rawset(L, 1);

        // Release Lua registry ref before freeing the C++ object (and say so,
        // so SimState's unregister hook does not release it a second time).
        if (lua_ref >= 0) {
            luaL_unref(L, LUA_REGISTRYINDEX, lua_ref);
            e->set_lua_table_ref(LUA_NOREF);
        }

        auto* sim = get_sim(L);
        if (sim) sim->entity_registry().unregister_entity(id);
    }
    return 0;
}

// entity:Kill([instigator, damageType, excessDamageRatio]). Moho hands the
// death to the script's OnKilled, which plays the death sequence (death
// weapon, animation, wreckage) and calls Destroy() when it ends. A unit is
// dead from here on (IsDead; no orders, weapons or economy; one in flight
// falls) and its death is counted now. Without an OnKilled the entity is
// destroyed at once. SetCanBeKilled(false) blocks it.
int entity_Kill(lua_State* L) {
    auto* e = check_entity(L);
    if (!e || e->destroyed() || e->script_owns_death()) return 0;
    if (e->is_unit()) {
        auto* u = static_cast<sim::Unit*>(e);
        if (!u->can_be_killed() || u->is_dying()) return 0;
        lua_pushstring(L, "CanBeKilled"); // SetCanBeKilled sets this Lua field
        lua_rawget(L, 1);
        const bool blocked = lua_isboolean(L, -1) && !lua_toboolean(L, -1);
        lua_pop(L, 1);
        if (blocked) return 0;
    }
    lua_settop(L, 4); // self, instigator, damageType, excessDamageRatio
    // Moho reads these into typed values before calling the script, so a
    // bare Kill() reaches OnKilled as (nil, 'Normal', 0): a whole wreck,
    // where a nil ratio would leave CreateWreckageProp a worthless one.
    if (lua_type(L, 3) != LUA_TSTRING) {
        lua_pushstring(L, "Normal");
        lua_replace(L, 3);
    }
    if (!lua_isnumber(L, 4)) {
        lua_pushnumber(L, 0);
        lua_replace(L, 4);
    }
    lua_pushstring(L, "OnKilled");
    lua_gettable(L, 1);
    if (!lua_isfunction(L, -1)) {
        lua_settop(L, 1);
        return entity_Destroy(L);
    }
    e->set_script_owns_death();
    if (e->is_unit()) {
        auto* u = static_cast<sim::Unit*>(e);
        record_unit_death(L, u);
        // Recording may run scripts (veterancy): re-validate.
        e = check_entity(L);
        if (!e || e->destroyed()) return 0;
        static_cast<sim::Unit*>(e)->begin_dying();
    }
    // Stack: self, instigator, type, ratio, OnKilled -> call OnKilled(self, ...)
    lua_insert(L, 1);
    lua_pushvalue(L, 2);
    lua_insert(L, 1); // self (kept), OnKilled, self, instigator, type, ratio
    if (lua_pcall(L, 4, 0, 0) != 0) {
        // The death sequence broke before it could Destroy() the unit (e.g.
        // a failing death weapon); finish the job rather than leave it
        // half-dead forever.
        const char* err = lua_tostring(L, -1);
        const std::string message = std::string("OnKilled error: ") + (err ? err : "(unknown)");
        spdlog::warn("{}", message);
        if (test_status::count_lua_failures()) test_status::record_failure(message);
        lua_settop(L, 1);
        if (auto* still = check_entity(L); still && !still->destroyed())
            return entity_Destroy(L);
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
// Build-related method implementations
// ====================================================================

static int entity_SetFractionComplete(lua_State* L) {
    auto* e = check_entity(L);
    if (e) e->set_fraction_complete(static_cast<f32>(lua_tonumber(L, 2)));
    return 0;
}

// entity:CreateProjectile(bp, offX, offY, offZ, dirX, dirY, dirZ): a
// projectile at the entity's centre plus the offset (each may be nil),
// launched by it. A direction points it, at its blueprint's InitialSpeed.
static int entity_CreateProjectile(lua_State* L) {
    auto* e = check_entity(L);
    if (!e || e->destroyed()) { lua_pushnil(L); return 1; }

    auto* sim = get_sim(L);
    if (!sim) { lua_pushnil(L); return 1; }

    auto proj = std::make_unique<sim::Projectile>();
    proj->set_blueprint_id(lowercase_arg(L, 2));
    const sim::Vector3 at = e->position();
    proj->set_position({at.x + static_cast<f32>(luaL_optnumber(L, 3, 0)),
                        at.y + static_cast<f32>(luaL_optnumber(L, 4, 0)),
                        at.z + static_cast<f32>(luaL_optnumber(L, 5, 0))});
    proj->set_army(e->army());
    proj->launcher_id = e->entity_id();
    apply_script_projectile_physics(L, *proj);
    if (lua_isnumber(L, 6) && lua_isnumber(L, 7) && lua_isnumber(L, 8)) {
        aim_projectile(L, sim, *proj,
                       {static_cast<f32>(lua_tonumber(L, 6)), static_cast<f32>(lua_tonumber(L, 7)),
                        static_cast<f32>(lua_tonumber(L, 8))});
    }

    u32 proj_id = sim->entity_registry().register_entity(std::move(proj));
    auto* proj_ptr = static_cast<sim::Projectile*>(
        sim->entity_registry().find(proj_id));

    // Its script object, as for every projectile (OnCreate runs).
    sim::create_projectile_object(L, *proj_ptr, under_water(sim, proj_ptr->position()), true);
    return 1;
}

// entity:CreateProjectileAtBone(bp, bone): a projectile at the bone, facing
// as it does (a unit's as posed), launched along it at its blueprint's
// InitialSpeed. Retail tosses a destroyed unit's parts this way.
static int entity_CreateProjectileAtBone(lua_State* L) {
    auto* e = check_entity(L);
    if (!e || e->destroyed()) { lua_pushnil(L); return 1; }

    auto* sim = get_sim(L);
    if (!sim) { lua_pushnil(L); return 1; }

    const i32 bone_idx = resolve_bone_index(e, L, 3);
    auto proj = std::make_unique<sim::Projectile>();
    proj->set_blueprint_id(lowercase_arg(L, 2));
    proj->set_position(bone_world_position(e, bone_idx));
    proj->set_army(e->army());
    proj->launcher_id = e->entity_id();
    apply_script_projectile_physics(L, *proj);
    const auto* bd = e->bone_data();
    const sim::Quaternion model_rot =
        e->is_unit() ? static_cast<const sim::Unit*>(e)->bone_pose(bone_idx).rotation
        : bd && bd->is_valid(bone_idx) ? bd->bones[static_cast<size_t>(bone_idx)].world_rotation
                                       : sim::Quaternion{};
    aim_projectile(L, sim, *proj,
                   sim::quat_rotate(sim::quat_multiply(e->orientation(), model_rot), {0, 0, 1}));

    u32 proj_id = sim->entity_registry().register_entity(std::move(proj));
    auto* proj_ptr = static_cast<sim::Projectile*>(
        sim->entity_registry().find(proj_id));

    // Its script object, as for every projectile (OnCreate runs).
    sim::create_projectile_object(L, *proj_ptr, under_water(sim, proj_ptr->position()), true);
    return 1;
}

// unit:SetCustomName(name). The UI's (UserUnit's: the rename dialog, the
// commander named for its player) goes to the sim as a ProcessInfo pair, as
// Moho's does, so every lockstep peer and a replay name the unit at the same
// tick. Only the UI state has a callback queue; the sim's own scripts name
// the unit at once.
static int entity_SetCustomName(lua_State* L) {
    auto* e = check_entity(L);
    if (!e || lua_type(L, 2) != LUA_TSTRING) return 0;
    if (auto* queue = get_callback_queue(L)) {
        sim::SimCallbackEntry entry;
        entry.func_name = sim::kProcessInfoCallback;
        entry.args["Action"] = std::string("CustomName");
        entry.args["Value"] = std::string(lua_tostring(L, 2));
        entry.unit_ids.push_back(e->entity_id());
        queue->push(std::move(entry));
        return 0;
    }
    e->set_custom_name(lua_tostring(L, 2));
    return 0;
}

// ====================================================================
// M65: Stub conversions — visibility, scale, mesh, collision, attach, shake
// ====================================================================

static sim::VizMode parse_viz_mode(lua_State* L, int arg) {
    if (lua_type(L, arg) != LUA_TSTRING) return sim::VizMode::INTEL;
    const char* s = lua_tostring(L, arg);
    if (std::strcmp(s, "Always") == 0) return sim::VizMode::ALWAYS;
    if (std::strcmp(s, "Never") == 0) return sim::VizMode::NEVER;
    return sim::VizMode::INTEL; // default and 'Intel'
}

static int entity_SetVizToAllies(lua_State* L) {
    auto* e = check_entity(L); if (!e) return 0;
    e->set_viz_allies(parse_viz_mode(L, 2));
    return 0;
}
static int entity_SetVizToEnemies(lua_State* L) {
    auto* e = check_entity(L); if (!e) return 0;
    e->set_viz_enemies(parse_viz_mode(L, 2));
    return 0;
}
static int entity_SetVizToFocusPlayer(lua_State* L) {
    auto* e = check_entity(L); if (!e) return 0;
    e->set_viz_focus_player(parse_viz_mode(L, 2));
    return 0;
}
static int entity_SetVizToNeutrals(lua_State* L) {
    auto* e = check_entity(L); if (!e) return 0;
    e->set_viz_neutrals(parse_viz_mode(L, 2));
    return 0;
}

static int entity_SetScale(lua_State* L) {
    auto* e = check_entity(L); if (!e) return 0;
    auto s = static_cast<f32>(luaL_checknumber(L, 2));
    e->set_scale(s, s, s);
    return 0;
}

int entity_SetMesh(lua_State* L) {
    auto* e = check_entity(L); if (!e) return 0;
    if (lua_type(L, 2) == LUA_TSTRING)
        e->set_mesh_override(lua_tostring(L, 2));
    else
        e->set_mesh_override(""); // clear override
    return 0;
}

int entity_SetCollisionShape(lua_State* L) {
    auto* e = check_entity(L); if (!e) return 0;
    sim::CollisionShape shape;
    if (lua_type(L, 2) == LUA_TSTRING) {
        const char* type = lua_tostring(L, 2);
        if (std::strcmp(type, "Sphere") == 0) {
            shape.type = sim::CollisionShapeType::SPHERE;
            shape.cx = static_cast<f32>(luaL_optnumber(L, 3, 0));
            shape.cy = static_cast<f32>(luaL_optnumber(L, 4, 0));
            shape.cz = static_cast<f32>(luaL_optnumber(L, 5, 0));
            shape.sx = static_cast<f32>(luaL_optnumber(L, 6, 1)); // radius
        } else if (std::strcmp(type, "Box") == 0) {
            shape.type = sim::CollisionShapeType::BOX;
            shape.cx = static_cast<f32>(luaL_optnumber(L, 3, 0));
            shape.cy = static_cast<f32>(luaL_optnumber(L, 4, 0));
            shape.cz = static_cast<f32>(luaL_optnumber(L, 5, 0));
            shape.sx = static_cast<f32>(luaL_optnumber(L, 6, 1));
            shape.sy = static_cast<f32>(luaL_optnumber(L, 7, 1));
            shape.sz = static_cast<f32>(luaL_optnumber(L, 8, 1));
        }
        // else "None" → default NONE
    }
    e->set_collision_shape(shape);
    return 0;
}

int entity_RevertCollisionShape(lua_State* L) {
    auto* e = check_entity(L); if (!e) return 0;
    e->revert_collision_shape(); // back to its blueprint's
    return 0;
}

static int entity_SetParentOffset(lua_State* L) {
    auto* e = check_entity(L); if (!e) return 0;
    sim::Vector3 off;
    if (lua_istable(L, 2)) {
        lua_pushnumber(L, 1); lua_rawget(L, 2);
        off.x = static_cast<f32>(lua_tonumber(L, -1)); lua_pop(L, 1);
        lua_pushnumber(L, 2); lua_rawget(L, 2);
        off.y = static_cast<f32>(lua_tonumber(L, -1)); lua_pop(L, 1);
        lua_pushnumber(L, 3); lua_rawget(L, 2);
        off.z = static_cast<f32>(lua_tonumber(L, -1)); lua_pop(L, 1);
    } else {
        off.x = static_cast<f32>(luaL_optnumber(L, 2, 0));
        off.y = static_cast<f32>(luaL_optnumber(L, 3, 0));
        off.z = static_cast<f32>(luaL_optnumber(L, 4, 0));
    }
    e->set_parent_offset(off);
    return 0;
}

/// Helper: extract an Entity* from a Lua table at the given stack index.
static sim::Entity* check_entity_arg(lua_State* L, int idx) {
    if (idx < 0) idx = lua_gettop(L) + idx + 1;
    return check_entity(L, idx);
}

static int entity_AttachTo(lua_State* L) {
    auto* self = check_entity(L); if (!self) return 0;
    auto* parent = check_entity_arg(L, 2); if (!parent) return 0;
    i32 bone = resolve_bone_index(parent, L, 3);
    // Detach from current parent first
    if (self->parent_entity_id()) {
        auto* sim = get_sim(L);
        if (sim) {
            auto* old = sim->entity_registry().find(self->parent_entity_id());
            if (old) old->remove_child(self->entity_id());
        }
    }
    self->set_parent(parent->entity_id(), bone);
    parent->add_child(self->entity_id(), bone);
    return 0;
}

static int entity_AttachBoneTo(lua_State* L) {
    auto* self = check_entity(L); if (!self) return 0;
    i32 self_bone = resolve_bone_index(self, L, 2);
    auto* parent = check_entity_arg(L, 3); if (!parent) return 0;
    i32 parent_bone = resolve_bone_index(parent, L, 4);
    // Detach from current parent first
    if (self->parent_entity_id()) {
        auto* sim = get_sim(L);
        if (sim) {
            auto* old = sim->entity_registry().find(self->parent_entity_id());
            if (old) old->remove_child(self->entity_id());
        }
    }
    self->set_parent(parent->entity_id(), parent_bone, self_bone);
    parent->add_child(self->entity_id(), parent_bone);
    return 0;
}

static int entity_AttachBoneToEntityBone(lua_State* L) {
    // self:AttachBoneToEntityBone(targetEntity, sourceBone, offsetBone, reparent)
    auto* self = check_entity(L); if (!self) return 0;
    auto* target = check_entity_arg(L, 2); if (!target) return 0;
    i32 self_bone = resolve_bone_index(self, L, 3);
    // Detach target from current parent
    if (target->parent_entity_id()) {
        auto* sim = get_sim(L);
        if (sim) {
            auto* old = sim->entity_registry().find(target->parent_entity_id());
            if (old) old->remove_child(target->entity_id());
        }
    }
    target->set_parent(self->entity_id(), self_bone);
    self->add_child(target->entity_id(), self_bone);
    return 0;
}

static int entity_DetachFrom(lua_State* L) {
    auto* self = check_entity(L); if (!self) return 0;
    if (self->parent_entity_id()) {
        auto* sim = get_sim(L);
        if (sim) {
            auto* parent = sim->entity_registry().find(self->parent_entity_id());
            if (parent) parent->remove_child(self->entity_id());
        }
        self->clear_parent();
    }
    return 0;
}

static int entity_DetachAll(lua_State* L) {
    auto* self = check_entity(L); if (!self) return 0;
    i32 bone = -1;
    if (lua_type(L, 2) == LUA_TNUMBER)
        bone = static_cast<i32>(lua_tonumber(L, 2));
    else if (lua_type(L, 2) == LUA_TSTRING)
        bone = resolve_bone_index(self, L, 2);

    auto* sim = get_sim(L);
    if (!sim) return 0;

    // Snapshot children to avoid mutation during iteration
    auto children_copy = self->children();
    for (auto& c : children_copy) {
        if (bone >= 0 && c.bone != bone) continue;
        auto* child = sim->entity_registry().find(c.entity_id);
        if (child) child->clear_parent();
        self->remove_child(c.entity_id);
    }
    return 0;
}

static int entity_ShakeCamera(lua_State* L) {
    auto* e = check_entity(L); if (!e) return 0;
    auto* sim = get_sim(L); if (!sim) return 0;
    sim::CameraShakeEvent ev;
    ev.x = e->position().x;
    ev.z = e->position().z;
    ev.radius    = static_cast<f32>(luaL_optnumber(L, 2, 30));
    ev.max_shake = static_cast<f32>(luaL_optnumber(L, 3, 1));
    ev.min_shake = static_cast<f32>(luaL_optnumber(L, 4, 0));
    ev.duration  = static_cast<f32>(luaL_optnumber(L, 5, 0.5));
    sim->add_camera_shake(ev);
    return 0;
}

int entity_SetUnSelectable(lua_State* L) {
    auto* e = check_entity(L); if (!e) return 0;
    e->set_unselectable(lua_toboolean(L, 2) != 0);
    return 0;
}

// Intel on an entity that isn't a unit: retail's VizMarker reveals an area
// with InitIntel(army, type, radius) and EnableIntel. SimState paints it
// each tick while the entity lives. (Units have their own.)
static int entity_InitIntel(lua_State* L) {
    auto* e = check_entity(L);
    auto* sim = get_sim(L);
    if (!e || !sim || lua_type(L, 3) != LUA_TSTRING) return 0;
    auto& intel = sim->entity_intel(e->entity_id());
    intel.army = lua_isnumber(L, 2) ? static_cast<i32>(lua_tonumber(L, 2)) - 1 : e->army();
    intel.sources[lua_tostring(L, 3)] = {static_cast<f32>(luaL_optnumber(L, 4, 0)), true};
    return 0;
}

static sim::SimState::EntityIntel::Source* entity_intel_source(lua_State* L) {
    auto* e = check_entity(L);
    auto* sim = get_sim(L);
    if (!e || !sim || lua_type(L, 2) != LUA_TSTRING) return nullptr;
    if (!sim->find_entity_intel(e->entity_id())) return nullptr;
    auto& sources = sim->entity_intel(e->entity_id()).sources;
    const auto it = sources.find(lua_tostring(L, 2));
    return it != sources.end() ? &it->second : nullptr;
}

static int entity_EnableIntel(lua_State* L) {
    if (auto* s = entity_intel_source(L)) s->enabled = true;
    return 0;
}

static int entity_DisableIntel(lua_State* L) {
    if (auto* s = entity_intel_source(L)) s->enabled = false;
    return 0;
}

static int entity_SetIntelRadius(lua_State* L) {
    if (auto* s = entity_intel_source(L)) s->radius = static_cast<f32>(luaL_checknumber(L, 3));
    return 0;
}

static int entity_IsIntelEnabled(lua_State* L) {
    const auto* s = entity_intel_source(L);
    lua_pushboolean(L, s && s->enabled ? 1 : 0);
    return 1;
}

static int entity_GetIntelRadius(lua_State* L) {
    const auto* s = entity_intel_source(L);
    lua_pushnumber(L, s ? s->radius : 0);
    return 1;
}

// clang-format off
const MethodEntry entity_methods[] = {
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
    // Any entity: retail units loop their ambient sounds on attached
    // helper entities (Unit.PlayUnitAmbientSound).
    {"SetAmbientSound",     entity_SetAmbientSound},
    {"GetBoneCount",        entity_GetBoneCount},
    {"GetBoneName",         entity_GetBoneName},
    {"IsValidBone",         entity_IsValidBone},
    // M65: real implementations
    {"SetCollisionShape",       entity_SetCollisionShape},
    {"SetDrawScale",            entity_SetScale},
    {"SetMesh",                 entity_SetMesh},
    {"SetScale",                entity_SetScale},
    {"InitIntel",               entity_InitIntel},
    {"EnableIntel",             entity_EnableIntel},
    {"DisableIntel",            entity_DisableIntel},
    {"SetIntelRadius",          entity_SetIntelRadius},
    {"IsIntelEnabled",          entity_IsIntelEnabled},
    {"GetIntelRadius",          entity_GetIntelRadius},
    {"SetParentOffset",         entity_SetParentOffset},
    {"SetVizToAllies",          entity_SetVizToAllies},
    {"SetVizToEnemies",         entity_SetVizToEnemies},
    {"SetVizToFocusPlayer",     entity_SetVizToFocusPlayer},
    {"SetVizToNeutrals",        entity_SetVizToNeutrals},
    {"ShakeCamera",             entity_ShakeCamera},
    {"AttachBoneTo",            entity_AttachBoneTo},
    {"AttachBoneToEntityBone",  entity_AttachBoneToEntityBone},
    {"AttachTo",                entity_AttachTo},
    {"DetachFrom",              entity_DetachFrom},
    {"DetachAll",               entity_DetachAll},
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
// clang-format on

// --- Targeting / reclaimable flags ---

int entity_SetDoNotTarget(lua_State* L) {
    auto* e = check_entity(L);
    if (!e) return 0;
    bool val = lua_toboolean(L, 2) != 0;
    e->set_do_not_target(val);
    return 0;
}

int entity_SetReclaimable(lua_State* L) {
    auto* e = check_entity(L);
    if (!e) return 0;
    bool val = lua_toboolean(L, 2) != 0;
    e->set_reclaimable(val);
    return 0;
}

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

    // Mark entity as wreckage for visual distinction in renderer
    auto* e = check_entity(L);
    if (e) e->set_is_wreckage(true);

    return 0;
}

// prop:Kill(instigator, type, overkill): the prop's OnKilled decides
// (Prop.OnKilled destroys it; a tree's dies).
static int prop_Kill(lua_State* L) {
    auto* e = check_entity(L);
    if (!e || e->destroyed()) return 0;
    lua_settop(L, 4);
    lua_pushstring(L, "OnKilled");
    lua_gettable(L, 1);
    const bool scripted = lua_isfunction(L, -1);
    if (!scripted) {
        lua_pop(L, 1);
        lua_pushstring(L, "Destroy");
        lua_gettable(L, 1);
        if (!lua_isfunction(L, -1)) return 0;
    }
    lua_pushvalue(L, 1);
    int args = 1;
    if (scripted) {
        for (int i = 2; i <= 4; ++i) lua_pushvalue(L, i);
        args = 4;
    }
    if (lua_pcall(L, args, 0, 0) != 0) {
        const char* err = lua_tostring(L, -1);
        const std::string message = std::string("Prop Kill error: ") + (err ? err : "(unknown)");
        spdlog::warn("{}", message);
        if (test_status::count_lua_failures()) test_status::record_failure(message);
        lua_pop(L, 1);
    }
    return 0;
}

// prop:CreatePropAtBone(bone, blueprint): a new prop at one of this prop's
// bones (tree groups breaking up into trees).
static int prop_CreatePropAtBone(lua_State* L) {
    auto* e = check_entity(L);
    auto* sim = get_sim(L);
    if (!e || !sim || e->destroyed() || lua_type(L, 3) != LUA_TSTRING) {
        lua_pushnil(L);
        return 1;
    }
    const i32 bone = resolve_bone_index(e, L, 2);
    const auto* bones = e->bone_data();
    const sim::Quaternion rot =
        bones && bones->is_valid(bone)
            ? sim::quat_multiply(e->orientation(),
                                 bones->bones[static_cast<size_t>(bone)].world_rotation)
            : e->orientation();
    sim::spawn_prop(L, *sim, lua_tostring(L, 3), bone_world_position(e, bone), rot, true);
    return 1;
}

// prop:SinkAway(rate): sink into the ground at `rate` units per second
// until the script destroys it.
static int prop_SinkAway(lua_State* L) {
    auto* e = check_entity(L);
    if (e && e->is_prop()) static_cast<sim::Prop*>(e)->sink_rate = static_cast<f32>(lua_tonumber(L, 2));
    return 0;
}

// motor:Whack(nx, ny, nz, depth, dotrunk): the tree falls over away from
// the push, once. (Moho simulates the fall; it lands the same way.)
static int falldown_Whack(lua_State* L) {
    if (!lua_istable(L, 1)) return 0;
    lua_pushstring(L, "_c_fallen");
    lua_rawget(L, 1);
    const bool fallen = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    lua_pushstring(L, "_c_prop_id");
    lua_rawget(L, 1);
    const auto id = static_cast<u32>(lua_tonumber(L, -1));
    lua_pop(L, 1);
    auto* sim = get_sim(L);
    sim::Entity* e = sim ? sim->entity_registry().find(id) : nullptr;
    if (fallen || !e || e->destroyed()) return 0;
    f32 dx = static_cast<f32>(lua_tonumber(L, 2));
    f32 dz = static_cast<f32>(lua_tonumber(L, 4));
    const f32 len = std::sqrt(dx * dx + dz * dz);
    if (len < 1e-4f) {
        dx = 1.0f; // no horizontal push: any way will do
        dz = 0.0f;
    } else {
        dx /= len;
        dz /= len;
    }
    // A quarter turn about (dz, 0, -dx) carries up (+Y) onto the push.
    const f32 s = 0.70710678f;
    const sim::Quaternion fall{dz * s, 0.0f, -dx * s, s};
    e->set_orientation(sim::quat_multiply(fall, e->orientation()));
    lua_pushstring(L, "_c_fallen");
    lua_pushboolean(L, 1);
    lua_rawset(L, 1);
    return 0;
}

// prop:FallDown() -> motor: whacking it topples the tree.
static int prop_FallDown(lua_State* L) {
    auto* e = check_entity(L);
    lua_newtable(L);
    lua_pushstring(L, "_c_prop_id");
    lua_pushnumber(L, e ? static_cast<lua_Number>(e->entity_id()) : 0);
    lua_rawset(L, -3);
    lua_pushstring(L, "__osc_falldown_mt");
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushstring(L, "__index");
        lua_pushvalue(L, -2);
        lua_rawset(L, -3);
        lua_pushstring(L, "Whack");
        lua_pushcfunction(L, falldown_Whack);
        lua_rawset(L, -3);
        lua_pushstring(L, "__osc_falldown_mt");
        lua_pushvalue(L, -2);
        lua_rawset(L, LUA_REGISTRYINDEX);
    }
    lua_setmetatable(L, -2);
    return 1;
}

// clang-format off
const MethodEntry prop_methods[] = {
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
    {"SetCollisionShape",           entity_SetCollisionShape},
    {"SetMesh",                     entity_SetMesh},
    {"SetScale",                    entity_SetScale},
    {"SetDrawScale",                entity_SetScale},
    {"SetVizToAllies",              entity_SetVizToAllies},
    {"SetVizToEnemies",             entity_SetVizToEnemies},
    {"SetVizToFocusPlayer",         entity_SetVizToFocusPlayer},
    {"SetVizToNeutrals",            entity_SetVizToNeutrals},
    {"SetReclaimable",              entity_SetReclaimable},
    {"SetMaxReclaimValues",          prop_SetMaxReclaimValues},
    {"Kill",                         prop_Kill},
    {"CreatePropAtBone",             prop_CreatePropAtBone},
    {"SinkAway",                     prop_SinkAway},
    {"FallDown",                     prop_FallDown},
    {"SetPropCollision",             entity_SetCollisionShape},
    {"GetHeading",                   entity_GetHeading},
    {nullptr, nullptr},
};
// clang-format on

// clang-format off
const MethodEntry entity_category_methods[] = {
    {nullptr, nullptr},
};
// clang-format on

// ====================================================================
// Audio globals for UI (M147b)
// ====================================================================

/// A sound's bank and cue: a Sound{Bank, Cue} table, or a bare cue name
/// (the Interface bank).
bool sound_arg(lua_State* L, int idx, std::string& bank, std::string& cue) {
    if (lua_type(L, idx) == LUA_TSTRING) {
        cue = lua_tostring(L, idx);
        bank = "Interface";
        return true;
    }
    return extract_sound_table(L, idx, bank, cue);
}

} // namespace osc::lua
