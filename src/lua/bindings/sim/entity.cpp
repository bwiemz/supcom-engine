// The sim's entity bindings: moho.entity_methods, props' and entity sounds.
// Split out of moho_bindings.cpp by class (M191 step 2); what the files
// share is declared in lua/moho_bindings_internal.hpp.

#include "lua/moho_bindings.hpp"
#include "lua/moho_bindings_internal.hpp"
#include "lua/arg_count.hpp"
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
#include "sim/script_class.hpp"
#include "sim/script_warning.hpp"
#include "sim/sim_state.hpp"
#include "sim/collision_beam.hpp"
#include "sim/projectile_script.hpp"
#include "sim/thread_manager.hpp"
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

/// entity:PlaySound(sound) -- a one-shot at the entity
static int entity_RequestRefreshUI(lua_State* L) {
    if (auto* e = check_entity(L)) {
        e->request_ui_refresh();
    }
    return 0;
}

static int entity_PlaySound(lua_State* L) {
    auto* e = check_entity(L);
    if (!e || e->destroyed()) return 0;

    std::string bank, cue, lod;
    if (!extract_sound_table(L, 2, bank, cue, &lod)) return 0;
    request_world_sound(L, *e, std::move(bank), std::move(cue), std::move(lod));
    return 0;
}

/// entity:SetAmbientSound(detail, rumble) -- the entity's two ambient loop
/// slots; nil stops a slot.
static int entity_SetAmbientSound(lua_State* L) {
    auto* e = check_entity(L);
    if (!e || e->destroyed()) return 0;
    const char* slots[2] = {"__ambient", "__rumble"};
    for (int i = 0; i < 2; ++i) {
        std::string bank, cue, lod;
        if (!extract_sound_table(L, 2 + i, bank, cue, &lod)) cue.clear();
        e->set_ambient_sound(slots[i], std::move(bank), std::move(cue), std::move(lod));
    }
    return 0;
}

// --- Bone query functions ---

/// GetCollisionExtents(): {Min, Max}, the world box around the entity's
/// collision shape (objective arrows sit on its top); nil without one.
static int entity_GetCollisionExtents(lua_State* L) {
    const auto* e = check_entity(L);
    const auto bounds = e ? sim::collision_bounds(*e) : std::nullopt;
    if (!bounds) {
        lua_pushnil(L);
        return 1;
    }
    lua_newtable(L);
    lua_pushstring(L, "Min");
    push_vector3(L, bounds->first);
    lua_rawset(L, -3);
    lua_pushstring(L, "Max");
    push_vector3(L, bounds->second);
    lua_rawset(L, -3);
    return 1;
}

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
    // Retail returns three numbers, not a vector table: the Scathis's
    // CreateProjectileAtMuzzle destructures `v.x, v.y, v.z = GetBoneDirection(b)`
    // (retail /units/URL0401/URL0401_Script.lua), and GetAngleInBetween then
    // errors on the nils, so the weapon never fires.
    auto* e = check_entity(L);
    if (!e) {
        push_direction(L, {0, 0, 1});
        return 3;
    }
    auto* bd = e->bone_data();
    if (!bd || lua_gettop(L) < 2) {
        // No bone data or no bone arg: return entity forward direction
        auto fwd = sim::quat_rotate(e->orientation(), {0, 0, 1});
        push_direction(L, fwd);
        return 3;
    }
    // The bone's forward (+Z) in the world: a unit's as posed (turrets,
    // rotators), anything else's in bind pose.
    i32 idx = resolve_bone_index(e, L, 2);
    const sim::Quaternion bone_world_rot =
        e->is_unit() ? static_cast<const sim::Unit*>(e)->bone_world_rotation(idx)
                     : sim::quat_multiply(e->orientation(),
                                          bd->bones[static_cast<size_t>(idx)].world_rotation);
    auto dir = sim::quat_rotate(bone_world_rot, {0, 0, 1});
    push_direction(L, dir);
    return 3;
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
        // Moho's SetPosition: the committed transform moved to `pos`, pending; an immediate one
        // is committed at once and shown as a jump.
        e->set_orientation(e->orientation());
        e->set_position(v);
        if (lua_toboolean(L, 3)) {
            e->note_snap();
            e->place_now();
        }
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
        e->set_position(e->position());
        e->set_orientation(q);
        if (lua_toboolean(L, 3)) {
            e->place_now();
        }
    }
    return 0;
}

static int entity_GetHealth(lua_State* L) {
    auto* e = check_entity(L);
    lua_pushnumber(L, e ? e->health() : 0);
    return 1;
}

// faf-re Entity::AdjustHealth; Lua's SetHealth goes through it too.
static void adjust_health(sim::Entity& e, f32 delta) {
    if (delta == 0.0f) {
        return;
    }
    e.set_health(std::min(e.health() + delta, e.max_health()));
}

static int entity_SetHealth(lua_State* L) {
    auto* e = check_entity(L);
    if (!e) return 0;
    // SetHealth(self, instigator, health)
    f32 h = static_cast<f32>(lua_tonumber(L, 3));
    adjust_health(*e, h - e->health());
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
    adjust_health(*e, delta);
    return 0;
}

static int entity_GetEntityId(lua_State* L) {
    auto* e = check_entity(L);
    sim::push_entity_id(L, e ? e->entity_id() : 0);
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

// Moho's Entity::Destroy queues it: SimState::run_deletion_queue runs its OnDestroy as the tick
// ends. A handle without a ref could not be found then, so it is cut now.
int entity_Destroy(lua_State* L) {
    auto* e = check_entity(L);
    auto* sim = get_sim(L);
    if (!e || !sim || e->destroyed()) {
        return 0;
    }
    if (e->lua_table_ref() < 0) {
        lua_pushstring(L, "_c_object");
        lua_pushlightuserdata(L, nullptr);
        lua_rawset(L, 1);
    }
    sim->entity_registry().destroy(*e);
    return 0;
}

// Moho's cfunc_EntityKillL (faf-re Entity.cpp): after the kill, a unit
// instigator's KILLS stat counts an enemy not BENIGN or being built.
// Retail's veterancy (Unit.lua CheckVeteranLevel) reads it.
static u32 kill_credit_for(lua_State* L, const sim::Unit* victim) {
    const auto* instigator = check_entity(L, 2);
    static const sim::CategoryName kBenign{"BENIGN"};
    if (!instigator || !instigator->is_unit() || instigator->army() == victim->army() ||
        victim->is_being_built() || victim->has_category(kBenign)) {
        return 0;
    }
    return instigator->entity_id();
}

static void credit_kill(lua_State* L, u32 instigator_id) {
    auto* sim = get_sim(L);
    auto* e = (instigator_id != 0 && sim) ? sim->entity_registry().find(instigator_id) : nullptr;
    if (e && e->is_unit()) {
        auto* u = static_cast<sim::Unit*>(e);
        u->set_stat("KILLS", u->get_stat("KILLS", 0) + 1);
    }
}

namespace {

/// target:method(arg) on an entity's script, an error logged (Moho's RunScriptEntity).
void call_with_entity(lua_State* L, sim::EntityRegistry& registry, u32 target_id,
                      const char* method, u32 arg_id) {
    const sim::Entity* target = registry.find(target_id);
    if (!target || target->lua_table_ref() < 0) return;
    const int top = lua_gettop(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, target->lua_table_ref());
    const int self = lua_gettop(L);
    lua_pushstring(L, method);
    lua_gettable(L, self);
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, self);
        const sim::Entity* arg = registry.find(arg_id);
        if (arg && arg->lua_table_ref() >= 0) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, arg->lua_table_ref());
        } else {
            lua_pushnil(L);
        }
        if (lua_pcall(L, 2, 0, 0) != 0) {
            sim::log_script_warning(registry.find(target_id), method, lua_tostring(L, -1));
        }
    }
    lua_settop(L, top);
}

/// What `e` holds: what is attached to it, then the cargo it carries.
std::vector<u32> held_by(const sim::Entity& e) {
    std::vector<u32> ids;
    for (const auto& c : e.children()) ids.push_back(c.entity_id);
    if (e.is_unit()) {
        for (const u32 id : static_cast<const sim::Unit&>(e).cargo_ids()) ids.push_back(id);
    }
    return ids;
}

/// Moho's Entity::Kill (faf-re Entity.cpp, 0x00679A80): what `id` is attached to hears
/// OnAttachedKilled(it), then each thing attached to it OnParentKilled(it), the list read again
/// at each step (a script may detach one). A transport still holding a unit counts as what it is
/// attached to, and a transport's cargo as attached to it.
void notify_kill(lua_State* L, sim::EntityRegistry& registry, u32 id) {
    const sim::Entity* e = registry.find(id);
    if (!e) return;
    u32 parent = e->parent_entity_id();
    if (parent == 0 && e->is_unit()) parent = static_cast<const sim::Unit*>(e)->transport_id();
    if (parent != 0) call_with_entity(L, registry, parent, "OnAttachedKilled", id);
    for (size_t i = 0;; ++i) {
        e = registry.find(id);
        if (!e) return;
        const std::vector<u32> held = held_by(*e);
        if (i >= held.size()) return;
        call_with_entity(L, registry, held[i], "OnParentKilled", id);
    }
}

} // namespace

// entity:Kill([instigator, damageType, excessDamageRatio]). Moho hands the
// death to the script's OnKilled, which plays the death sequence (death
// weapon, animation, wreckage) and calls Destroy() when it ends. A unit is
// dead from here on (IsDead; no orders, weapons or economy; one in flight
// falls) and its death is counted after OnKilled. Without an OnKilled the
// entity is destroyed. SetCanBeKilled(false) blocks it.
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
    // Moho's cfunc_EntityKillL: a bare Kill() is (nil, '', 0)
    if (lua_type(L, 3) != LUA_TSTRING) {
        lua_pushstring(L, "");
        lua_replace(L, 3);
    }
    if (!lua_isnumber(L, 4)) {
        lua_pushnumber(L, 0);
        lua_replace(L, 4);
    }
    const u32 credit = e->is_unit() ? kill_credit_for(L, static_cast<sim::Unit*>(e)) : 0;
    const sim::Entity* by = check_entity(L, 2);
    const i32 killer_army = by ? by->army() : -1;
    auto* sim = get_sim(L);
    auto* unit = e->is_unit() ? static_cast<sim::Unit*>(e) : nullptr;
    // Moho's Unit::Kill (faf-re unit/core/Unit.cpp:16072).
    if (unit) {
        sim::Entity* carrier = sim && unit->transport_id() != 0
                                   ? sim->entity_registry().find(unit->transport_id())
                                   : nullptr;
        if (carrier && carrier->is_unit()) {
            // CAiTransportImpl::TransportDetachUnit: an aircraft keeps a unit whose footprint
            // can't stand where it hangs; it hears OnAttachedKilled below.
            auto* transport = static_cast<sim::Unit*>(carrier);
            const bool held = transport->layer() == "Air" &&
                              sim->footprint_fits_at(unit->footprint(), unit->position().x,
                                                     unit->position().z) == 0;
            if (!held) transport->detach_cargo({unit->entity_id()}, sim->entity_registry(), L);
        }
        if (carrier || (unit->is_being_built() && unit->fraction_complete() < 0.5f)) {
            lua_pushnumber(L, 10);
            lua_replace(L, 4);
        }
        unit->call_lua_method(L, "SetDead");
        if (sim) notify_kill(L, sim->entity_registry(), unit->entity_id());
        unit->begin_dying();
        if (sim) {
            sim->drop_adjacency(*unit);
            sim->detach_all_units(*unit, true);
        }
        unit->clear_commands();
    }
    lua_pushstring(L, "OnKilled");
    lua_gettable(L, 1);
    if (!lua_isfunction(L, -1)) {
        lua_settop(L, 1);
        entity_Destroy(L);
    } else {
        e->set_script_owns_death();
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
            if (test_status::count_lua_failures()) {
                test_status::record_failure(message);
            }
            lua_settop(L, 1);
            if (auto* still = check_entity(L); still && !still->destroyed()) {
                entity_Destroy(L);
            }
        }
    }
    if (unit && sim) {
        sim->record_unit_death(*unit, killer_army);
    }
    credit_kill(L, credit);
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

static u32 launcher_of(const sim::Entity& e) {
    if (e.is_projectile()) {
        return static_cast<const sim::Projectile&>(e).launcher_id;
    }
    return e.entity_id();
}

/// What a projectile blueprint's Physics launches a projectile a script makes with (faf-re
/// Entity.cpp's CreateProjectile, RProjectileBlueprint.cpp): its offset from its maker
/// (Position{X,Y,Z}) and its direction (Direction{X,Y,Z}) where the script gives none, and its
/// speed (InitialSpeed), each moved by up to its *Range either way. A field the blueprint leaves
/// out is RProjectileBlueprintPhysics's default (0x0051B650): up, spread 1.5 either way across,
/// at 1.
struct ProjectileLaunch {
    sim::Vector3 offset{};
    sim::Vector3 offset_range{};
    sim::Vector3 direction{0, 1, 0};
    sim::Vector3 direction_range{1.5f, 0, 1.5f};
    f32 speed = 1;
    f32 speed_range = 0;
};

/// Nothing for an id with no blueprint (Moho's "Invalid blueprint" error): the
/// projectile is made unlaunched.
static std::optional<ProjectileLaunch> projectile_launch(lua_State* L, const std::string& bp_id) {
    const int top = lua_gettop(L);
    if (bp_id.empty() || !blueprints::push_blueprint(L, bp_id)) return std::nullopt;
    ProjectileLaunch launch;
    lua_pushstring(L, "Physics");
    lua_rawget(L, -2);
    if (lua_istable(L, -1)) {
        const int physics = lua_gettop(L);
        const auto number = [&](const char* key, f32 absent = 0.0f) {
            lua_pushstring(L, key);
            lua_rawget(L, physics);
            const f32 value = lua_isnumber(L, -1) ? static_cast<f32>(lua_tonumber(L, -1)) : absent;
            lua_pop(L, 1);
            return value;
        };
        launch.offset = {number("PositionX"), number("PositionY"), number("PositionZ")};
        launch.offset_range = {number("PositionXRange"), number("PositionYRange"),
                               number("PositionZRange")};
        launch.direction = {number("DirectionX"), number("DirectionY", 1.0f), number("DirectionZ")};
        launch.direction_range = {number("DirectionXRange", 1.5f), number("DirectionYRange"),
                                  number("DirectionZRange", 1.5f)};
        launch.speed = number("InitialSpeed", 1.0f);
        launch.speed_range = number("InitialSpeedRange");
    }
    lua_settop(L, top);
    return launch;
}

/// `base` moved by up to `range` either way, uniformly (Moho's FRand(-range, range) + base): a
/// draw only for a range there is, as the engine's other spreads take
static f32 spread(f32 base, f32 range, sim::SimRandom* rng) {
    return rng && range != 0.0f ? base + rng->range(-range, range) : base;
}

/// Per axis, x then y then z
static sim::Vector3 spread(const sim::Vector3& base, const sim::Vector3& range,
                           sim::SimRandom* rng) {
    const f32 x = spread(base.x, range.x, rng);
    const f32 y = spread(base.y, range.y, rng);
    return {x, y, spread(base.z, range.z, rng)};
}

/// Point a new projectile along `dir`, at `speed` (Moho's COORDS_Orient of its launch direction,
/// its velocity forward); a zero direction leaves it as it was.
static void aim_projectile(sim::Projectile& p, const sim::Vector3& dir, f32 speed) {
    const f32 len = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    if (len < 1e-6f) return;
    const sim::Vector3 d{dir.x / len, dir.y / len, dir.z / len};
    p.set_orientation(sim::euler_to_quat(osc::dmath::atan2(d.x, d.z),
                                         osc::dmath::atan2(-d.y, std::sqrt(d.x * d.x + d.z * d.z)),
                                         0.0f));
    p.velocity = {d.x * speed, d.y * speed, d.z * speed};
}

// entity:CreateProjectile(bp, offX, offY, offZ, dirX, dirY, dirZ): a
// projectile at the entity's centre plus the offset, launched by it, or by
// its launcher if it is a projectile (faf-re Projectile::Projectile). An offset
// or a direction left nil (or out) is its blueprint's, each axis spread by its
// range (Moho's BuildRandomProjectileOffset, BuildRandomDirectionVector), and
// it leaves along the direction at InitialSpeed spread by InitialSpeedRange
// (GetRandomInitialSpeed): retail's death sparks, fire and plumes fly up and out.
static int entity_CreateProjectile(lua_State* L) {
    auto* e = check_entity(L);
    if (!e || e->destroyed()) { lua_pushnil(L); return 1; }

    auto* sim = get_sim(L);
    if (!sim) { lua_pushnil(L); return 1; }

    auto proj = std::make_unique<sim::Projectile>();
    proj->set_blueprint_id(lowercase_arg(L, 2));
    sim::SimRandom* rng = &sim->entity_registry().sim_random();
    const auto launch = projectile_launch(L, proj->blueprint_id());
    const auto given = [&](int idx) {
        return sim::Vector3{static_cast<f32>(luaL_optnumber(L, idx, 0)),
                            static_cast<f32>(luaL_optnumber(L, idx + 1, 0)),
                            static_cast<f32>(luaL_optnumber(L, idx + 2, 0))};
    };
    const sim::Vector3 offset = !lua_isnoneornil(L, 3) ? given(3)
                                : launch ? spread(launch->offset, launch->offset_range, rng)
                                         : sim::Vector3{};
    const sim::Vector3 direction = !lua_isnoneornil(L, 6) ? given(6)
                                   : launch
                                       ? spread(launch->direction, launch->direction_range, rng)
                                       : sim::Vector3{};
    const f32 speed = launch ? spread(launch->speed, launch->speed_range, rng) : 0.0f;
    const sim::Vector3 at = e->position();
    proj->set_position({at.x + offset.x, at.y + offset.y, at.z + offset.z});
    proj->set_army(e->army());
    proj->launcher_id = launcher_of(*e);
    apply_script_projectile_physics(L, *proj);
    aim_projectile(*proj, direction, speed);

    u32 proj_id = sim->entity_registry().register_entity(std::move(proj));
    auto* proj_ptr = static_cast<sim::Projectile*>(
        sim->entity_registry().find(proj_id));

    // Its script object, as for every projectile (OnCreate runs).
    sim::create_projectile_object(L, *proj_ptr, under_water(sim, proj_ptr->position()), true);
    return 1;
}

// entity:CreateProjectileAtBone(bp, bone): a projectile at the bone, facing
// as it does (a unit's as posed), launched by its blueprint's direction, each
// axis spread by its range, or along the bone where that comes to nothing, at
// InitialSpeed spread by InitialSpeedRange. As Moho's, the velocity is the
// direction times the speed, not made a unit first. Retail tosses a destroyed
// unit's parts this way, up and out (DebrisBoneAttach*).
static int entity_CreateProjectileAtBone(lua_State* L) {
    auto* e = check_entity(L);
    if (!e || e->destroyed()) { lua_pushnil(L); return 1; }

    auto* sim = get_sim(L);
    if (!sim) { lua_pushnil(L); return 1; }

    const i32 bone_idx = resolve_bone_index(e, L, 3);
    auto proj = std::make_unique<sim::Projectile>();
    proj->set_blueprint_id(lowercase_arg(L, 2));
    sim::SimRandom* rng = &sim->entity_registry().sim_random();
    const auto launch = projectile_launch(L, proj->blueprint_id());
    sim::Vector3 direction =
        launch ? spread(launch->direction, launch->direction_range, rng) : sim::Vector3{};
    const f32 speed = launch ? spread(launch->speed, launch->speed_range, rng) : 0.0f;
    proj->set_position(bone_world_position(e, bone_idx));
    proj->set_army(e->army());
    proj->launcher_id = launcher_of(*e);
    apply_script_projectile_physics(L, *proj);
    const auto* bd = e->bone_data();
    const sim::Quaternion bone_world_rot =
        e->is_unit() ? static_cast<const sim::Unit*>(e)->bone_world_rotation(bone_idx)
        : bd && bd->is_valid(bone_idx)
            ? sim::quat_multiply(e->orientation(),
                                 bd->bones[static_cast<size_t>(bone_idx)].world_rotation)
            : e->orientation();
    proj->set_orientation(bone_world_rot);
    if (direction.x == 0.0f && direction.y == 0.0f && direction.z == 0.0f)
        direction = sim::quat_rotate(bone_world_rot, {0, 0, 1});
    proj->velocity = {direction.x * speed, direction.y * speed, direction.z * speed};

    u32 proj_id = sim->entity_registry().register_entity(std::move(proj));
    auto* proj_ptr = static_cast<sim::Projectile*>(
        sim->entity_registry().find(proj_id));

    // Its script object, as for every projectile (OnCreate runs).
    sim::create_projectile_object(L, *proj_ptr, under_water(sim, proj_ptr->position()), true);
    return 1;
}

static sim::Entity* scroller_entity(lua_State* L, const char* help, int args) {
    const int n = lua_gettop(L);
    if (n != args) {
        arg_count_error(L, help, args, n);
    }
    return check_entity(L);
}

static f32 scroller_arg(lua_State* L, int i) {
    return static_cast<f32>(luaL_checknumber(L, i));
}

static int entity_AddManualScroller(lua_State* L) {
    auto* e = scroller_entity(L, "Entity:AddManualScroller(scrollSpeed1, scrollSpeed2)", 3);
    if (!e) {
        return 0;
    }
    sim::ScrollerSpec spec;
    spec.type = sim::ScrollType::Manual;
    spec.rate = {scroller_arg(L, 2), scroller_arg(L, 3)};
    e->change_scroller(spec);
    return 0;
}

static int entity_AddThreadScroller(lua_State* L) {
    auto* e = scroller_entity(L, "Entity:AddThreadScroller(sideDist, scrollMult)", 3);
    if (!e) {
        return 0;
    }
    sim::ScrollerSpec spec;
    spec.type = sim::ScrollType::MotionDerived;
    spec.side_dist = scroller_arg(L, 2);
    spec.scroll_mult = scroller_arg(L, 3);
    e->change_scroller(spec);
    return 0;
}

static int entity_AddPingPongScroller(lua_State* L) {
    auto* e = scroller_entity(L,
                              "Entity:AddPingPongScroller(ping1, pingSpeed1, pong1, pongSpeed1, "
                              "ping2, pingSpeed2, pong2, pongSpeed2)",
                              9);
    if (!e) {
        return 0;
    }
    sim::ScrollerSpec spec;
    spec.type = sim::ScrollType::PingPong;
    spec.ping = {scroller_arg(L, 2), scroller_arg(L, 6)};
    spec.pong = {scroller_arg(L, 4), scroller_arg(L, 8)};
    spec.ping_seconds[0] = scroller_arg(L, 3);
    spec.pong_seconds[0] = scroller_arg(L, 5);
    spec.ping_seconds[1] = scroller_arg(L, 7);
    spec.pong_seconds[1] = scroller_arg(L, 9);
    e->change_scroller(spec);
    return 0;
}

static int entity_RemoveScroller(lua_State* L) {
    auto* e = scroller_entity(L, "Entity:RemoveScroller()", 1);
    if (!e) {
        return 0;
    }
    e->change_scroller({});
    return 0;
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
    const auto x = static_cast<f32>(luaL_checknumber(L, 2));
    const auto y = lua_isnumber(L, 3) ? static_cast<f32>(lua_tonumber(L, 3)) : x;
    const auto z = lua_isnumber(L, 4) ? static_cast<f32>(lua_tonumber(L, 4)) : x;
    const f32 per = draw_scale_unit(L, *e);
    e->set_scale(x * per, y * per, z * per);
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

/// The parent bone an attachment names: bone -1 (any negative) is the
/// parent itself, a unit's centre (Moho's GetBoneWorldTransform(-1); M211k),
/// not its root bone as resolve_bone_index takes it.
static i32 attach_bone_index(const sim::Entity* parent, lua_State* L, int arg) {
    if (lua_type(L, arg) == LUA_TNUMBER && lua_tonumber(L, arg) < 0) return -1;
    return resolve_bone_index(parent, L, arg);
}

// Moho's Unit::AttachTo and Unit::DetachFrom.
static void set_attached(sim::Entity& e, bool attached) {
    if (e.is_unit() && static_cast<sim::Unit&>(e).is_mobile()) {
        static_cast<sim::Unit&>(e).set_unit_state("Attached", attached);
    }
}

static int entity_AttachTo(lua_State* L) {
    auto* self = check_entity(L); if (!self) return 0;
    auto* parent = check_entity_arg(L, 2); if (!parent) return 0;
    i32 bone = attach_bone_index(parent, L, 3);
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
    set_attached(*self, true);
    return 0;
}

static int entity_AttachBoneTo(lua_State* L) {
    auto* self = check_entity(L); if (!self) return 0;
    i32 self_bone = resolve_bone_index(self, L, 2);
    auto* parent = check_entity_arg(L, 3); if (!parent) return 0;
    i32 parent_bone = attach_bone_index(parent, L, 4);
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
    set_attached(*self, true);
    return 0;
}

// unit:AttachBoneToEntityBone(unitBone, entity, entityBone, skipInterp): a
// manipulator on the unit (faf-re cfunc_EntityAttachBoneToEntityBoneL).
static int entity_AttachBoneToEntityBone(lua_State* L) {
    const int n = lua_gettop(L);
    if (n != 5) {
        return arg_count_error(L, "Attach a unit bone position to an entity bone position", 5, n);
    }
    auto* self = check_entity(L);
    if (!self || !self->is_unit()) {
        return luaL_error(L, "Incorrect type of game object.  (Did you call with '.' instead of "
                             "':'?)");
    }
    auto* unit = static_cast<sim::Unit*>(self);
    auto* target = check_entity_arg(L, 3);
    if (!target) {
        return luaL_error(L, "Expected a game object. (Did you call with '.' instead of ':'?)");
    }
    auto manip = std::make_unique<sim::BoneEntityManipulator>(get_sim(L), target->entity_id(),
                                                              attach_bone_index(target, L, 4));
    manip->set_bone_index(resolve_bone_index(unit, L, 2));
    auto* raw = unit->add_manipulator(std::move(manip));
    lua_newtable(L);
    const int tbl = lua_gettop(L);
    lua_pushstring(L, "_c_object");
    lua_pushlightuserdata(L, raw);
    lua_rawset(L, tbl);
    set_manip_metatable(L, tbl, "__osc_bone_entity_mt", "BoneEntityManipulator");
    return 1;
}

static void released(sim::Entity& e, lua_State* L) {
    auto* sim = get_sim(L);
    if (!e.is_unit() || !sim) {
        return;
    }
    auto& unit = static_cast<sim::Unit&>(e);
    if (unit.is_air_unit()) {
        unit.fly_on_from_here(sim->terrain());
    }
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
        set_attached(*self, false);
        released(*self, L);
    } else if (self->is_unit() && static_cast<sim::Unit*>(self)->transport_id() != 0) {
        // Cargo is attached to its transport (Moho's Entity::DetachFrom): it lets go, as retail's
        // transports' OnAttachedKilled has one let go of a unit killed where it couldn't stand.
        auto* sim = get_sim(L);
        auto* unit = static_cast<sim::Unit*>(self);
        sim::Entity* transport = sim ? sim->entity_registry().find(unit->transport_id()) : nullptr;
        if (transport && transport->is_unit()) {
            static_cast<sim::Unit*>(transport)->detach_cargo(
                {unit->entity_id()}, sim->entity_registry(), L, sim->terrain());
        }
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
        if (child) {
            child->clear_parent();
            set_attached(*child, false);
            released(*child, L);
        }
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
    // A new handle, off until EnableIntel (Moho's InitIntel)
    intel.sources[lua_tostring(L, 3)] = {static_cast<f32>(luaL_optnumber(L, 4, 0)), false};
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
    {"GetCollisionExtents", entity_GetCollisionExtents},
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
    {"AddManualScroller",       entity_AddManualScroller},
    {"AddPingPongScroller",     entity_AddPingPongScroller},
    {"AddThreadScroller",       entity_AddThreadScroller},
    {"RemoveScroller",          entity_RemoveScroller},
    {"RequestRefreshUI",        entity_RequestRefreshUI},
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
    if (e && e->is_prop()) {
        static_cast<sim::Prop*>(e)->sink_away(static_cast<f32>(lua_tonumber(L, 2)));
    }
    return 0;
}

// motor:Whack(nx, ny, nz, force, dobreak) -> motor
static int falldown_Whack(lua_State* L) {
    if (!lua_istable(L, 1)) return 0;
    lua_pushstring(L, "_c_prop_id");
    lua_rawget(L, 1);
    const auto id = static_cast<u32>(lua_tonumber(L, -1));
    lua_pop(L, 1);
    auto* sim = get_sim(L);
    sim::Entity* e = sim ? sim->entity_registry().find(id) : nullptr;
    if (e && !e->destroyed() && e->is_prop()) {
        static_cast<sim::Prop*>(e)->whack(
            static_cast<f32>(lua_tonumber(L, 2)), static_cast<f32>(lua_tonumber(L, 4)),
            static_cast<f32>(lua_tonumber(L, 5)), lua_toboolean(L, 6) != 0);
    }
    lua_settop(L, 1);
    return 1;
}

// prop:FallDown() -> motor: whacking it topples the tree. The motor is a
// moho.MotorFallDown, as Moho's is.
static int prop_FallDown(lua_State* L) {
    auto* e = check_entity(L);
    if (e && e->is_prop()) {
        lua_pushstring(L, "Blueprint");
        lua_rawget(L, 1);
        f32 size_x = 0;
        if (lua_istable(L, -1)) {
            lua_pushstring(L, "SizeX");
            lua_rawget(L, -2);
            size_x = static_cast<f32>(lua_tonumber(L, -1));
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
        static_cast<sim::Prop*>(e)->fall_down(size_x);
    }
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
        lua_pushstring(L, "moho");
        lua_rawget(L, LUA_GLOBALSINDEX);
        if (lua_istable(L, -1)) {
            lua_pushstring(L, "MotorFallDown");
            lua_rawget(L, -2);
            lua_remove(L, -2);
        }
        if (!lua_istable(L, -1)) { // (no moho table: the method alone)
            lua_pop(L, 1);
            lua_newtable(L);
            lua_pushstring(L, "Whack");
            lua_pushcfunction(L, falldown_Whack);
            lua_rawset(L, -3);
        }
        lua_rawset(L, -3);
        lua_pushstring(L, "__osc_falldown_mt");
        lua_pushvalue(L, -2);
        lua_rawset(L, LUA_REGISTRYINDEX);
    }
    lua_setmetatable(L, -2);
    return 1;
}

// clang-format off
const MethodEntry motor_falldown_methods[] = {
    {"Whack",                        falldown_Whack},
    {nullptr, nullptr}
};
// clang-format on

// faf-re cfunc_PropAddBoundedPropL and EntityDB::AddBoundedProp.
static int prop_AddBoundedProp(lua_State* L) {
    const int n = lua_gettop(L);
    if (n != 2) {
        return arg_count_error(L, "Prop:AddBoundedProp(priority)", 2, n);
    }
    auto* e = check_entity(L);
    if (!e || !e->is_prop()) {
        return luaL_error(L, "Incorrect type of game object.  (Did you call with '.' instead of "
                             "':'?)");
    }
    if (lua_type(L, 2) != LUA_TNUMBER) {
        luaL_typerror(L, 2, "number");
    }
    auto* sim = get_sim(L);
    if (!sim) {
        return 0;
    }
    auto* prop = static_cast<sim::Prop*>(e);
    auto& bounded = sim->bounded_props();
    if (prop->bounded_handle != -1) {
        bounded.remove(prop->bounded_handle);
        prop->bounded_handle = -1;
    }
    prop->bounded_priority = static_cast<i32>(std::ceil(static_cast<f32>(lua_tonumber(L, 2))));
    prop->bounded_tick = static_cast<i32>(sim->tick_count());
    while (bounded.size() >= sim::BoundedProps::kLimit) {
        sim::Prop* evicted = bounded.lowest();
        bounded.pop_lowest();
        evicted->bounded_handle = -1;
        if (evicted->destroyed()) {
            continue;
        }
        sim->entity_registry().destroy(*evicted);
    }
    prop->bounded_handle = bounded.insert(prop->bounded_priority, prop->bounded_tick, prop);
    return 0;
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
    {"AddBoundedProp",              prop_AddBoundedProp},
    {"SetCollisionShape",           entity_SetCollisionShape},
    {"GetCollisionExtents",         entity_GetCollisionExtents},
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
