// The sim's unit bindings: moho.unit_methods (orders, builds, economy,
// toggles and script bits, selection sets).
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

static int (*const stub_return_true)(lua_State*) = lua_stubs::return_true;

/// Look up Blueprint.Audio[soundName] for entity e.
/// On success pushes 3 values (Blueprint, Audio, audioEntry) and returns true.
/// On failure pops any partial pushes and returns false.
static bool lookup_blueprint_audio(lua_State* L, const sim::Entity* e,
                                   int sound_arg) {
    if (lua_type(L, sound_arg) != LUA_TSTRING) return false;
    if (!push_entity_blueprint(L, e)) return false;
    lua_pushstring(L, "Audio");
    lua_rawget(L, -2);
    if (!lua_istable(L, -1)) { lua_pop(L, 2); return false; }
    lua_pushvalue(L, sound_arg);
    lua_rawget(L, -2);
    if (!lua_istable(L, -1)) { lua_pop(L, 3); return false; }
    return true;
}

/// unit:PlayUnitSound(name) -- the one-shot Blueprint.Audio[name], at the
/// unit; true if the blueprint has it.
static int unit_PlayUnitSound(lua_State* L) {
    auto* mgr = get_sound_mgr(L);
    if (!mgr) { lua_pushboolean(L, 0); return 1; }
    auto* e = check_entity(L);
    if (!e || e->destroyed()) { lua_pushboolean(L, 0); return 1; }
    if (!lookup_blueprint_audio(L, e, 2)) { lua_pushboolean(L, 0); return 1; }

    std::string bank, cue, lod;
    const bool ok = extract_sound_table(L, lua_gettop(L), bank, cue, &lod);
    lua_pop(L, 3);
    if (ok) {
        auto pos = e->position();
        mgr->play(bank, cue, &pos, lod);
    }
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

/// unit:PlayUnitAmbientSound(name) -- loop Blueprint.Audio[name] on the
/// unit under that name; already playing, it carries on. A fallback: retail's
/// (and FAF's) Unit class defines its own in Lua, which loops the sound on an
/// attached child entity through SetAmbientSound.
static int unit_PlayUnitAmbientSound(lua_State* L) {
    auto* mgr = get_sound_mgr(L);
    if (!mgr) { lua_pushboolean(L, 0); return 1; }
    auto* e = check_entity(L);
    if (!e || e->destroyed()) { lua_pushboolean(L, 0); return 1; }
    const std::string name = lua_type(L, 2) == LUA_TSTRING ? lua_tostring(L, 2) : "";
    if (!name.empty() && mgr->is_playing(e->ambient_sound(name))) {
        lua_pushboolean(L, 1);
        return 1;
    }
    if (!lookup_blueprint_audio(L, e, 2)) { lua_pushboolean(L, 0); return 1; }

    std::string bank, cue;
    const bool ok = extract_sound_table(L, lua_gettop(L), bank, cue);
    lua_pop(L, 3);
    if (ok) {
        auto pos = e->position();
        e->set_ambient_sound(name, mgr->play_loop(bank, cue, &pos));
    }
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

/// unit:StopUnitAmbientSound([name]) -- stop that ambient loop (all of
/// them without a name)
static int unit_StopUnitAmbientSound(lua_State* L) {
    auto* e = check_entity(L);
    if (e && !e->destroyed())
        stop_ambient(get_sound_mgr(L), e, lua_type(L, 2) == LUA_TSTRING ? lua_tostring(L, 2) : nullptr);
    lua_pushboolean(L, 1);
    return 1;
}

// unit:GetCurrentMoveLocation(): where it is going (its navigator's goal),
// or where it is when it isn't.
static int unit_GetCurrentMoveLocation(lua_State* L) {
    auto* u = check_unit(L);
    if (!u) {
        push_vector3(L, {0, 0, 0});
        return 1;
    }
    push_vector3(L, u->navigator().busy() ? u->navigator().goal() : u->position());
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

/// unit:IsDead() -- UserUnit's engine method (the sim's Unit class defines
/// its own in Lua, which shadows this). A UI handle whose unit is gone, or
/// dying, is dead.
static int unit_IsDead(lua_State* L) {
    auto* u = check_unit(L);
    lua_pushboolean(L, !u || u->destroyed() || u->is_dying());
    return 1;
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
            result = u->is_being_captured();
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

    sim->entity_registry().for_each_unit([&](sim::Entity& e) {
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

    // Class: the unit's weapon class for this label (unit:GetWeaponClass,
    // i.e. its Weapons table entry or the base /lua/sim/Weapon.lua Weapon),
    // with `unit` set as Weapon.__init does. A classed weapon fires through
    // its script (Weapon::fires_through_script).
    const int wtable = lua_gettop(L);
    bool classed = false;
    if (unit->lua_table_ref() >= 0) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, unit->lua_table_ref());
        const int utable = lua_gettop(L);
        lua_pushstring(L, "GetWeaponClass");
        lua_gettable(L, utable);
        if (lua_isfunction(L, -1)) {
            lua_pushvalue(L, utable);
            lua_pushstring(L, weapon->label.c_str());
            if (lua_pcall(L, 2, 1, 0) == 0 && lua_istable(L, -1)) {
                lua_setmetatable(L, wtable);
                lua_pushstring(L, "unit");
                lua_pushvalue(L, utable);
                lua_rawset(L, wtable);
                classed = true;
            }
        }
        lua_settop(L, wtable);
    }

    // Otherwise the shared engine-methods metatable.
    if (!classed) {
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
    }

    // Store Lua table ref on the weapon
    lua_pushvalue(L, -1);
    weapon->lua_table_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    weapon->script_class = classed;

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

// The first of a factory's rally orders' positions (Moho: its factory command
// queue's first target), or nil. Retail's roll-off reads it unguarded, and a
// factory always has one: its initial rally until it is given another. Only
// reads: the UI's unit objects share these methods, and a query from one
// player's UI must not change the sim.
static int unit_GetRallyPoint(lua_State* L) {
    auto* u = check_unit(L);
    auto* sim = get_sim(L);
    sim::Vector3 point;
    if (u && u->rally_point(sim ? sim->lua_state() : L, point)) {
        push_vector3(L, point);
        return 1;
    }
    lua_pushnil(L);
    return 1;
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
/// TestCommandCaps(cap) -> whether the unit has that order cap now.
static int unit_TestCommandCaps(lua_State* L) {
    auto* u = check_unit(L);
    const char* cap = luaL_checkstring(L, 2);
    lua_pushboolean(L, u && u->has_command_cap(cap) ? 1 : 0);
    return 1;
}

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

    // Read Slot from Blueprint.Enhancements[name]
    if (push_entity_blueprint(L, u)) {
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
        lua_pop(L, 1); // blueprint
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
    if (u) u->pause(lua_toboolean(L, 2) != 0);
    return 0;
}

// unit:IsPaused() -> bool
static int unit_IsPaused(lua_State* L) {
    auto* u = check_unit(L);
    lua_pushboolean(L, u && u->is_paused() ? 1 : 0);
    return 1;
}

// unit:CanBuild(bp_id) -> bool — checks Economy.BuildableCategory
static int unit_CanBuild(lua_State* L) {
    auto* u = check_unit(L);
    const char* target_bp = luaL_checkstring(L, 2);
    lua_pushboolean(L, u && target_bp && sim::blueprint_can_build(L, u->blueprint_id(), target_bp));
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

// unit:CanPathTo(destPos) -> reachable, bestPos
// Whether the unit can get to destPos at all, and where it would end up:
// destPos when reachable, else the reachable point closest to it (retail AI
// retargets to bestPos). A connectivity query, not a path search, so a
// movement-heavy tick can't make it answer "unreachable" -- the AI would
// then wait for transports that never come.
static int push_path_reachability(lua_State* L) {
    auto* unit = check_unit(L);
    auto* sim = get_sim(L);
    sim::Vector3 dest{0, 0, 0};
    if (lua_istable(L, 2)) {
        lua_rawgeti(L, 2, 1);
        dest.x = static_cast<f32>(lua_tonumber(L, -1));
        lua_rawgeti(L, 2, 2);
        dest.y = static_cast<f32>(lua_tonumber(L, -1));
        lua_rawgeti(L, 2, 3);
        dest.z = static_cast<f32>(lua_tonumber(L, -1));
        lua_pop(L, 3);
    }
    if (!unit || !sim || !sim->pathfinder()) {
        lua_pushboolean(L, 1); // no pathfinding grid: nothing blocks
        push_vector3(L, dest);
        return 2;
    }
    const auto& pos = unit->position();
    const auto reach = sim->pathfinder()->reachability(
        pos.x, pos.z, dest.x, dest.z, unit->layer(), unit->naval_draft(),
        unit->is_amphibious() || unit->is_hover());
    lua_pushboolean(L, reach.reachable ? 1 : 0);
    if (reach.reachable) {
        push_vector3(L, dest);
    } else {
        const f32 y = sim->terrain()
            ? sim->terrain()->get_surface_height(reach.best_x, reach.best_z) : pos.y;
        push_vector3(L, {reach.best_x, y, reach.best_z});
    }
    return 2;
}

static int unit_CanPathTo(lua_State* L) { return push_path_reachability(L); }

// unit:CanPathToCell(destPos) -> reachable, bestPos
// Retail's cell-granular variant; the reachability answer is already
// cell-granular, so it is the same query.
static int unit_CanPathToCell(lua_State* L) { return push_path_reachability(L); }

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

/// StopSiloBuild(): the missile under way is abandoned, and the builds
/// ordered with it.
static int unit_StopSiloBuild(lua_State* L) {
    auto* unit = check_unit(L);
    if (unit) unit->stop_silo_build();
    return 0;
}

static int unit_GetMissileInfo(lua_State* L) {
    auto* unit = check_unit(L);
    if (!unit) { lua_newtable(L); return 1; }

    lua_newtable(L);

    // Each kind's missiles stored, its storage, and the builds ordered.
    for (const bool nuke : {true, false}) {
        const std::string kind = nuke ? "nukeSilo" : "tacticalSilo";
        for (const auto& [field, value] :
             {std::pair{"StorageCount", unit->silo_ammo(nuke)},
              std::pair{"MaxStorageCount", unit->silo_max_storage(nuke)},
              std::pair{"BuildCount", unit->silo_build_count(nuke)}}) {
            lua_pushstring(L, (kind + field).c_str());
            lua_pushnumber(L, value);
            lua_rawset(L, -3);
        }
    }
    return 1;
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

static int unit_GetCreator(lua_State* L) {
    auto* u = check_unit(L);
    if (!u || u->creator_id() == 0) { lua_pushnil(L); return 1; }
    auto* sim = get_sim(L);
    if (!sim) { lua_pushnil(L); return 1; }
    auto* creator = sim->entity_registry().find(u->creator_id());
    if (!creator || creator->destroyed() || creator->lua_table_ref() < 0) {
        lua_pushnil(L);
        return 1;
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, creator->lua_table_ref());
    return 1;
}

// unit:IsInCategory(category): a category's name, as UserUnit's takes it
// (retail's UI asks 'COMMAND', 'FACTORY', a faction), or a category object.
static int unit_IsInCategory(lua_State* L) {
    auto* u = check_unit(L);
    if (!u) { lua_pushboolean(L, 0); return 1; }
    if (lua_type(L, 2) == LUA_TSTRING) {
        lua_pushboolean(L, u->has_category(lua_tostring(L, 2)) ? 1 : 0);
        return 1;
    }
    if (!lua_istable(L, 2)) { lua_pushboolean(L, 0); return 1; }
    bool matches = osc::lua::unit_matches_category(L, 2, u->categories());
    lua_pushboolean(L, matches ? 1 : 0);
    return 1;
}

static int unit_IsOverchargePaused(lua_State* L) {
    auto* u = check_unit(L);
    lua_pushboolean(L, (u && u->overcharge_paused()) ? 1 : 0);
    return 1;
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
        e->set_orientation({0, osc::dmath::sin(half), 0, osc::dmath::cos(half)});
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

// self:AddOnGivenCallback(fn)
static int unit_AddOnGivenCallback(lua_State* L) {
    auto* u = check_unit(L);
    if (!u) return 0;
    if (!lua_isfunction(L, 2)) return 0;
    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    u->add_on_given_callback(ref);
    return 0;
}

// self:AddOnUnitBuiltCallback(fn, category)
// Registers a callback fired when this unit finishes building another unit.
static int unit_AddOnUnitBuiltCallback(lua_State* L) {
    auto* u = check_unit(L);
    if (!u) return 0;
    if (!lua_isfunction(L, 2)) return 0;
    lua_pushvalue(L, 2);
    int func_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    int cat_ref = -1;
    if (lua_istable(L, 3)) {
        lua_pushvalue(L, 3);
        cat_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    u->add_on_unit_built_callback(func_ref, cat_ref);
    return 0;
}

// --- Veterancy ---

// unit:GetVeterancyLevel()
static int unit_GetVeterancyLevel(lua_State* L) {
    auto* u = check_unit(L);
    if (!u) return 0;
    lua_pushnumber(L, u->vet_level());
    return 1;
}

// unit:SetVeterancyLevel(level)
static int unit_SetVeterancyLevel(lua_State* L) {
    auto* u = check_unit(L);
    if (!u) return 0;
    int level = static_cast<int>(lua_tonumber(L, 2));
    if (level < 0) level = 0;
    if (level > 5) level = 5;
    u->set_vet_level(static_cast<u8>(level));
    return 0;
}

// unit:AddXP(amount)
static int unit_AddXP(lua_State* L) {
    auto* u = check_unit(L);
    if (!u) return 0;
    f32 amount = static_cast<f32>(lua_tonumber(L, 2));
    auto* sim = get_sim(L);
    if (sim) {
        u->add_xp(amount, L, sim->entity_registry());
    }
    return 0;
}

// unit:GetXPValue()
static int unit_GetXPValue(lua_State* L) {
    auto* u = check_unit(L);
    if (!u) return 0;
    lua_pushnumber(L, u->xp_value());
    return 1;
}

// --- Cloak / Stealth / AutoMode / DeathWeapon bindings ---

// EnableCloak/EnableStealth/EnableSonarStealth are not Moho methods (the
// retail binary has no such names); scripts calling them mean "make this
// unit stealthy", so they grant the intel rather than following
// EnableIntel's has-it-already rule.
static void grant_intel(sim::Unit* u, const char* type) {
    if (!u) return;
    u->add_intel(type, 0.0f);
    u->enable_intel(type);
}

static int unit_EnableCloak(lua_State* L) {
    grant_intel(check_unit(L), "Cloak");
    return 0;
}
static int unit_DisableCloak(lua_State* L) {
    auto* u = check_unit(L);
    if (u) u->disable_intel("Cloak");
    return 0;
}
static int unit_IsUnitCloaked(lua_State* L) {
    auto* u = check_unit(L);
    lua_pushboolean(L, u && u->is_cloaked());
    return 1;
}
static int unit_EnableStealth(lua_State* L) {
    grant_intel(check_unit(L), "RadarStealth");
    return 0;
}
static int unit_DisableStealth(lua_State* L) {
    auto* u = check_unit(L);
    if (u) u->disable_intel("RadarStealth");
    return 0;
}
static int unit_EnableSonarStealth(lua_State* L) {
    grant_intel(check_unit(L), "SonarStealth");
    return 0;
}
static int unit_DisableSonarStealth(lua_State* L) {
    auto* u = check_unit(L);
    if (u) u->disable_intel("SonarStealth");
    return 0;
}
static int unit_SetAutoMode(lua_State* L) {
    auto* u = check_unit(L);
    if (u) u->set_auto_mode(lua_toboolean(L, 2) != 0);
    return 0;
}
static int unit_GetAutoMode(lua_State* L) {
    auto* u = check_unit(L);
    lua_pushboolean(L, u && u->auto_mode());
    return 1;
}
static int unit_SetDeathWeaponEnabled(lua_State* L) {
    auto* u = check_unit(L);
    if (!u) return 0;
    const char* label = lua_type(L, 2) == LUA_TSTRING ? lua_tostring(L, 2) : nullptr;
    bool enabled = lua_toboolean(L, 3) != 0;
    if (label) {
        for (auto& w : u->weapons()) {
            if (w->label == label) { w->fire_on_death = enabled; break; }
        }
    }
    return 0;
}
static int unit_GetDeathWeaponEnabled(lua_State* L) {
    auto* u = check_unit(L);
    if (!u) { lua_pushboolean(L, 0); return 1; }
    const char* label = lua_type(L, 2) == LUA_TSTRING ? lua_tostring(L, 2) : nullptr;
    if (label) {
        for (auto& w : u->weapons()) {
            if (w->label == label) { lua_pushboolean(L, w->fire_on_death ? 1 : 0); return 1; }
        }
    }
    lua_pushboolean(L, 0);
    return 1;
}

static int unit_HasValidTeleportDest(lua_State* L) {
    auto* u = check_unit(L);
    if (!u) { lua_pushboolean(L, 0); return 1; }
    auto* sim = get_sim(L);
    auto& cmds = u->command_queue();
    bool valid = false;
    for (const auto& cmd : cmds) {
        if (cmd.type == sim::CommandType::Teleport) {
            valid = sim && sim->is_valid_teleport_destination(*u, cmd.target_pos);
            break;
        }
    }
    lua_pushboolean(L, valid ? 1 : 0);
    return 1;
}

// --- UserUnit methods (the game UI's unit objects; resolve by id) ---


static int unit_GetCustomName(lua_State* L) {
    auto* e = check_entity(L);
    if (!e || e->custom_name().empty()) lua_pushnil(L);
    else lua_pushstring(L, e->custom_name().c_str());
    return 1;
}

/// unit:GetEconData() -> this unit's production and use, per second.
static int unit_GetEconData(lua_State* L) {
    auto* u = check_unit(L);
    lua_newtable(L);
    if (!u) return 1;
    const auto& econ = u->economy();
    auto set = [&](const char* k, f64 v) {
        lua_pushstring(L, k);
        lua_pushnumber(L, v);
        lua_rawset(L, -3);
    };
    set("massProduced", econ.production_active ? econ.production_mass : 0.0);
    set("energyProduced", econ.production_active ? econ.production_energy : 0.0);
    set("massConsumed", econ.consumption_active ? econ.consumption_mass : 0.0);
    set("energyConsumed", econ.consumption_active ? econ.consumption_energy : 0.0);
    set("massRequested", econ.consumption_mass);
    set("energyRequested", econ.consumption_energy);
    return 1;
}

/// unit:GetFocus() -> the unit it is building (or repairing), or nil.
static int unit_GetFocus(lua_State* L) {
    auto* u = check_unit(L);
    auto* sim = get_sim(L);
    sim::Entity* focus = (u && sim && u->build_target_id())
        ? sim->entity_registry().find(u->build_target_id()) : nullptr;
    if (focus && focus->is_unit() && !focus->destroyed()) push_unit_for_ui(L, focus);
    else lua_pushnil(L);
    return 1;
}

/// unit:GetGuardedEntity() -> the unit it guards/assists, or nil.
static int unit_GetGuardedEntity(lua_State* L) {
    auto* u = check_unit(L);
    auto* sim = get_sim(L);
    sim::Entity* target = nullptr;
    if (u && sim && !u->command_queue().empty() &&
        u->command_queue().front().type == sim::CommandType::Guard)
        target = sim->entity_registry().find(u->command_queue().front().target_id);
    if (target && target->is_unit() && !target->destroyed()) push_unit_for_ui(L, target);
    else lua_pushnil(L);
    return 1;
}

static int unit_GetFootPrintSize(lua_State* L) {
    auto* u = check_unit(L);
    lua_pushnumber(L, u ? std::max(u->footprint_size_x(), u->footprint_size_z()) : 1.0f);
    return 1;
}

static int unit_HasUnloadCommandQueuedUp(lua_State* L) {
    auto* u = check_unit(L);
    bool found = false;
    if (u)
        for (const auto& c : u->command_queue())
            if (c.type == sim::CommandType::TransportUnload) { found = true; break; }
    lua_pushboolean(L, found);
    return 1;
}

static int unit_IsAutoMode(lua_State* L) {
    auto* u = check_unit(L);
    lua_pushboolean(L, u && u->auto_mode());
    return 1;
}

static int unit_IsRepeatQueue(lua_State* L) {
    auto* u = check_unit(L);
    lua_pushboolean(L, u && u->repeat_queue());
    return 1;
}

static int unit_SetRepeatQueue(lua_State* L) {
    auto* u = check_unit(L);
    if (u) u->set_repeat_queue(lua_toboolean(L, 2) != 0);
    return 0;
}

static int unit_IsAutoSurfaceMode(lua_State* L) {
    auto* u = check_unit(L);
    lua_pushboolean(L, u && u->auto_surface_mode() ? 1 : 0);
    return 1;
}
// Not simulated yet: nothing stuns.
static int unit_IsStunned(lua_State* L) { lua_pushboolean(L, 0); return 1; }

// Selection sets: named groups a unit belongs to (selection.lua's
// control-group hotkeys). Per-UI-state bookkeeping keyed by entity id.
static constexpr const char* kSelectionSetsKey = "__osc_selection_sets";

/// Push this unit's set table (name -> true), creating it if `create`;
/// pushes nil otherwise when there is none.
static void push_unit_selection_sets(lua_State* L, u32 id, bool create) {
    lua_pushstring(L, kSelectionSetsKey);
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushstring(L, kSelectionSetsKey);
        lua_pushvalue(L, -2);
        lua_rawset(L, LUA_REGISTRYINDEX);
    }
    lua_rawgeti(L, -1, static_cast<int>(id));
    if (!lua_istable(L, -1) && create) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_rawseti(L, -3, static_cast<int>(id));
    }
    lua_remove(L, -2);
}

static int unit_AddSelectionSet(lua_State* L) {
    auto* e = check_entity(L);
    if (!e || lua_type(L, 2) != LUA_TSTRING) return 0;
    push_unit_selection_sets(L, e->entity_id(), true);
    lua_pushvalue(L, 2);
    lua_pushboolean(L, 1);
    lua_rawset(L, -3);
    lua_pop(L, 1);
    return 0;
}

static int unit_RemoveSelectionSet(lua_State* L) {
    auto* e = check_entity(L);
    if (!e || lua_type(L, 2) != LUA_TSTRING) return 0;
    push_unit_selection_sets(L, e->entity_id(), false);
    if (lua_istable(L, -1)) {
        lua_pushvalue(L, 2);
        lua_pushnil(L);
        lua_rawset(L, -3);
    }
    lua_pop(L, 1);
    return 0;
}

static int unit_HasSelectionSet(lua_State* L) {
    auto* e = check_entity(L);
    bool has = false;
    if (e && lua_type(L, 2) == LUA_TSTRING) {
        push_unit_selection_sets(L, e->entity_id(), false);
        if (lua_istable(L, -1)) {
            lua_pushvalue(L, 2);
            lua_rawget(L, -2);
            has = lua_toboolean(L, -1) != 0;
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }
    lua_pushboolean(L, has);
    return 1;
}

/// unit:GetSelectionSets() -> array of the set names this unit is in.
static int unit_GetSelectionSets(lua_State* L) {
    auto* e = check_entity(L);
    lua_newtable(L);
    if (!e) return 1;
    const int out = lua_gettop(L);
    push_unit_selection_sets(L, e->entity_id(), false);
    if (lua_istable(L, -1)) {
        int n = 1;
        lua_pushnil(L);
        while (lua_next(L, -2) != 0) {
            lua_pop(L, 1); // value
            if (lua_type(L, -1) == LUA_TSTRING) {
                lua_pushvalue(L, -1);
                lua_rawseti(L, out, n++);
            }
        }
    }
    lua_pop(L, 1);
    return 1;
}

static int unit_ProcessInfo(lua_State* L);

// clang-format off
const MethodEntry unit_methods[] = {
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
    {"IsIdle",                      unit_IsIdleState}, // UserUnit
    {"IsDead",                      unit_IsDead},      // UserUnit
    {"GetCustomName",               unit_GetCustomName},       // UserUnit
    {"GetEconData",                 unit_GetEconData},         // UserUnit
    {"GetFocus",                    unit_GetFocus},            // UserUnit
    {"GetGuardedEntity",            unit_GetGuardedEntity},    // UserUnit
    {"GetFootPrintSize",            unit_GetFootPrintSize},    // UserUnit
    {"HasUnloadCommandQueuedUp",    unit_HasUnloadCommandQueuedUp}, // UserUnit
    {"IsAutoMode",                  unit_IsAutoMode},          // UserUnit
    {"IsAutoSurfaceMode",           unit_IsAutoSurfaceMode},   // UserUnit
    {"IsRepeatQueue",               unit_IsRepeatQueue},       // UserUnit
    {"SetRepeatQueue",              unit_SetRepeatQueue},
    {"IsStunned",                   unit_IsStunned},           // UserUnit
    {"AddSelectionSet",             unit_AddSelectionSet},     // UserUnit
    {"RemoveSelectionSet",          unit_RemoveSelectionSet},  // UserUnit
    {"HasSelectionSet",             unit_HasSelectionSet},     // UserUnit
    {"GetSelectionSets",            unit_GetSelectionSets},    // UserUnit
    {"ProcessInfo",                 unit_ProcessInfo},         // UserUnit
    {"GetFireState",                unit_GetFireState},
    {"SetFireState",                unit_SetFireState},
    {"ToggleFireState",             unit_ToggleFireState},
    {"SetPaused",                   unit_SetPaused},
    {"IsPaused",                    unit_IsPaused},
    // Bones / visual
    {"ShowBone",                    unit_ShowBone},
    {"HideBone",                    unit_HideBone},
    {"SetMesh",                     entity_SetMesh},
    {"IsValidBone",                 entity_IsValidBone},
    {"GetBoneDirection",            entity_GetBoneDirection},
    // Stubs — build / command
    {"GetCommandQueue",             unit_GetCommandQueue},
    {"CanBuild",                    unit_CanBuild},
    {"AddCommandCap",               unit_AddCommandCap},
    {"RemoveCommandCap",            unit_RemoveCommandCap},
    {"RestoreCommandCaps",          unit_RestoreCommandCaps},
    {"HasValidTeleportDest",        unit_HasValidTeleportDest},
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
    {"SetCollisionShape",           entity_SetCollisionShape},
    {"RevertCollisionShape",        entity_RevertCollisionShape},
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
    {"GetCurrentMoveLocation",      unit_GetCurrentMoveLocation},
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
    {"GetMissileInfo",              unit_GetMissileInfo},
    {"StopSiloBuild",               unit_StopSiloBuild},
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
    {"AddOnGivenCallback",          unit_AddOnGivenCallback},
    {"AddOnUnitBuiltCallback",      unit_AddOnUnitBuiltCallback},
    {"PlayUnitSound",               unit_PlayUnitSound},
    {"PlayUnitAmbientSound",        unit_PlayUnitAmbientSound},
    {"StopUnitAmbientSound",        unit_StopUnitAmbientSound},
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
    {"SetUnSelectable",             entity_SetUnSelectable},
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
    {"Kill",                        entity_Kill},
    {"GetFocusUnit",                unit_GetFocusUnit},
    {"RestoreBuildRestrictions",    unit_RestoreBuildRestrictions},
    {"SetCreator",                  unit_SetCreator},
    {"GetCreator",                  unit_GetCreator},
    {"IsInCategory",                unit_IsInCategory},
    {"IsOverchargePaused",          unit_IsOverchargePaused},
    {"OccupyGround",               stub_return_true},
    {"ResetSpeedAndAccel",          unit_ResetSpeedAndAccel},
    {"AddToggleCap",                unit_AddToggleCap},
    {"RemoveToggleCap",             unit_RemoveToggleCap},
    {"TestCommandCaps",             unit_TestCommandCaps},
    {"TestToggleCaps",              unit_TestToggleCaps},
    {"SetBlockCommandQueue",        unit_SetBlockCommandQueue},
    {"PlayCommanderWarpInEffect",   stub_noop},
    {"GetRallyPoint",                unit_GetRallyPoint},
    {"SetBusy",                      unit_SetBusy},
    {"SetRotation",                  unit_SetRotation},
    {"GetGuardedUnit",               unit_GetGuardedUnit},
    {"PlayFxRollOffEnd",             stub_noop},
    {"SetupBuildBones",              stub_noop},
    {"GetBlip",                      unit_GetBlip},
    {"GetVeterancyLevel",           unit_GetVeterancyLevel},
    {"SetVeterancyLevel",           unit_SetVeterancyLevel},
    {"AddXP",                       unit_AddXP},
    {"GetXPValue",                  unit_GetXPValue},
    {"EnableCloak",                 unit_EnableCloak},
    {"DisableCloak",                unit_DisableCloak},
    {"IsUnitCloaked",               unit_IsUnitCloaked},
    {"EnableStealth",               unit_EnableStealth},
    {"DisableStealth",              unit_DisableStealth},
    {"EnableSonarStealth",          unit_EnableSonarStealth},
    {"DisableSonarStealth",         unit_DisableSonarStealth},
    {"SetAutoMode",                 unit_SetAutoMode},
    {"GetAutoMode",                 unit_GetAutoMode},
    {"SetDeathWeaponEnabled",       unit_SetDeathWeaponEnabled},
    {"GetDeathWeaponEnabled",       unit_GetDeathWeaponEnabled},
    {nullptr, nullptr},
};
// clang-format on


/// unit:ProcessInfo(action, value) -- UserUnit's request to change unit
/// settings (SetAutoMode, SetRepeatQueue, ...). Queued with the UI's sim
/// callbacks, so it reaches the sim through its input like other orders.
static int unit_ProcessInfo(lua_State* L) {
    auto* queue = get_callback_queue(L);
    auto* e = check_entity(L, 1);
    if (!queue || !e || lua_type(L, 2) != LUA_TSTRING) return 0;
    sim::SimCallbackEntry entry;
    entry.func_name = sim::kProcessInfoCallback;
    entry.args["Action"] = std::string(lua_tostring(L, 2));
    if (lua_isstring(L, 3)) entry.args["Value"] = std::string(lua_tostring(L, 3));
    entry.unit_ids.push_back(e->entity_id());
    queue->push(std::move(entry));
    return 0;
}

/// Same idle test as unit:IsIdleState().
bool unit_is_idle(const sim::Unit& u) {
    return u.command_queue().empty() && !u.is_building() && !u.is_being_built() &&
           !u.is_repairing() && !u.is_capturing();
}

} // namespace osc::lua
