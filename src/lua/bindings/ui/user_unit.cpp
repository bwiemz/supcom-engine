// UserUnit (M191 step 3): the UI state's unit objects. Moho's user side sees
// a unit as the sim last reported it, a tick at a time, and changes it only
// through its command stream. These methods read the tick's WorldSnapshot,
// captured for the UI the first time it asks after each tick; ProcessInfo
// and SetCustomName queue their change as a SimCallback. Two methods no
// retail UI script calls, GetStat and CanAttackTarget, still read the live
// unit (read-only): the snapshot doesn't carry a unit's stats or weapons.

#include "lua/category_utils.hpp"
#include "lua/lua_state.hpp"
#include "lua/moho_bindings_internal.hpp"
#include "blueprints/blueprint_store.hpp"
#include "sim/blueprint_categories.hpp"
#include "sim/sim_callback_queue.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/weapon.hpp"
#include "sim/world_snapshot.hpp"

#include <algorithm>
#include <new>
#include <string>
#include <unordered_set>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace osc::lua {

namespace {

constexpr const char* kUiWorldKey = "__osc_ui_world";
constexpr const char* kUiWorldSourceKey = "__osc_ui_world_source";
constexpr const char* kSelectionSetsKey = "__osc_selection_sets";

/// The UI's copy of the world, and the tick it shows.
struct UiWorld {
    u32 generation = 0;
    u32 tick = 0;
    bool valid = false;
    sim::WorldSnapshot snapshot;
};

int ui_world_gc(lua_State* L) {
    static_cast<UiWorld*>(lua_touserdata(L, 1))->~UiWorld();
    return 0;
}

/// The UI state's UiWorld, a userdata the registry keeps (and Lua frees).
UiWorld& ui_world_holder(lua_State* L) {
    lua_pushstring(L, kUiWorldKey);
    lua_rawget(L, LUA_REGISTRYINDEX);
    auto* world = static_cast<UiWorld*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    if (world) return *world;
    world = new (lua_newuserdata(L, sizeof(UiWorld))) UiWorld();
    lua_newtable(L);
    lua_pushstring(L, "__gc");
    lua_pushcfunction(L, ui_world_gc);
    lua_rawset(L, -3);
    lua_setmetatable(L, -2);
    lua_pushstring(L, kUiWorldKey);
    lua_insert(L, -2);
    lua_rawset(L, LUA_REGISTRYINDEX);
    return *world;
}

/// The unit object's id, or 0 for a handle from an earlier game.
u32 user_unit_id(lua_State* L, int idx = 1) {
    if (!lua_istable(L, idx)) return 0;
    lua_pushstring(L, "_c_sim_gen");
    lua_rawget(L, idx);
    const bool stale = lua_isnumber(L, -1) &&
                       static_cast<u32>(lua_tonumber(L, -1)) != sim::SimState::sim_generation();
    lua_pop(L, 1);
    if (stale) return 0;
    lua_pushstring(L, "_c_entity_id");
    lua_rawget(L, idx);
    const u32 id = lua_isnumber(L, -1) ? static_cast<u32>(lua_tonumber(L, -1)) : 0u;
    lua_pop(L, 1);
    return id;
}

/// The unit as the UI's tick shows it: null once it is gone.
const sim::EntityRecord* record(lua_State* L, int idx = 1) {
    const u32 id = user_unit_id(L, idx);
    const sim::WorldSnapshot* world = id ? ui_world(L) : nullptr;
    const sim::EntityRecord* r = world ? world->find(id) : nullptr;
    return r && r->is_unit ? r : nullptr;
}

/// The unit `id` if the UI's tick shows it, as a unit object; else nil.
void push_user_unit_or_nil(lua_State* L, u32 id) {
    const sim::WorldSnapshot* world = id ? ui_world(L) : nullptr;
    const sim::EntityRecord* r = world ? world->find(id) : nullptr;
    if (r && r->is_unit) push_user_unit(L, id, r->army);
    else lua_pushnil(L);
}

bool push_blueprint(lua_State* L, const std::string& bp_id) {
    auto* store = LuaState::get_blueprint_store(L);
    auto* entry = store && !bp_id.empty() ? store->find(bp_id) : nullptr;
    if (!entry) return false;
    store->push_lua_table(*entry, L);
    if (lua_istable(L, -1)) return true;
    lua_pop(L, 1);
    return false;
}

/// Queue a ProcessInfo pair for the unit, as Moho's UserUnit sends it.
void queue_process_info(lua_State* L, u32 id, const std::string& action, const char* value) {
    auto* queue = get_callback_queue(L);
    if (!queue || id == 0) return;
    sim::SimCallbackEntry entry;
    entry.func_name = sim::kProcessInfoCallback;
    entry.args["Action"] = action;
    if (value) entry.args["Value"] = std::string(value);
    entry.unit_ids.push_back(id);
    queue->push(std::move(entry));
}

/// This unit's selection-set table (name -> true), made if `create`; else
/// nil when it has none. The UI's own bookkeeping.
void push_selection_sets(lua_State* L, u32 id, bool create) {
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

template <typename T> int push_number_of(lua_State* L, T(sim::EntityRecord::* field), T absent) {
    const auto* r = record(L);
    lua_pushnumber(L, static_cast<lua_Number>(r ? r->*field : absent));
    return 1;
}

int push_bool_of(lua_State* L, bool(sim::EntityRecord::* field)) {
    const auto* r = record(L);
    lua_pushboolean(L, r && r->*field ? 1 : 0);
    return 1;
}

// --- Identity -------------------------------------------------------------

int uu_GetEntityId(lua_State* L) {
    lua_pushnumber(L, user_unit_id(L));
    return 1;
}

int uu_GetArmy(lua_State* L) {
    const auto* r = record(L);
    lua_pushnumber(L, r && r->army >= 0 ? r->army + 1 : -1); // 1-based; -1 for none
    return 1;
}

int uu_GetBlueprint(lua_State* L) {
    const auto* r = record(L);
    if (!r || !push_blueprint(L, r->blueprint_id)) lua_pushnil(L);
    return 1;
}

int uu_GetUnitId(lua_State* L) {
    const auto* r = record(L);
    lua_pushstring(L, r ? r->unit_id.c_str() : "");
    return 1;
}

int uu_GetCustomName(lua_State* L) {
    const auto* r = record(L);
    if (r && !r->custom_name.empty()) lua_pushstring(L, r->custom_name.c_str());
    else lua_pushnil(L);
    return 1;
}

/// A name the player gives: it reaches the sim as a ProcessInfo pair, so
/// every peer and a replay name the unit at the same tick (M191).
int uu_SetCustomName(lua_State* L) {
    if (lua_type(L, 2) == LUA_TSTRING)
        queue_process_info(L, user_unit_id(L), "CustomName", lua_tostring(L, 2));
    return 0;
}

/// unit:ProcessInfo(action, value): a change of the unit's settings
/// (SetAutoMode, SetRepeatQueue, ...), sent through the command stream.
int uu_ProcessInfo(lua_State* L) {
    if (lua_type(L, 2) == LUA_TSTRING)
        queue_process_info(L, user_unit_id(L), lua_tostring(L, 2),
                           lua_isstring(L, 3) ? lua_tostring(L, 3) : nullptr);
    return 0;
}

int uu_IsInCategory(lua_State* L) {
    const auto* r = record(L);
    bool in = false;
    if (r && push_blueprint(L, r->blueprint_id)) {
        std::unordered_set<std::string> categories;
        sim::collect_blueprint_categories(L, lua_gettop(L), categories);
        lua_pop(L, 1);
        if (lua_type(L, 2) == LUA_TSTRING) in = categories.count(lua_tostring(L, 2)) > 0;
        else if (lua_istable(L, 2)) in = unit_matches_category(L, 2, categories);
    }
    lua_pushboolean(L, in ? 1 : 0);
    return 1;
}

// --- State ----------------------------------------------------------------

int uu_GetPosition(lua_State* L) {
    const auto* r = record(L);
    push_vector3(L, r ? r->position : sim::Vector3{0, 0, 0});
    return 1;
}

int uu_GetHealth(lua_State* L) {
    return push_number_of(L, &sim::EntityRecord::health, 0.0f);
}
int uu_GetMaxHealth(lua_State* L) {
    return push_number_of(L, &sim::EntityRecord::max_health, 0.0f);
}
int uu_GetWorkProgress(lua_State* L) {
    return push_number_of(L, &sim::EntityRecord::work_progress, 0.0f);
}
int uu_GetFuelRatio(lua_State* L) {
    return push_number_of(L, &sim::EntityRecord::fuel_ratio, -1.0f);
}
int uu_GetShieldRatio(lua_State* L) {
    return push_number_of(L, &sim::EntityRecord::shield_ratio, 1.0f);
}
int uu_GetBuildRate(lua_State* L) {
    return push_number_of(L, &sim::EntityRecord::build_rate, 0.0f);
}
int uu_GetFootPrintSize(lua_State* L) {
    const auto* r = record(L);
    lua_pushnumber(L, r ? std::max(r->footprint_size_x, r->footprint_size_z) : 1.0f);
    return 1;
}

int uu_IsAutoMode(lua_State* L) {
    return push_bool_of(L, &sim::EntityRecord::auto_mode);
}
int uu_IsRepeatQueue(lua_State* L) {
    return push_bool_of(L, &sim::EntityRecord::repeat_queue);
}
int uu_IsOverchargePaused(lua_State* L) {
    return push_bool_of(L, &sim::EntityRecord::overcharge_paused);
}
int uu_IsAutoSurfaceMode(lua_State* L) {
    return push_bool_of(L, &sim::EntityRecord::auto_surface);
}

int uu_IsStunned(lua_State* L) {
    lua_pushboolean(L, 0); // nothing stuns a unit yet
    return 1;
}

int uu_IsDead(lua_State* L) {
    const auto* r = record(L);
    lua_pushboolean(L, !r || r->is_dying ? 1 : 0);
    return 1;
}

int uu_IsIdle(lua_State* L) {
    const auto* r = record(L);
    lua_pushboolean(L, r && r->command_count == 0 && !r->is_building() && !r->is_being_built &&
                               !r->is_repairing() && !r->is_capturing()
                           ? 1
                           : 0);
    return 1;
}

int uu_GetEconData(lua_State* L) {
    const auto* r = record(L);
    lua_newtable(L);
    if (!r) return 1;
    const auto set = [&](const char* key, f32 value) {
        lua_pushstring(L, key);
        lua_pushnumber(L, value);
        lua_rawset(L, -3);
    };
    set("massProduced", r->mass_produced);
    set("energyProduced", r->energy_produced);
    set("massConsumed", r->mass_consumed);
    set("energyConsumed", r->energy_consumed);
    set("massRequested", r->mass_requested);
    set("energyRequested", r->energy_requested);
    return 1;
}

int uu_GetMissileInfo(lua_State* L) {
    const auto* r = record(L);
    lua_newtable(L);
    if (!r) return 1;
    const auto set = [&](const char* key, i32 value) {
        lua_pushstring(L, key);
        lua_pushnumber(L, value);
        lua_rawset(L, -3);
    };
    set("nukeSiloStorageCount", r->nuke_silo_ammo);
    set("nukeSiloMaxStorageCount", r->nuke_silo_max);
    set("nukeSiloBuildCount", r->nuke_silo_builds);
    set("tacticalSiloStorageCount", r->tactical_silo_ammo);
    set("tacticalSiloMaxStorageCount", r->tactical_silo_max);
    set("tacticalSiloBuildCount", r->tactical_silo_builds);
    return 1;
}

// --- Orders and other units ----------------------------------------------

int uu_GetCommandQueue(lua_State* L) {
    const auto* r = record(L);
    const sim::WorldSnapshot* world = r ? ui_world(L) : nullptr;
    lua_newtable(L);
    if (!world) return 1;
    int n = 1;
    for (const auto& c : world->commands_of(*r)) {
        lua_newtable(L);
        lua_pushstring(L, "commandType");
        lua_pushnumber(L, static_cast<int>(c.type));
        lua_rawset(L, -3);
        if (c.type == sim::CommandType::Move || c.type == sim::CommandType::Attack) {
            lua_pushstring(L, "x");
            lua_pushnumber(L, c.target_pos.x);
            lua_rawset(L, -3);
            lua_pushstring(L, "y");
            lua_pushnumber(L, c.target_pos.y);
            lua_rawset(L, -3);
            lua_pushstring(L, "z");
            lua_pushnumber(L, c.target_pos.z);
            lua_rawset(L, -3);
        }
        if (c.target_id > 0) {
            lua_pushstring(L, "targetId");
            lua_pushnumber(L, c.target_id);
            lua_rawset(L, -3);
        }
        lua_rawseti(L, -2, n++);
    }
    return 1;
}

int uu_HasUnloadCommandQueuedUp(lua_State* L) {
    const auto* r = record(L);
    const sim::WorldSnapshot* world = r ? ui_world(L) : nullptr;
    bool found = false;
    if (world)
        for (const auto& c : world->commands_of(*r))
            if (c.type == sim::CommandType::TransportUnload) found = true;
    lua_pushboolean(L, found ? 1 : 0);
    return 1;
}

int uu_GetFocus(lua_State* L) {
    const auto* r = record(L);
    push_user_unit_or_nil(L, r ? r->build_target_id : 0);
    return 1;
}

int uu_GetGuardedEntity(lua_State* L) {
    const auto* r = record(L);
    const sim::WorldSnapshot* world = r ? ui_world(L) : nullptr;
    u32 guarded = 0;
    if (world) {
        const auto commands = world->commands_of(*r);
        if (!commands.empty() && commands.front().type == sim::CommandType::Guard)
            guarded = commands.front().target_id;
    }
    push_user_unit_or_nil(L, guarded);
    return 1;
}

int uu_GetCreator(lua_State* L) {
    const auto* r = record(L);
    push_user_unit_or_nil(L, r ? r->creator_id : 0);
    return 1;
}

// --- Selection sets: the UI's own bookkeeping ------------------------------

int uu_AddSelectionSet(lua_State* L) {
    const u32 id = user_unit_id(L);
    if (id == 0 || lua_type(L, 2) != LUA_TSTRING) return 0;
    push_selection_sets(L, id, true);
    lua_pushvalue(L, 2);
    lua_pushboolean(L, 1);
    lua_rawset(L, -3);
    lua_pop(L, 1);
    return 0;
}

int uu_RemoveSelectionSet(lua_State* L) {
    const u32 id = user_unit_id(L);
    if (id == 0 || lua_type(L, 2) != LUA_TSTRING) return 0;
    push_selection_sets(L, id, false);
    if (lua_istable(L, -1)) {
        lua_pushvalue(L, 2);
        lua_pushnil(L);
        lua_rawset(L, -3);
    }
    lua_pop(L, 1);
    return 0;
}

int uu_HasSelectionSet(lua_State* L) {
    const u32 id = user_unit_id(L);
    bool has = false;
    if (id != 0 && lua_type(L, 2) == LUA_TSTRING) {
        push_selection_sets(L, id, false);
        if (lua_istable(L, -1)) {
            lua_pushvalue(L, 2);
            lua_rawget(L, -2);
            has = lua_toboolean(L, -1) != 0;
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }
    lua_pushboolean(L, has ? 1 : 0);
    return 1;
}

int uu_GetSelectionSets(lua_State* L) {
    const u32 id = user_unit_id(L);
    lua_newtable(L);
    if (id == 0) return 1;
    const int out = lua_gettop(L);
    push_selection_sets(L, id, false);
    if (lua_istable(L, -1)) {
        int n = 1;
        lua_pushnil(L);
        while (lua_next(L, -2) != 0) {
            lua_pop(L, 1); // the value
            if (lua_type(L, -1) == LUA_TSTRING) {
                lua_pushvalue(L, -1);
                lua_rawseti(L, out, n++);
            }
        }
    }
    lua_pop(L, 1);
    return 1;
}

// --- Read from the live unit (read-only): not in the snapshot --------------

int uu_GetStat(lua_State* L) {
    auto* u = check_unit(L);
    f64 value = lua_isnumber(L, 3) ? lua_tonumber(L, 3) : 0;
    if (u && lua_type(L, 2) == LUA_TSTRING) value = u->get_stat(lua_tostring(L, 2), value);
    lua_newtable(L);
    lua_pushstring(L, "Value");
    lua_pushnumber(L, value);
    lua_rawset(L, -3);
    return 1;
}

/// Whether one of the unit's weapons can fire at the target's layer.
int uu_CanAttackTarget(lua_State* L) {
    const auto* u = check_unit(L);
    const auto* target = check_entity(L, 2);
    bool can = false;
    if (u && target && !target->destroyed() && target->is_unit()) {
        const u8 bit = sim::layer_to_bit(static_cast<const sim::Unit*>(target)->layer());
        for (const auto& w : u->weapons())
            if (w->enabled && !w->fire_on_death && (w->fire_target_layer_caps & bit) != 0)
                can = true;
    }
    lua_pushboolean(L, can ? 1 : 0);
    return 1;
}

} // namespace

void set_ui_world_source(lua_State* L, const sim::WorldHistory* history) {
    lua_pushstring(L, kUiWorldSourceKey);
    lua_pushlightuserdata(L, const_cast<sim::WorldHistory*>(history));
    lua_rawset(L, LUA_REGISTRYINDEX);
}

const sim::WorldSnapshot* ui_world(lua_State* L) {
    auto* sim = get_sim(L);
    if (!sim) return nullptr;
    // The renderer's capture of this very tick, when there is one.
    lua_pushstring(L, kUiWorldSourceKey);
    lua_rawget(L, LUA_REGISTRYINDEX);
    const auto* history = static_cast<const sim::WorldHistory*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    if (history && history->ticks_captured() > 0 && history->cur().tick == sim->tick_count())
        return &history->cur();
    UiWorld& world = ui_world_holder(L);
    const u32 generation = sim::SimState::sim_generation();
    if (!world.valid || world.generation != generation || world.tick != sim->tick_count()) {
        sim::capture_world(*sim, world.snapshot);
        world.generation = generation;
        world.tick = sim->tick_count();
        world.valid = true;
    }
    return &world.snapshot;
}

// Moho's UserUnit methods (all of them; UserEntity adds none).
// clang-format off
const MethodEntry user_unit_methods[] = {
    {"AddSelectionSet",          uu_AddSelectionSet},
    {"CanAttackTarget",          uu_CanAttackTarget},
    {"GetArmy",                  uu_GetArmy},
    {"GetBlueprint",             uu_GetBlueprint},
    {"GetBuildRate",             uu_GetBuildRate},
    {"GetCommandQueue",          uu_GetCommandQueue},
    {"GetCreator",               uu_GetCreator},
    {"GetCustomName",            uu_GetCustomName},
    {"GetEconData",              uu_GetEconData},
    {"GetEntityId",              uu_GetEntityId},
    {"GetFocus",                 uu_GetFocus},
    {"GetFootPrintSize",         uu_GetFootPrintSize},
    {"GetFuelRatio",             uu_GetFuelRatio},
    {"GetGuardedEntity",         uu_GetGuardedEntity},
    {"GetHealth",                uu_GetHealth},
    {"GetMaxHealth",             uu_GetMaxHealth},
    {"GetMissileInfo",           uu_GetMissileInfo},
    {"GetPosition",              uu_GetPosition},
    {"GetSelectionSets",         uu_GetSelectionSets},
    {"GetShieldRatio",           uu_GetShieldRatio},
    {"GetStat",                  uu_GetStat},
    {"GetUnitId",                uu_GetUnitId},
    {"GetWorkProgress",          uu_GetWorkProgress},
    {"HasSelectionSet",          uu_HasSelectionSet},
    {"HasUnloadCommandQueuedUp", uu_HasUnloadCommandQueuedUp},
    {"IsAutoMode",               uu_IsAutoMode},
    {"IsAutoSurfaceMode",        uu_IsAutoSurfaceMode},
    {"IsDead",                   uu_IsDead},
    {"IsIdle",                   uu_IsIdle},
    {"IsInCategory",             uu_IsInCategory},
    {"IsOverchargePaused",       uu_IsOverchargePaused},
    {"IsRepeatQueue",            uu_IsRepeatQueue},
    {"IsStunned",                uu_IsStunned},
    {"ProcessInfo",              uu_ProcessInfo},
    {"RemoveSelectionSet",       uu_RemoveSelectionSet},
    {"SetCustomName",            uu_SetCustomName},
    {nullptr, nullptr},
};
// clang-format on

} // namespace osc::lua
