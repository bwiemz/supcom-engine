// Army brains: moho.aibrain_methods (threat, enemies and counting,
// platoons, build placement).
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
#include <array>
#include <cctype>
#include <limits>
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

static sim::ArmyBrain* check_brain(lua_State* L, int idx = 1) {
    if (!lua_istable(L, idx)) return nullptr;

    // Check sim generation — stale handles from a previous SimState return nullptr
    lua_pushstring(L, "_c_sim_gen");
    lua_rawget(L, idx);
    if (lua_isnumber(L, -1)) {
        u32 stored_gen = static_cast<u32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        if (stored_gen != sim::SimState::sim_generation()) {
            return nullptr;  // Stale handle from old SimState
        }
    } else {
        lua_pop(L, 1);
        // No generation stored — legacy table, allow through
    }

    lua_pushstring(L, "_c_object");
    lua_rawget(L, idx);
    auto* brain = static_cast<sim::ArmyBrain*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return brain;
}

// ====================================================================
// aibrain_methods — real implementations
// ====================================================================

static int brain_GetArmyIndex(lua_State* L) {
    auto* brain = check_brain(L);
    lua_pushnumber(L, brain ? brain->index() + 1 : 0); // 1-based for Lua
    return 1;
}

/// brain:NumCurrentlyBuilding(built_category, builder_category) — how many of
/// this army's units matching builder_category are building something that
/// matches built_category (retail build conditions cap parallel builds).
static int brain_NumCurrentlyBuilding(lua_State* L) {
    auto* brain = check_brain(L);
    auto* sim = get_sim(L);
    if (!brain || !sim || !lua_istable(L, 2) || !lua_istable(L, 3)) {
        lua_pushnumber(L, 0);
        return 1;
    }
    int count = 0;
    const CategoryMatcher builder_category(L, 3);
    const CategoryMatcher target_category(L, 2);
    for (auto* e : brain->get_units(sim->entity_registry())) {
        if (!e || e->destroyed() || !e->is_unit()) continue;
        auto* builder = static_cast<sim::Unit*>(e);
        if (!builder->is_building()) continue;
        auto* target = sim->entity_registry().find(builder->build_target_id());
        if (!target || target->destroyed() || !target->is_unit()) continue;
        if (!builder_category.matches(builder->category_bits())) continue;
        if (!target_category.matches(static_cast<sim::Unit*>(target)->category_bits())) continue;
        ++count;
    }
    lua_pushnumber(L, count);
    return 1;
}

/// brain:GetAvailableFactories([position, radius]) — this army's finished,
/// idle factories (nothing building, empty queue), optionally within radius.
static int brain_GetAvailableFactories(lua_State* L) {
    auto* brain = check_brain(L);
    auto* sim = get_sim(L);
    lua_newtable(L);
    const int result = lua_gettop(L);
    if (!brain || !sim) return 1;

    const bool filter = lua_istable(L, 2);
    f32 cx = 0, cz = 0, radius_sq = 0;
    if (filter) {
        lua_rawgeti(L, 2, 1);
        cx = static_cast<f32>(lua_tonumber(L, -1));
        lua_rawgeti(L, 2, 3);
        cz = static_cast<f32>(lua_tonumber(L, -1));
        lua_pop(L, 2);
        const f32 r = static_cast<f32>(luaL_optnumber(L, 3, 0));
        radius_sq = r * r;
    }
    int idx = 1;
    for (auto* e : brain->get_units(sim->entity_registry())) {
        if (!e || e->destroyed() || !e->is_unit() || e->lua_table_ref() < 0) continue;
        auto* u = static_cast<sim::Unit*>(e);
        if (!u->has_category("FACTORY") || u->is_being_built() || u->is_building() ||
            !u->command_queue().empty()) {
            continue;
        }
        if (filter) {
            const f32 dx = u->position().x - cx;
            const f32 dz = u->position().z - cz;
            if (dx * dx + dz * dz > radius_sq) continue;
        }
        lua_pushnumber(L, idx++);
        lua_rawgeti(L, LUA_REGISTRYINDEX, e->lua_table_ref());
        lua_rawset(L, result);
    }
    return 1;
}

/// brain:GetNoRushTicks() — ticks of the NoRush period still to run, 0 once
/// it is over or when the option is off (retail build conditions gate
/// transport and attack builders on it).
static int brain_GetNoRushTicks(lua_State* L) {
    auto* sim = get_sim(L);
    double remaining = 0;
    if (sim && sim->no_rush_active()) {
        remaining = (sim->no_rush_seconds() - sim->game_time()) /
                    sim::SimState::SECONDS_PER_TICK;
    }
    lua_pushnumber(L, remaining > 0 ? std::ceil(remaining) : 0);
    return 1;
}

/// brain:IsOpponentAIRunning() — Moho reports its `ai_RunOpponentAI` debug
/// toggle, which is on by default. Retail aibrain.lua gates plan evaluation
/// and execution on it; there is no toggle here, so the AI always runs.
static int brain_IsOpponentAIRunning(lua_State* L) {
    lua_pushboolean(L, 1);
    return 1;
}

static int brain_GetFactionIndex(lua_State* L) {
    auto* brain = check_brain(L);
    lua_pushnumber(L, brain ? brain->faction() : 1);
    return 1;
}

// brain:GetArmyStartPos() -> x, z  (two numbers, not a vector: scripts do
// `local x, z = brain:GetArmyStartPos()`)
static int brain_GetArmyStartPos(lua_State* L) {
    auto* brain = check_brain(L);
    const sim::Vector3 pos = brain ? brain->start_position() : sim::Vector3{};
    lua_pushnumber(L, pos.x);
    lua_pushnumber(L, pos.z);
    return 2;
}

// The economy's rates as Moho reports them: a tick's worth (its CEconomy
// keeps each tick's income, request and use). Retail's AI multiplies them by
// 10 against per-second drains (EconomyBuildConditions), and its economy bar
// by GetSimTicksPerSecond.
static f64 per_tick(f64 per_second) {
    return per_second * sim::SimState::SECONDS_PER_TICK;
}

static int brain_GetEconomyIncome(lua_State* L) {
    auto* brain = check_brain(L);
    const char* res = luaL_checkstring(L, 2);
    lua_pushnumber(L, brain ? per_tick(brain->get_economy_income(res)) : 0.0);
    return 1;
}

static int brain_GetEconomyRequested(lua_State* L) {
    auto* brain = check_brain(L);
    const char* res = luaL_checkstring(L, 2);
    lua_pushnumber(L, brain ? per_tick(brain->get_economy_requested(res)) : 0.0);
    return 1;
}

// GetEconomyUsage = actual consumption (capped by available resources).
static int brain_GetEconomyUsage(lua_State* L) {
    auto* brain = check_brain(L);
    const char* res = luaL_checkstring(L, 2);
    lua_pushnumber(L, brain ? per_tick(brain->get_economy_usage(res)) : 0.0);
    return 1;
}

// GetEconomyTrend = income less use (Moho's mIncome - mLastUseActual).
static int brain_GetEconomyTrend(lua_State* L) {
    auto* brain = check_brain(L);
    const char* res = luaL_checkstring(L, 2);
    lua_pushnumber(L, brain ? per_tick(brain->get_economy_trend(res)) : 0.0);
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

// brain:TakeResource(type, amount) -> taken: up to `amount` from the army's
// store, which never goes below 0 (Moho's CAiBrain::TakeResource; the Eye of
// Rhianne pays its scrying this way). A NaN amount is taken as is, as
// Moho's `request > stored ? stored : request` does.
static int brain_TakeResource(lua_State* L) {
    auto* brain = check_brain(L);
    if (!brain || lua_type(L, 2) != LUA_TSTRING || lua_type(L, 3) != LUA_TNUMBER) {
        lua_pushnumber(L, 0);
        return 1;
    }
    const char* res = lua_tostring(L, 2);
    const f64 request = lua_tonumber(L, 3);
    auto& econ = brain->economy();
    auto* store = std::strcmp(res, "ENERGY") == 0 || std::strcmp(res, "Energy") == 0 ? &econ.energy
                  : std::strcmp(res, "MASS") == 0 || std::strcmp(res, "Mass") == 0   ? &econ.mass
                                                                                     : nullptr;
    if (!store) {
        lua_pushnumber(L, 0);
        return 1;
    }
    const f64 taken = request > store->stored ? store->stored : request;
    const f64 left = store->stored - taken;
    store->stored = left > 0.0 ? left : 0.0;
    lua_pushnumber(L, taken);
    return 1;
}

static int brain_IsDefeated(lua_State* L) {
    auto* brain = check_brain(L);
    lua_pushboolean(L, brain ? brain->is_defeated() : 0);
    return 1;
}

static int brain_GetCurrentUnits(lua_State* L) {
    auto* brain = check_brain(L);
    auto* sim = get_sim(L);
    if (!brain || !sim) { lua_pushnumber(L, 0); return 1; }

    // Optional category filter (arg 2 — Lua category expression table)
    if (lua_istable(L, 2)) {
        const osc::lua::CategoryMatcher category(L, 2);
        i32 count = 0;
        sim->entity_registry().for_each_unit([&](const sim::Entity& e) {
            if (e.army() == brain->index() && !e.destroyed() && e.is_unit()) {
                auto* u = static_cast<const sim::Unit*>(&e);
                if (category.matches(u->category_bits())) count++;
            }
        });
        lua_pushnumber(L, count);
    } else {
        // No category filter — return total unit count
        lua_pushnumber(L, brain->get_unit_cost_total(sim->entity_registry()));
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
    const std::optional<osc::lua::CategoryMatcher> category =
        has_category ? std::optional<osc::lua::CategoryMatcher>(std::in_place, L, cat_idx)
                     : std::nullopt;

    lua_newtable(L);
    int idx = 1;
    for (auto* entity : entities) {
        if (!entity || entity->destroyed()) continue;
        if (entity->lua_table_ref() < 0) continue;
        if (!entity->is_unit()) continue;
        auto* unit = static_cast<sim::Unit*>(entity);
        if (need_built && unit->is_being_built()) continue;
        if (category && !category->matches(unit->category_bits())) continue;

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
    const std::optional<osc::lua::CategoryMatcher> category =
        has_category ? std::optional<osc::lua::CategoryMatcher>(std::in_place, L, cat_arg)
                     : std::nullopt;
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
    const char* team_filter = (lua_type(L, team_arg) == LUA_TSTRING)
                                  ? lua_tostring(L, team_arg) : "";

    // Collect units in radius
    const auto units = sim->entity_registry().units_in_radius(px, pz, radius);

    lua_newtable(L);
    int idx = 1;
    for (auto* entity : units) {
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
        if (category && !category->matches(unit->category_bits())) continue;

        lua_pushnumber(L, idx++);
        lua_rawgeti(L, LUA_REGISTRYINDEX, unit->lua_table_ref());
        lua_rawset(L, -3);
    }
    return 1;
}

static int brain_GetArmyStat(lua_State* L) {
    // GetArmyStat(self, statName, defaultValue) -> { Value = n }
    auto* brain = check_brain(L);
    if (!brain) {
        lua_newtable(L);
        lua_pushstring(L, "Value"); lua_pushnumber(L, 0); lua_rawset(L, -3);
        return 1;
    }
    const char* stat_name = luaL_checkstring(L, 2);
    f64 def_val = 0;
    if (lua_isnumber(L, 3)) {
        def_val = lua_tonumber(L, 3);
    }
    f64 val = brain->get_stat(stat_name, def_val);
    lua_newtable(L);
    lua_pushstring(L, "Value"); lua_pushnumber(L, val); lua_rawset(L, -3);
    return 1;
}

static int brain_SetArmyStat(lua_State* L) {
    auto* brain = check_brain(L);
    if (!brain) return 0;
    const char* stat_name = luaL_checkstring(L, 2);
    f64 value = luaL_checknumber(L, 3);
    brain->set_stat(stat_name, value);
    return 0;
}

// GetBlueprintStat(self, statName, category) -> number: the stat over the
// blueprints in the category (retail's score splits kills, builds and losses
// into land, air, naval, structures, commanders and experimentals). A plain
// number, unlike GetArmyStat's {Value = n}.
static int brain_GetBlueprintStat(lua_State* L) {
    auto* brain = check_brain(L);
    if (!brain) { lua_pushnumber(L, 0); return 1; }
    const char* stat_name = luaL_checkstring(L, 2);
    const auto* per_bp = brain->blueprint_stats(stat_name);
    auto* store = LuaState::get_blueprint_store(L);
    if (!lua_istable(L, 3) || !per_bp || !store) {
        lua_pushnumber(L, lua_istable(L, 3) && per_bp ? 0.0 : brain->get_stat(stat_name, 0.0));
        return 1;
    }
    f64 total = 0.0;
    const CategoryMatcher category(L, 3);
    for (const auto& [bp_id, value] : *per_bp) {
        auto* entry = store->find(bp_id);
        if (!entry) continue;
        store->push_lua_table(*entry, L);
        std::unordered_set<std::string> cats;
        sim::collect_blueprint_categories(L, lua_gettop(L), cats);
        lua_pop(L, 1);
        if (category.matches(cats)) total += value;
    }
    lua_pushnumber(L, total);
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
        lua_pushnumber(L, per_tick(econ.mass.income));
        lua_rawset(L, -3);
        lua_pushstring(L, "MassConsumed");
        lua_pushnumber(L, per_tick(econ.mass.requested));
        lua_rawset(L, -3);
        lua_pushstring(L, "EnergyIncome");
        lua_pushnumber(L, per_tick(econ.energy.income));
        lua_rawset(L, -3);
        lua_pushstring(L, "EnergyConsumed");
        lua_pushnumber(L, per_tick(econ.energy.requested));
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

// brain:ForkThread(fn, ...) — fork a sim coroutine thread
// Stack: [1]=self(brain), [2]=fn, [3..n]=extra args
// Reorder to: [1]=fn, [2]=self, [3..n]=extra args  then call fork_thread
static int brain_ForkThread(lua_State* L) {
    auto* sim = get_sim(L);
    if (!sim) { lua_pushnil(L); return 1; }
    if (lua_gettop(L) < 2 || !lua_isfunction(L, 2)) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushvalue(L, 2); // copy fn to top
    lua_remove(L, 2);    // remove fn from pos 2
    lua_insert(L, 1);    // move fn from top to pos 1
    // Stack is now: [1]=fn, [2]=self(brain), [3..n]=extra args
    return sim->thread_manager().fork_thread(L);
}

// brain:GetPersonality() — returns a personality table with methods used by AI.
// In the original engine this reads .aip personality files; we return a default
// personality with balanced emphasis values and identity delay adjustment.
static int brain_GetPersonality(lua_State* L) {
    // Check if we already cached the personality table in the registry
    lua_pushstring(L, "__osc_default_personality");
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (lua_istable(L, -1)) {
        return 1; // return cached table
    }
    lua_pop(L, 1);

    // Create a personality table with closures for the required methods
    lua_newtable(L);
    int tbl = lua_gettop(L);

    // AdjustDelay(self, base, divisor) → base (no adjustment for default)
    lua_pushstring(L, "AdjustDelay");
    lua_pushcfunction(L, [](lua_State* Ls) -> int {
        // personality:AdjustDelay(base, divisor) → return base
        f64 base_val = lua_tonumber(Ls, 2);
        lua_pushnumber(Ls, base_val);
        return 1;
    });
    lua_rawset(L, tbl);

    // GetAirUnitsEmphasis() → 0.5 (balanced)
    lua_pushstring(L, "GetAirUnitsEmphasis");
    lua_pushcfunction(L, [](lua_State* Ls) -> int {
        lua_pushnumber(Ls, 0.5);
        return 1;
    });
    lua_rawset(L, tbl);

    // GetTankUnitsEmphasis() → 0.5
    lua_pushstring(L, "GetTankUnitsEmphasis");
    lua_pushcfunction(L, [](lua_State* Ls) -> int {
        lua_pushnumber(Ls, 0.5);
        return 1;
    });
    lua_rawset(L, tbl);

    // GetBotUnitsEmphasis() → 0.5
    lua_pushstring(L, "GetBotUnitsEmphasis");
    lua_pushcfunction(L, [](lua_State* Ls) -> int {
        lua_pushnumber(Ls, 0.5);
        return 1;
    });
    lua_rawset(L, tbl);

    // GetSeaUnitsEmphasis() → 0.5
    lua_pushstring(L, "GetSeaUnitsEmphasis");
    lua_pushcfunction(L, [](lua_State* Ls) -> int {
        lua_pushnumber(Ls, 0.5);
        return 1;
    });
    lua_rawset(L, tbl);

    // GetPlatoonSize() → {1.0, 1.0} (platoon size multiplier {min, max})
    lua_pushstring(L, "GetPlatoonSize");
    lua_pushcfunction(L, [](lua_State* Ls) -> int {
        lua_newtable(Ls);
        lua_pushnumber(Ls, 1.0);
        lua_rawseti(Ls, -2, 1);
        lua_pushnumber(Ls, 1.0);
        lua_rawseti(Ls, -2, 2);
        return 1;
    });
    lua_rawset(L, tbl);

    // Make methods callable via : syntax (self-referencing __index)
    lua_pushvalue(L, tbl);
    lua_pushstring(L, "__index");
    lua_pushvalue(L, tbl);
    lua_rawset(L, -3);
    lua_setmetatable(L, tbl);

    // Cache in registry for reuse
    lua_pushstring(L, "__osc_default_personality");
    lua_pushvalue(L, tbl);
    lua_rawset(L, LUA_REGISTRYINDEX);

    return 1; // return the personality table
}

// brain:ExecutePlan(planPath) — import the plan file, call its ExecutePlan(brain)
static int brain_ExecutePlan(lua_State* L) {
    // Stack: [1]=self(brain), [2]=planPath
    const char* plan = lua_isstring(L, 2) ? lua_tostring(L, 2) : nullptr;
    if (!plan || plan[0] == '\0') return 0;

    lua_getglobal(L, "import");
    if (!lua_isfunction(L, -1)) { lua_pop(L, 1); return 0; }
    lua_pushstring(L, plan);
    if (lua_pcall(L, 1, 1, 0) != 0) {
        spdlog::warn("ExecutePlan: import('{}') failed: {}", plan,
                     lua_tostring(L, -1) ? lua_tostring(L, -1) : "?");
        lua_pop(L, 1);
        return 0;
    }
    // Stack: [1]=brain, [2]=planPath, [-1]=module
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return 0; }
    lua_pushstring(L, "ExecutePlan");
    lua_gettable(L, -2);
    if (!lua_isfunction(L, -1)) { lua_pop(L, 2); return 0; }
    lua_pushvalue(L, 1); // push brain as arg
    if (lua_pcall(L, 1, 0, 0) != 0) {
        spdlog::warn("ExecutePlan: call failed: {}",
                     lua_tostring(L, -1) ? lua_tostring(L, -1) : "?");
        lua_pop(L, 1);
    }
    lua_pop(L, 1); // pop module
    return 0;
}

// brain:EvaluatePlan(planPath) → score (number)
static int brain_EvaluatePlan(lua_State* L) {
    const char* plan = lua_isstring(L, 2) ? lua_tostring(L, 2) : nullptr;
    if (!plan || plan[0] == '\0') { lua_pushnumber(L, 0); return 1; }

    lua_getglobal(L, "import");
    if (!lua_isfunction(L, -1)) { lua_pop(L, 1); lua_pushnumber(L, 0); return 1; }
    lua_pushstring(L, plan);
    if (lua_pcall(L, 1, 1, 0) != 0) {
        lua_pop(L, 1);
        lua_pushnumber(L, 0);
        return 1;
    }
    if (!lua_istable(L, -1)) { lua_pop(L, 1); lua_pushnumber(L, 0); return 1; }
    lua_pushstring(L, "EvaluatePlan");
    lua_gettable(L, -2);
    if (!lua_isfunction(L, -1)) { lua_pop(L, 2); lua_pushnumber(L, 0); return 1; }
    lua_pushvalue(L, 1); // push brain as arg
    if (lua_pcall(L, 1, 1, 0) != 0) {
        lua_pop(L, 1);
        lua_pushnumber(L, 0);
    }
    // result is on stack (score number)
    lua_remove(L, -2); // remove module
    return 1;
}

// brain:GetStartVector3f() → {x, y, z} table
static int brain_GetStartVector3f(lua_State* L) {
    auto* brain = check_brain(L);
    osc::sim::Vector3 default_pos{0, 0, 0};
    const auto& pos = brain ? brain->start_position() : default_pos;
    lua_newtable(L);
    int tbl = lua_gettop(L);
    lua_pushnumber(L, 1);
    lua_pushnumber(L, static_cast<f64>(pos.x));
    lua_rawset(L, tbl);
    lua_pushnumber(L, 2);
    lua_pushnumber(L, static_cast<f64>(pos.y));
    lua_rawset(L, tbl);
    lua_pushnumber(L, 3);
    lua_pushnumber(L, static_cast<f64>(pos.z));
    lua_rawset(L, tbl);
    return 1;
}

// brain:GetMapWaterRatio(): the share of the map under water (M207a), which
// retail's AI weighs when it chooses naval builders.
static int brain_GetMapWaterRatio(lua_State* L) {
    auto* sim = get_sim(L);
    const auto* terrain = sim ? sim->terrain() : nullptr;
    lua_pushnumber(L, terrain ? terrain->water_ratio() : 0.0f);
    return 1;
}

// brain:SetConstantEvaluate(bool)
static int brain_SetConstantEvaluate(lua_State* L) {
    // Store on the brain's Lua table for Lua code to read
    if (lua_istable(L, 1)) {
        lua_pushstring(L, "ConstantEval");
        lua_pushboolean(L, lua_toboolean(L, 2));
        lua_rawset(L, 1);
    }
    return 0;
}

// brain:SetRepeatExecution(bool)
static int brain_SetRepeatExecution(lua_State* L) {
    if (lua_istable(L, 1)) {
        lua_pushstring(L, "RepeatExecution");
        lua_pushboolean(L, lua_toboolean(L, 2));
        lua_rawset(L, 1);
    }
    return 0;
}

// ====================================================================
// Brain threat methods
// ====================================================================

// ====================================================================
// Threat: the army's influence map (M207b, Moho's CInfluenceMap). The
// queries count what the army's intel has reported, in the map's cells;
// see sim/influence_map.hpp.
// ====================================================================

namespace {

/// A position argument's x and z (a {x, y, z} table).
sim::Vector3 threat_pos_arg(lua_State* L, int idx) {
    sim::Vector3 pos{0, 0, 0};
    if (!lua_istable(L, idx)) return pos;
    lua_rawgeti(L, idx, 1);
    pos.x = static_cast<f32>(lua_tonumber(L, -1));
    lua_pop(L, 1);
    lua_rawgeti(L, idx, 3);
    pos.z = static_cast<f32>(lua_tonumber(L, -1));
    lua_pop(L, 1);
    return pos;
}

/// The threat type named at `idx`: Overall when absent (or nil); an unknown
/// name is an error, as Moho's enum lookup makes it.
sim::ThreatType threat_type_arg(lua_State* L, int idx) {
    if (lua_gettop(L) < idx || lua_isnil(L, idx)) return sim::ThreatType::Overall;
    if (lua_type(L, idx) != LUA_TSTRING) luaL_typerror(L, idx, "string");
    sim::ThreatType type = sim::ThreatType::Overall;
    if (!sim::threat_type_from_name(lua_tostring(L, idx), type))
        luaL_error(L, "Invalid enum value '%s' for EThreatType", lua_tostring(L, idx));
    return type;
}

/// The 1-based army at `idx` as 0-based, or -1 (every army) when absent.
i32 threat_army_arg(lua_State* L, int idx, sim::SimState& sim) {
    if (lua_gettop(L) < idx || lua_isnil(L, idx)) return -1;
    const i32 army = static_cast<i32>(luaL_checknumber(L, idx));
    if (army < 1 || static_cast<size_t>(army) > sim.army_count())
        luaL_error(L, "Invalid army index %d", army);
    return army - 1;
}

} // namespace

// brain:GetThreatAtPosition(pos, rings, onMap[, type[, army]]): the threat in
// the square of cells `rings` about pos's cell, clipped to the playable area
// when onMap (Moho's fourth argument restricts to the map; it is no
// visibility check).
static int brain_GetThreatAtPosition(lua_State* L) {
    auto* brain = check_brain(L);
    auto* sim = get_sim(L);
    sim::InfluenceMap* map = brain && sim ? sim->influence_map(brain->index()) : nullptr;
    const sim::Vector3 pos = threat_pos_arg(L, 2);
    const i32 rings = static_cast<i32>(lua_tonumber(L, 3));
    const bool on_map = lua_toboolean(L, 4) != 0;
    const sim::ThreatType type = threat_type_arg(L, 5);
    if (!map) {
        lua_pushnumber(L, 0);
        return 1;
    }
    const i32 army = threat_army_arg(L, 6, *sim);
    const sim::CellRect playable = sim->playable_cells(*map);
    const i32 cell = map->cell_of(pos);
    lua_pushnumber(L, map->threat_rect(cell % map->width(), cell / map->width(), rings,
                                       on_map ? &playable : nullptr, type, army));
    return 1;
}

// brain:GetThreatsAroundPosition(pos, rings, onMap[, type[, army]]): a row
// {x, z, threat} for each cell of that square with threat, at the cell's
// centre, highest first.
static int brain_GetThreatsAroundPosition(lua_State* L) {
    auto* brain = check_brain(L);
    auto* sim = get_sim(L);
    sim::InfluenceMap* map = brain && sim ? sim->influence_map(brain->index()) : nullptr;
    const sim::Vector3 pos = threat_pos_arg(L, 2);
    const i32 rings = static_cast<i32>(lua_tonumber(L, 3));
    const bool on_map = lua_toboolean(L, 4) != 0;
    const sim::ThreatType type = threat_type_arg(L, 5);
    const i32 army = map ? threat_army_arg(L, 6, *sim) : -1; // before pushing the result
    lua_newtable(L);
    if (!map) return 1;
    const sim::CellRect playable = sim->playable_cells(*map);
    const int result = lua_gettop(L);
    int row = 1;
    for (const auto& c :
         map->threats_around(pos, rings, on_map ? &playable : nullptr, type, army)) {
        lua_newtable(L);
        lua_pushnumber(L, c.x);
        lua_rawseti(L, -2, 1);
        lua_pushnumber(L, c.z);
        lua_rawseti(L, -2, 2);
        lua_pushnumber(L, c.threat);
        lua_rawseti(L, -2, 3);
        lua_rawseti(L, result, row++);
    }
    return 1;
}

// brain:GetHighestThreatPosition(rings, onMap[, type[, army]]): the centre of
// the cell of most threat (summed over its square when rings > 0) and that
// threat; ties go to the cell nearer the army's start.
static int brain_GetHighestThreatPosition(lua_State* L) {
    auto* brain = check_brain(L);
    auto* sim = get_sim(L);
    sim::InfluenceMap* map = brain && sim ? sim->influence_map(brain->index()) : nullptr;
    const i32 rings = static_cast<i32>(lua_tonumber(L, 2));
    const bool on_map = lua_toboolean(L, 3) != 0;
    const sim::ThreatType type = threat_type_arg(L, 4);
    if (!map) {
        push_vector3(L, {0, 0, 0});
        lua_pushnumber(L, 0);
        return 2;
    }
    const i32 army = threat_army_arg(L, 5, *sim);
    const sim::CellRect playable = sim->playable_cells(*map);
    const auto best = map->highest_threat(rings, on_map ? &playable : nullptr, type, army,
                                          brain->start_position());
    push_vector3(L, {best.x, 0.0f, best.z});
    lua_pushnumber(L, best.threat);
    return 2;
}

// brain:GetThreatBetweenPositions(a, b, onMap[, type[, army]]): the threat of
// each cell on the line from a's cell to b's, summed.
static int brain_GetThreatBetweenPositions(lua_State* L) {
    auto* brain = check_brain(L);
    auto* sim = get_sim(L);
    sim::InfluenceMap* map = brain && sim ? sim->influence_map(brain->index()) : nullptr;
    const sim::Vector3 a = threat_pos_arg(L, 2);
    const sim::Vector3 b = threat_pos_arg(L, 3);
    const bool on_map = lua_toboolean(L, 4) != 0;
    const sim::ThreatType type = threat_type_arg(L, 5);
    if (!map) {
        lua_pushnumber(L, 0);
        return 1;
    }
    const i32 army = threat_army_arg(L, 6, *sim);
    const sim::CellRect playable = sim->playable_cells(*map);
    lua_pushnumber(L, map->threat_between(a, b, on_map ? &playable : nullptr, type, army));
    return 1;
}

// brain:AssignThreatAtPosition(pos, amount[, rate[, type]]): a script's
// threat in pos's cell of the army's influence map (M207b), fading by `rate`
// of itself each update (clamped to [0, 1]; 0.01 when absent).
static int brain_AssignThreatAtPosition(lua_State* L) {
    auto* brain = check_brain(L);
    auto* sim = get_sim(L);
    sim::InfluenceMap* map = brain && sim ? sim->influence_map(brain->index()) : nullptr;
    if (!map) return 0;
    const sim::Vector3 pos = threat_pos_arg(L, 2);
    const f32 amount = static_cast<f32>(luaL_checknumber(L, 3));
    // Absent, the rate is -1, which the map takes as 0.01. Given, it is
    // clamped to [0, 1] first, as Moho's binding does, so a negative one is 0:
    // threat that never fades.
    f32 rate = -1.0f;
    if (lua_gettop(L) >= 4 && !lua_isnil(L, 4))
        rate = std::clamp(static_cast<f32>(luaL_checknumber(L, 4)), 0.0f, 1.0f);
    sim::ThreatType type = sim::ThreatType::Overall;
    if (lua_gettop(L) >= 5 && !lua_isnil(L, 5)) {
        const char* name = luaL_checkstring(L, 5);
        if (!sim::threat_type_from_name(name, type))
            luaL_error(L, "Invalid enum value '%s' for EThreatType", name);
    }
    map->assign_threat(pos, type, amount, rate);
    return 0;
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
    const std::optional<osc::lua::CategoryMatcher> category =
        has_category ? std::optional<osc::lua::CategoryMatcher>(std::in_place, L, cat_arg)
                     : std::nullopt;

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
    const char* team_filter = (lua_type(L, team_arg) == LUA_TSTRING)
                                  ? lua_tostring(L, team_arg) : "";

    const auto units = sim->entity_registry().units_in_radius(px, pz, radius);

    int count = 0;
    for (auto* entity : units) {
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

        if (category && !category->matches(unit->category_bits())) continue;

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

// Placement rules for a structure blueprint, read from __blueprints.
static sim::PlacementRules placement_rules_of(lua_State* L, const std::string& bp_id) {
    sim::PlacementRules r;
    const int top = lua_gettop(L);
    lua_pushstring(L, "__blueprints");
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (lua_istable(L, -1)) {
        lua_pushstring(L, bp_id.c_str());
        lua_rawget(L, -2);
    }
    if (!lua_istable(L, -1)) {
        lua_settop(L, top);
        return r;
    }
    const int bp = lua_gettop(L);

    if (const auto [fx, fz] = sim::blueprint_footprint(L, bp); fx > 0 && fz > 0) {
        r.size_x = fx;
        r.size_z = fz;
    }

    lua_pushstring(L, "Physics");
    lua_rawget(L, bp);
    if (lua_istable(L, -1)) {
        const int phys = lua_gettop(L);
        lua_pushstring(L, "BuildOnLayerCaps"); // absent: land only
        lua_rawget(L, phys);
        if (lua_istable(L, -1)) {
            const int caps = lua_gettop(L);
            auto cap = [L, caps](const char* layer) {
                lua_pushstring(L, layer);
                lua_rawget(L, caps);
                const bool on = lua_toboolean(L, -1) != 0;
                lua_pop(L, 1);
                return on;
            };
            r.on_land = cap("LAYER_Land");
            r.on_water = cap("LAYER_Water");
            r.on_seabed = cap("LAYER_Seabed");
        }
        lua_pop(L, 1);
        const auto number = [L, phys](const char* key) {
            lua_pushstring(L, key);
            lua_rawget(L, phys);
            const f32 v = lua_isnumber(L, -1) ? static_cast<f32>(lua_tonumber(L, -1)) : 0.0f;
            lua_pop(L, 1);
            return v;
        };
        r.skirt_x = number("SkirtSizeX");
        r.skirt_z = number("SkirtSizeZ");
        r.skirt_off_x = number("SkirtOffsetX");
        r.skirt_off_z = number("SkirtOffsetZ");
        lua_pushstring(L, "BuildRestriction");
        lua_rawget(L, phys);
        if (lua_type(L, -1) == LUA_TSTRING) {
            const std::string restriction = lua_tostring(L, -1);
            if (restriction == "RULEUBR_OnMassDeposit")
                r.deposit = sim::PlacementRules::Deposit::Mass;
            else if (restriction == "RULEUBR_OnHydrocarbonDeposit")
                r.deposit = sim::PlacementRules::Deposit::Hydrocarbon;
        }
        lua_pop(L, 1);
    }
    lua_settop(L, top);
    return r;
}

const sim::PlacementRules& structure_rules(lua_State* L, const sim::SimState& sim,
                                           const std::string& bp_id) {
    return sim.placement_rules(bp_id, [&] { return placement_rules_of(L, bp_id); });
}

static sim::StructurePlacement placement_for(lua_State* L, const sim::SimState& sim, i32 army,
                                             bool scheduled = false) {
    return sim::StructurePlacement(
        sim, army, [L, &sim](const std::string& bp_id) { return structure_rules(L, sim, bp_id); },
        scheduled);
}

// Builder types whose FindPlaceToBuild answer is a deposit, not a template
// point (AIBuildStructures.IsResource).
static bool is_resource_builder_type(const std::string& type) {
    return type == "Resource" || type == "T1Resource" || type == "T2Resource" ||
           type == "T3Resource" || type == "T1HydroCarbon";
}

// brain:FindPlaceToBuild(type, structureName, buildingTypes, relative, builder,
//     optIgnoreAlliance, optOverridePosX, optOverridePosZ, optIgnoreThreatOver)
//   -> {x, z, 0} or false
// Moho's semantics (FAF engine notes, engine/Sim/CAiBrain.lua): the start
// location is the override if given (X alone is ignored), else the army
// start; the target is the override, else the builder's position.
// - Resource types: the buildable deposit nearest the start location (mass,
//   or hydrocarbon for a hydrocarbon structure).
// - Others: among the template points listed for `type` in buildingTypes
//   ({{types...}, {x, z, 0}, ...} groups) where the structure fits -- offset
//   by the start location when `relative` -- the one nearest the target.
// The answer is relative to the start location when `relative`: callers add
// their own reference point. With optIgnoreThreatOver above 0, a site whose
// cell of the army's influence map holds that much AntiSurface threat or more
// is passed over (M207b).
static int brain_FindPlaceToBuild(lua_State* L) {
    auto* brain = check_brain(L);
    auto* sim = get_sim(L);
    if (!brain || !sim || lua_type(L, 2) != LUA_TSTRING || lua_type(L, 3) != LUA_TSTRING) {
        lua_pushboolean(L, 0);
        return 1;
    }
    const std::string type = lua_tostring(L, 2);
    const std::string structure = lua_tostring(L, 3);
    const bool relative = lua_toboolean(L, 5) != 0;

    f32 start_x = brain->start_position().x;
    f32 start_z = brain->start_position().z;
    f32 target_x = start_x, target_z = start_z;
    if (lua_isnumber(L, 9)) {
        start_x = lua_isnumber(L, 8) ? static_cast<f32>(lua_tonumber(L, 8)) : 0.0f;
        start_z = static_cast<f32>(lua_tonumber(L, 9));
        target_x = start_x;
        target_z = start_z;
    } else if (lua_istable(L, 6)) {
        lua_pushstring(L, "_c_object");
        lua_rawget(L, 6);
        auto* builder = lua_isuserdata(L, -1)
                            ? static_cast<sim::Entity*>(lua_touserdata(L, -1)) : nullptr;
        lua_pop(L, 1);
        if (builder && !builder->destroyed()) {
            target_x = builder->position().x;
            target_z = builder->position().z;
        }
    }

    const auto placement = placement_for(L, *sim, brain->index());
    const i32 threat_over = lua_isnumber(L, 10) ? static_cast<i32>(lua_tonumber(L, 10)) : 0;
    const sim::InfluenceMap* threat_map =
        threat_over > 0 ? sim->influence_map(brain->index()) : nullptr;
    const sim::CellRect playable = threat_map ? sim->playable_cells(*threat_map) : sim::CellRect{};
    auto threatened = [&](f32 world_x, f32 world_z) {
        if (!threat_map) return false;
        const i32 cell = threat_map->cell_of({world_x, 0.0f, world_z});
        return threat_map->threat_rect(cell % threat_map->width(), cell / threat_map->width(), 0,
                                       &playable, sim::ThreatType::AntiSurface,
                                       -1) >= static_cast<f32>(threat_over);
    };
    bool found = false;
    f32 best_x = 0, best_z = 0, best_d2 = 0;
    auto consider = [&](f32 answer_x, f32 answer_z, f32 world_x, f32 world_z, f32 from_x,
                        f32 from_z) {
        if (threatened(world_x, world_z)) return;
        if (!placement.can_build(structure, world_x, world_z)) return;
        const f32 d2 = (world_x - from_x) * (world_x - from_x) +
                       (world_z - from_z) * (world_z - from_z);
        if (found && d2 >= best_d2) return; // first of equals wins: deterministic
        found = true;
        best_d2 = d2;
        best_x = answer_x;
        best_z = answer_z;
    };

    if (is_resource_builder_type(type)) {
        const auto want = placement.rules(structure).deposit ==
                                  sim::PlacementRules::Deposit::Hydrocarbon
                              ? sim::ResourceDeposit::Hydrocarbon
                              : sim::ResourceDeposit::Mass;
        for (const auto& d : sim->resource_deposits()) {
            if (d.type != want) continue;
            consider(relative ? d.x - start_x : d.x, relative ? d.z - start_z : d.z,
                     d.x, d.z, start_x, start_z);
        }
    } else if (lua_istable(L, 4)) {
        for (int g = 1;; ++g) {
            lua_rawgeti(L, 4, g);
            if (lua_isnil(L, -1)) { lua_pop(L, 1); break; }
            const int group = lua_gettop(L);
            bool listed = false;
            if (lua_istable(L, group)) {
                lua_rawgeti(L, group, 1);
                if (lua_istable(L, -1)) {
                    for (int t = 1; !listed; ++t) {
                        lua_rawgeti(L, -1, t);
                        if (lua_isnil(L, -1)) { lua_pop(L, 1); break; }
                        listed = lua_type(L, -1) == LUA_TSTRING && type == lua_tostring(L, -1);
                        lua_pop(L, 1);
                    }
                }
                lua_pop(L, 1);
            }
            for (int i = 2; listed; ++i) {
                lua_rawgeti(L, group, i);
                if (!lua_istable(L, -1)) { lua_pop(L, 1); break; }
                lua_rawgeti(L, -1, 1);
                lua_rawgeti(L, -2, 2);
                const bool numeric = lua_isnumber(L, -2) && lua_isnumber(L, -1);
                const f32 px = static_cast<f32>(lua_tonumber(L, -2));
                const f32 pz = static_cast<f32>(lua_tonumber(L, -1));
                lua_pop(L, 3);
                if (!numeric) continue;
                const f32 wx = relative ? px + start_x : px;
                const f32 wz = relative ? pz + start_z : pz;
                consider(px, pz, wx, wz, target_x, target_z);
            }
            lua_settop(L, group - 1);
        }
    }

    if (!found) {
        lua_pushboolean(L, 0);
        return 1;
    }
    lua_newtable(L);
    lua_pushnumber(L, best_x);
    lua_rawseti(L, -2, 1);
    lua_pushnumber(L, best_z);
    lua_rawseti(L, -2, 2);
    lua_pushnumber(L, 0);
    lua_rawseti(L, -2, 3);
    return 1;
}

// brain:BuildStructure(unit, whatToBuild, location, relative)
// unit = arg 2 (Lua table with _c_object), whatToBuild = arg 3 (bp string),
// location = arg 4 ({x, z, dist} from FindPlaceToBuild)
static int brain_BuildStructure(lua_State* L) {
    if (!lua_istable(L, 2)) {
        spdlog::debug("brain_BuildStructure: arg2 not table");
        return 0;
    }
    lua_pushstring(L, "_c_object");
    lua_rawget(L, 2);
    auto* e = lua_isuserdata(L, -1)
                  ? static_cast<sim::Entity*>(lua_touserdata(L, -1))
                  : nullptr;
    lua_pop(L, 1);
    if (!e || !e->is_unit() || e->destroyed()) {
        spdlog::debug("brain_BuildStructure: no valid unit");
        return 0;
    }
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

    if (lua_toboolean(L, 5)) { // buildRelative: an offset from the builder
        pos.x += unit->position().x;
        pos.z += unit->position().z;
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

// brain:CanBuildPlatoon(template, factories)
// template = {name, plan, {bp_id, min, max, squad, formation}, ...}
// factories = {factory1, factory2, ...}
// Returns: factories table (truthy) on success, false on failure.
static int brain_CanBuildPlatoon(lua_State* L) {
    auto* brain = check_brain(L);
    if (!brain || !lua_istable(L, 2) || !lua_istable(L, 3)) {
        lua_pushboolean(L, 0);
        return 1;
    }

    int tmpl = 2;
    int facs = 3;

    // Iterate template sub-tables starting at index 3
    for (int i = 3; ; i++) {
        lua_rawgeti(L, tmpl, i);
        if (lua_isnil(L, -1)) { lua_pop(L, 1); break; }
        if (!lua_istable(L, -1)) { lua_pop(L, 1); continue; }
        int sub = lua_gettop(L);

        // sub[1] = blueprint ID string
        lua_rawgeti(L, sub, 1);
        if (!lua_isstring(L, -1)) { lua_pop(L, 2); continue; }
        std::string bp_id = lua_tostring(L, -1);
        lua_pop(L, 1);

        // Look up the blueprint's categories
        std::unordered_set<std::string> bp_cats;
        lua_pushstring(L, "__blueprints");
        lua_rawget(L, LUA_GLOBALSINDEX);
        if (lua_istable(L, -1)) {
            lua_pushstring(L, bp_id.c_str());
            lua_rawget(L, -2);
            sim::collect_blueprint_categories(L, lua_gettop(L), bp_cats);
            lua_pop(L, 1); // bp table or nil
        }
        lua_pop(L, 1); // __blueprints

        if (bp_cats.empty()) {
            lua_pop(L, 1); // sub
            lua_pushboolean(L, 0);
            return 1;
        }

        // Check if any factory can build this blueprint
        bool found = false;
        for (int f = 1; !found; f++) {
            lua_rawgeti(L, facs, f);
            if (lua_isnil(L, -1)) { lua_pop(L, 1); break; }
            if (!lua_istable(L, -1)) { lua_pop(L, 1); continue; }

            // Get factory Unit*
            lua_pushstring(L, "_c_object");
            lua_rawget(L, -2);
            auto* e = lua_isuserdata(L, -1)
                          ? static_cast<sim::Entity*>(lua_touserdata(L, -1))
                          : nullptr;
            lua_pop(L, 1); // _c_object

            if (!e || !e->is_unit() || e->destroyed()) {
                lua_pop(L, 1); // factory table
                continue;
            }

            // Look up factory blueprint's Economy.BuildableCategory
            lua_pushstring(L, "__blueprints");
            lua_rawget(L, LUA_GLOBALSINDEX);
            if (lua_istable(L, -1)) {
                lua_pushstring(L, e->blueprint_id().c_str());
                lua_rawget(L, -2);
                if (lua_istable(L, -1)) {
                    lua_pushstring(L, "Economy");
                    lua_rawget(L, -2);
                    if (lua_istable(L, -1)) {
                        lua_pushstring(L, "BuildableCategory");
                        lua_rawget(L, -2);
                        if (lua_istable(L, -1)) {
                            // Table of category strings
                            int bc = lua_gettop(L);
                            for (int b = 1; !found; b++) {
                                lua_rawgeti(L, bc, b);
                                if (lua_isnil(L, -1)) { lua_pop(L, 1); break; }
                                if (lua_isstring(L, -1)) {
                                    std::string pattern = lua_tostring(L, -1);
                                    bool match = true;
                                    std::istringstream ss(pattern);
                                    std::string token;
                                    while (ss >> token) {
                                        if (bp_cats.count(token) == 0) {
                                            match = false;
                                            break;
                                        }
                                    }
                                    if (match) found = true;
                                }
                                lua_pop(L, 1); // entry
                            }
                        } else if (lua_isstring(L, -1)) {
                            // Single string fallback
                            std::string pattern = lua_tostring(L, -1);
                            bool match = true;
                            std::istringstream ss(pattern);
                            std::string token;
                            while (ss >> token) {
                                if (bp_cats.count(token) == 0) {
                                    match = false;
                                    break;
                                }
                            }
                            if (match) found = true;
                        }
                        lua_pop(L, 1); // BuildableCategory
                    }
                    lua_pop(L, 1); // Economy
                }
                lua_pop(L, 1); // factory bp
            }
            lua_pop(L, 1); // __blueprints
            lua_pop(L, 1); // factory table
        }

        lua_pop(L, 1); // sub

        if (!found) {
            lua_pushboolean(L, 0);
            return 1;
        }
    }

    // All template entries satisfied — return the factories table
    lua_pushvalue(L, facs);
    return 1;
}

// brain:BuildPlatoon(template, factories, count)
// template = {name, plan, {bp_id, min, max, squad, formation}, ...}
// factories = {factory1, ...}
// count = multiplier for how many of each unit to build
// Returns: nil (fire-and-forget)
static int brain_BuildPlatoon(lua_State* L) {
    auto* brain = check_brain(L);
    if (!brain || !lua_istable(L, 2) || !lua_istable(L, 3)) return 0;

    int tmpl = 2;
    int facs = 3;
    int count = lua_isnumber(L, 4) ? static_cast<int>(lua_tonumber(L, 4)) : 1;
    if (count < 1) count = 1;

    // Collect factory Unit* pointers
    std::vector<sim::Unit*> factories;
    for (int f = 1; ; f++) {
        lua_rawgeti(L, facs, f);
        if (lua_isnil(L, -1)) { lua_pop(L, 1); break; }
        if (!lua_istable(L, -1)) { lua_pop(L, 1); continue; }
        lua_pushstring(L, "_c_object");
        lua_rawget(L, -2);
        auto* e = lua_isuserdata(L, -1)
                      ? static_cast<sim::Entity*>(lua_touserdata(L, -1))
                      : nullptr;
        lua_pop(L, 1); // _c_object
        lua_pop(L, 1); // factory table
        if (e && e->is_unit() && !e->destroyed())
            factories.push_back(static_cast<sim::Unit*>(e));
    }

    if (factories.empty()) return 0;

    // Iterate template sub-tables starting at index 3
    for (int i = 3; ; i++) {
        lua_rawgeti(L, tmpl, i);
        if (lua_isnil(L, -1)) { lua_pop(L, 1); break; }
        if (!lua_istable(L, -1)) { lua_pop(L, 1); continue; }
        int sub = lua_gettop(L);

        // sub[1] = blueprint ID
        lua_rawgeti(L, sub, 1);
        if (!lua_isstring(L, -1)) { lua_pop(L, 2); continue; }
        std::string bp_id = lua_tostring(L, -1);
        lua_pop(L, 1);

        // sub[2] = min count
        lua_rawgeti(L, sub, 2);
        int num = lua_isnumber(L, -1)
                      ? static_cast<int>(lua_tonumber(L, -1)) * count
                      : count;
        lua_pop(L, 1);

        lua_pop(L, 1); // sub

        // Distribute build commands across factories (round-robin)
        auto* factory = factories[(i - 3) % factories.size()];
        for (int n = 0; n < num; n++) {
            sim::UnitCommand cmd;
            cmd.type = sim::CommandType::BuildFactory;
            cmd.blueprint_id = bp_id;
            factory->push_command(cmd, false);
        }
    }

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

// brain:FindUpgradeBP(unitBpId, upgradeTemplate)
// upgradeTemplate = array of {fromBP, toBP} pairs.  Returns toBP when
// fromBP matches unitBpId, or nil if no match.
static int brain_FindUpgradeBP(lua_State* L) {
    const char* unit_bp = luaL_checkstring(L, 2);
    if (!lua_istable(L, 3)) { lua_pushnil(L); return 1; }

    int len = luaL_getn(L, 3);
    for (int i = 1; i <= len; i++) {
        lua_rawgeti(L, 3, i); // pair table
        if (lua_istable(L, -1)) {
            lua_rawgeti(L, -1, 1); // fromBP
            const char* from = lua_tostring(L, -1);
            if (from && std::strcmp(from, unit_bp) == 0) {
                lua_pop(L, 1); // pop fromBP
                lua_rawgeti(L, -1, 2); // toBP
                lua_remove(L, -2); // remove pair table
                return 1;
            }
            lua_pop(L, 1); // pop fromBP
        }
        lua_pop(L, 1); // pop pair table
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
    const std::string formation = lua_type(L, 5) == LUA_TSTRING ? lua_tostring(L, 5) : "";

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
            // A unit is in exactly one platoon (the pool when in no other):
            // assigning moves it.
            if (auto* owner = check_brain(L); owner) {
                for (size_t pi = 0; pi < owner->platoon_count(); ++pi) {
                    auto* other = owner->platoon_at(pi);
                    if (other && other != platoon && !other->destroyed() &&
                        other->has_unit(e->entity_id()))
                        other->remove_unit(e->entity_id());
                }
            }
            platoon->add_unit(e->entity_id());
            platoon->set_unit_squad(e->entity_id(), squad);
            platoon->set_unit_formation(e->entity_id(), formation);

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

// brain:DisbandPlatoon(platoon): destroy the platoon but not its units,
// which go back to the army pool. The pool itself ("ArmyPool") is the
// engine's and holds every unit not in another platoon: disbanding it is a
// no-op. (Retail PlatoonDisband reaches it when an idle engineer's
// PlatoonHandle is the pool; FAF later added a script guard for the same.)
static int brain_DisbandPlatoon(lua_State* L) {
    auto* platoon = check_platoon(L, 2);
    if (!platoon || platoon->name() == "ArmyPool") return 0;

    auto* sim = get_sim(L);

    // Units return to the pool.
    if (auto* owner = check_brain(L); owner && sim) {
        if (auto* pool = owner->find_platoon_by_name("ArmyPool"); pool && pool != platoon) {
            for (u32 id : platoon->unit_ids()) {
                auto* e = sim->entity_registry().find(id);
                if (e && !e->destroyed() && !pool->has_unit(id)) pool->add_unit(id);
            }
        }
    }

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

// brain:CanBuildStructureAt(bp_id, position) -> bool
// position is a vector {x, y, z} (scripts pass {loc[1], 0, loc[2]}).
static int brain_CanBuildStructureAt(lua_State* L) {
    auto* brain = check_brain(L);
    auto* sim = get_sim(L);
    if (!brain || !sim || lua_type(L, 2) != LUA_TSTRING || !lua_istable(L, 3)) {
        lua_pushboolean(L, 0);
        return 1;
    }
    lua_rawgeti(L, 3, 1);
    lua_rawgeti(L, 3, 3);
    const f32 x = static_cast<f32>(lua_tonumber(L, -2));
    const f32 z = static_cast<f32>(lua_tonumber(L, -1));
    lua_pop(L, 2);
    const auto placement = placement_for(L, *sim, brain->index());
    lua_pushboolean(L, placement.can_build(lua_tostring(L, 2), x, z) ? 1 : 0);
    return 1;
}

/// A blueprint id as Moho names blueprints: lowercase (retail's
/// aibrain.lua writes 'UEB1103'; the blueprint tables are keyed lowercase).
static std::string blueprint_name(const char* id) {
    std::string out = id;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

// Make a unit for the brain's army as a script's CreateUnitHPR does (its
// full creation path), facing north; its table on the stack, or nil.
static void create_brain_unit(lua_State* L, const sim::ArmyBrain& brain, const std::string& bp,
                              f32 x, f32 y, f32 z) {
    lua_pushstring(L, "CreateUnitHPR");
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        lua_pushnil(L);
        return;
    }
    lua_pushstring(L, bp.c_str());
    lua_pushnumber(L, brain.index() + 1);
    lua_pushnumber(L, x);
    lua_pushnumber(L, y);
    lua_pushnumber(L, z);
    lua_pushnumber(L, 0);
    lua_pushnumber(L, 0);
    lua_pushnumber(L, 0);
    if (lua_pcall(L, 8, 1, 0) != 0) {
        spdlog::warn("creating {} for army {}: {}", bp, brain.index(), lua_tostring(L, -1));
        lua_pop(L, 1);
        lua_pushnil(L);
    }
}

bool can_build_structure(lua_State* L, const sim::SimState& sim, int army, const std::string& bp_id,
                         f32 x, f32 z) {
    return placement_for(L, sim, army, true).can_build(bp_id, x, z);
}

// Where a structure stands (Moho: a footprint that can sit on the seabed
// occupies OC_SEABED, which BuildOnLayerCaps' LAYER_Seabed gives).
f32 structure_elevation(const sim::SimState& sim, const sim::PlacementRules& rules, f32 x, f32 z) {
    const auto* terrain = sim.terrain();
    if (!terrain) return 0.0f;
    return rules.on_seabed ? terrain->get_terrain_height(x, z) : terrain->get_surface_height(x, z);
}

// brain:CreateResourceBuildingNearest(bp, x, z) -> unit or nil: the
// resource building on the free deposit of its kind nearest (x, z) -- a
// hydrocarbon plant's on hydrocarbon, anything else's on mass (Moho's
// CAiBrain::CreateResourceBuildingNearest; the lobby's prebuilt units).
static int brain_CreateResourceBuildingNearest(lua_State* L) {
    auto* brain = check_brain(L);
    auto* sim = get_sim(L);
    if (!brain || !sim || lua_type(L, 2) != LUA_TSTRING || lua_type(L, 3) != LUA_TNUMBER ||
        lua_type(L, 4) != LUA_TNUMBER) {
        lua_pushnil(L);
        return 1;
    }
    const std::string bp = blueprint_name(lua_tostring(L, 2));
    const f32 x = static_cast<f32>(lua_tonumber(L, 3));
    const f32 z = static_cast<f32>(lua_tonumber(L, 4));
    const auto placement = placement_for(L, *sim, brain->index());
    const auto wanted = placement.rules(bp).deposit == sim::PlacementRules::Deposit::Hydrocarbon
                            ? sim::ResourceDeposit::Hydrocarbon
                            : sim::ResourceDeposit::Mass;
    struct Candidate {
        f32 x, z, distance_sq;
    };
    std::vector<Candidate> candidates;
    for (const auto& d : sim->resource_deposits()) {
        if (d.type != wanted) continue;
        const f32 dx = x - d.x, dz = z - d.z;
        candidates.push_back({d.x, d.z, dx * dx + dz * dz});
    }
    // Nearest first; ties in deposit order (stable, as lockstep needs).
    std::stable_sort(
        candidates.begin(), candidates.end(),
        [](const Candidate& a, const Candidate& b) { return a.distance_sq < b.distance_sq; });
    for (const auto& c : candidates) {
        if (!placement.can_build(bp, c.x, c.z)) continue;
        create_brain_unit(L, *brain, bp, c.x,
                          structure_elevation(*sim, placement.rules(bp), c.x, c.z), c.z);
        if (!lua_isnil(L, -1)) return 1;
        lua_pop(L, 1);
    }
    lua_pushnil(L);
    return 1;
}

// brain:CreateUnitNearSpot(bp, x, z) -> unit or nil: the unit at the first
// free site found about (x, z), keeping clear of the army's start spot
// (Moho's CAiBrain::CreateUnitNearSpot occupies the start +-5 while it
// looks). The search is Moho's TryBuildStructureAt: the spot itself, then
// square rings one cell further out each time, up to 900 tries.
static int brain_CreateUnitNearSpot(lua_State* L) {
    auto* brain = check_brain(L);
    auto* sim = get_sim(L);
    if (!brain || !sim || lua_type(L, 2) != LUA_TSTRING || lua_type(L, 3) != LUA_TNUMBER ||
        lua_type(L, 4) != LUA_TNUMBER) {
        lua_pushnil(L);
        return 1;
    }
    const std::string bp = blueprint_name(lua_tostring(L, 2));
    const f32 x = static_cast<f32>(lua_tonumber(L, 3));
    const f32 z = static_cast<f32>(lua_tonumber(L, 4));
    const auto placement = placement_for(L, *sim, brain->index());
    const auto& rules = placement.rules(bp);
    const sim::Vector3& start = brain->start_position();
    const sim::StructureSite start_box{static_cast<f32>(static_cast<i32>(start.x)),
                                       static_cast<f32>(static_cast<i32>(start.z)), 10.0f, 10.0f};
    const auto free_at = [&](f32 cx, f32 cz, f32& out_x, f32& out_z) {
        sim::snap_structure_center(cx, cz, rules.size_x, rules.size_z);
        if (start_box.overlaps({cx, cz, rules.size_x, rules.size_z})) return false;
        if (!placement.can_build(bp, cx, cz)) return false;
        out_x = cx;
        out_z = cz;
        return true;
    };
    f32 at_x = 0, at_z = 0;
    bool found = free_at(x, z, at_x, at_z);
    constexpr i32 kBorder = 1; // the cell step (Moho passes `true` for its border)
    for (i32 ring = 1, attempts = 0; !found && attempts < 900; ++ring) {
        const i32 lower = -ring, upper = ring;
        for (i32 i = lower; i <= upper && !found; ++i) {
            const i32 dj = (i == lower || i == upper) ? 1 : 2 * upper;
            for (i32 j = lower; j <= upper && !found; j += dj) {
                ++attempts;
                found = free_at(x + static_cast<f32>(kBorder * i),
                                z + static_cast<f32>(kBorder * j), at_x, at_z);
            }
        }
    }
    if (!found) {
        lua_pushnil(L);
        return 1;
    }
    create_brain_unit(L, *brain, bp, at_x, structure_elevation(*sim, rules, at_x, at_z), at_z);
    return 1;
}

// brain:IsAnyEngineerBuilding(category) -> bool
// Check if any engineer/commander in this army is building a unit matching category
static int brain_IsAnyEngineerBuilding(lua_State* L) {
    auto* brain = check_brain(L);
    auto* sim = get_sim(L);
    if (!brain || !sim) { lua_pushboolean(L, 0); return 1; }

    int cat_idx = lua_istable(L, 2) ? 2 : 0;
    const std::optional<osc::lua::CategoryMatcher> category =
        cat_idx > 0 ? std::optional<osc::lua::CategoryMatcher>(std::in_place, L, cat_idx)
                    : std::nullopt;
    i32 army = brain->index();

    bool found = false;
    sim->entity_registry().for_each_unit([&](sim::Entity& e) {
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
            if (!category->matches(target_unit->category_bits())) return;
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
    if (type == "MASS" || type == "Mass")
        brain->give_storage(amount, 0.0);
    else if (type == "ENERGY" || type == "Energy")
        brain->give_storage(0.0, amount);
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

// brain:SetArmyStatsTrigger(stat, name, compare, value[, category])
static int brain_SetArmyStatsTrigger(lua_State* L) {
    auto* brain = check_brain(L);
    if (!brain) {
        return 0;
    }
    sim::StatTrigger t;
    t.stat = luaL_checkstring(L, 2);
    t.name = luaL_checkstring(L, 3);
    t.compare = luaL_checkstring(L, 4);
    t.value = luaL_checknumber(L, 5);
    if (!lua_isnoneornil(L, 6)) {
        t.category = "category";
    }
    brain->add_stat_trigger(std::move(t));
    return 0;
}

// brain:RemoveArmyStatsTrigger(stat, name)
static int brain_RemoveArmyStatsTrigger(lua_State* L) {
    auto* brain = check_brain(L);
    if (brain) {
        brain->remove_stat_trigger(luaL_checkstring(L, 2), luaL_checkstring(L, 3));
    }
    return 0;
}

// Not called in FA — named no-ops to replace generic stubs
static int brain_RemoveEnergyDependingEntity(lua_State*) { return 0; }
static int brain_PBMAddBuildLocation(lua_State*) { return 0; }
static int brain_PBMRemoveBuildLocation(lua_State*) { return 0; }
// brain:SetUpAttackVectorsToArmy([category]): the attack vectors on the
// current enemy, as Moho's CAiBrain::ProcessAttackVectors (faf-re): the map
// in 32-unit cells, those holding one of the enemy's units in the category
// (MOBILE - STRUCTURE without one) marked; from the middle of each unmarked
// cell, at height 0, an arrow to the middle of each marked one of the 3x3
// about it -- the frontier of the enemy's presence -- cell by cell, row by
// row. None without an enemy.
static int brain_SetUpAttackVectorsToArmy(lua_State* L) {
    const int n = lua_gettop(L);
    if (n < 1 || n > 2)
        return luaL_error(L, "%s\n  expected between %d and %d args, but got %d",
                          "CAiBrain:SetUpAttackVectorsToArmy()", 1, 2, n);
    auto* brain = check_brain(L);
    auto* sim = get_sim(L);
    if (!brain || !sim) return 0;
    std::vector<sim::ArmyBrain::AttackVector> vectors;
    const i32 enemy = brain->current_enemy_index();
    const auto* terrain = sim->terrain();
    if (enemy < 0 || !terrain) {
        brain->set_attack_vectors({});
        return 0;
    }
    const bool given = n > 1 && !lua_isnil(L, 2);
    const osc::lua::CategoryMatcher wanted(L, 2);
    constexpr i32 kCell = 32;
    const i32 cols = static_cast<i32>(terrain->map_width()) / kCell;
    const i32 rows = static_cast<i32>(terrain->map_height()) / kCell;
    if (cols <= 0 || rows <= 0) {
        brain->set_attack_vectors({});
        return 0;
    }
    std::vector<sim::Vector3> at;
    sim->entity_registry().for_each_unit([&](sim::Entity& e) {
        if (e.destroyed() || e.army() != enemy) return;
        const auto& u = static_cast<const sim::Unit&>(e);
        if (given ? wanted.matches(u.category_bits())
                  : u.has_category("MOBILE") && !u.has_category("STRUCTURE"))
            at.push_back(u.position());
    });
    // A cell's bounds take its edges: a unit on one marks both cells.
    const auto cell = [cols](i32 row, i32 col) {
        return static_cast<size_t>(row) * static_cast<size_t>(cols) + static_cast<size_t>(col);
    };
    std::vector<u8> marked(cell(rows, 0), 0);
    const f32 half = kCell * 0.5f;
    for (i32 row = 0; row < rows; ++row)
        for (i32 col = 0; col < cols; ++col) {
            const f32 cx = half + static_cast<f32>(kCell * col);
            const f32 cz = half + static_cast<f32>(kCell * row);
            marked[cell(row, col)] = std::any_of(at.begin(), at.end(), [&](const sim::Vector3& p) {
                return cx - half <= p.x && p.x <= cx + half && cz - half <= p.z && p.z <= cz + half;
            });
        }
    const auto is_marked = [&](i32 row, i32 col) { return marked[cell(row, col)] != 0; };
    for (i32 row = 0; row < rows; ++row)
        for (i32 col = 0; col < cols; ++col) {
            if (is_marked(row, col)) continue;
            const sim::Vector3 from{half + static_cast<f32>(kCell * col), 0.0f,
                                    half + static_cast<f32>(kCell * row)};
            for (i32 r = std::max(row - 1, 0); r <= std::min(row + 1, rows - 1); ++r)
                for (i32 c = std::max(col - 1, 0); c <= std::min(col + 1, cols - 1); ++c) {
                    if (!is_marked(r, c)) continue;
                    vectors.push_back({from,
                                       {half + static_cast<f32>(kCell * c) - from.x, 0.0f,
                                        half + static_cast<f32>(kCell * r) - from.z}});
                }
        }
    brain->set_attack_vectors(std::move(vectors));
    return 0;
}

/// An attack vector as Moho hands one to Lua (SCR_ToLua<SPointVector>).
static void push_attack_vector(lua_State* L, const sim::ArmyBrain::AttackVector& v) {
    lua_newtable(L);
    const std::pair<const char*, f32> fields[] = {{"px", v.position.x},  {"py", v.position.y},
                                                  {"pz", v.position.z},  {"vx", v.direction.x},
                                                  {"vy", v.direction.y}, {"vz", v.direction.z}};
    for (const auto& [key, value] : fields) {
        lua_pushstring(L, key);
        lua_pushnumber(L, value);
        lua_rawset(L, -3);
    }
}

// brain:GetAttackVectors() -> {{px, py, pz, vx, vy, vz}, ...}, as
// SetUpAttackVectorsToArmy left them; nil when there are none, as Moho's.
static int brain_GetAttackVectors(lua_State* L) {
    auto* brain = check_brain(L);
    if (!brain || brain->attack_vectors().empty()) {
        lua_pushnil(L);
        return 1;
    }
    lua_newtable(L);
    int i = 1;
    for (const auto& v : brain->attack_vectors()) {
        push_attack_vector(L, v);
        lua_rawseti(L, -2, i++);
    }
    return 1;
}

namespace {

/// A name of Moho's that Lua gives in any case, with or without its prefix
/// (gpg's REnumType::SetLexical); its index in `names`, or -1.
template <size_t N>
int parse_enum(std::string_view text, std::string_view prefix,
               const std::array<std::string_view, N>& names) {
    const auto same = [](std::string_view a, std::string_view b) {
        return a.size() == b.size() &&
               std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
                   return std::tolower(static_cast<unsigned char>(x)) ==
                          std::tolower(static_cast<unsigned char>(y));
               });
    };
    if (text.size() > prefix.size() && same(text.substr(0, prefix.size()), prefix))
        text.remove_prefix(prefix.size());
    for (size_t i = 0; i < N; ++i)
        if (same(text, names[i])) return static_cast<int>(i);
    return -1;
}

enum class Alliance { Neutral, Ally, Enemy };
enum class Compare { Closest, Furthest, HighestValue, LeastDefended };

f32 dist_sq(const sim::Vector3& a, const sim::Vector3& b) {
    const f32 dx = a.x - b.x;
    const f32 dy = a.y - b.y;
    const f32 dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

/// Moho's func_GetUnitsAroundPoint: the live units whose footprints reach
/// the square `reach` about `at`, of `alliance` to the brain's army, known
/// to it (its own, or one it holds a blip of), in `category`.
std::vector<const sim::Unit*> units_around(const sim::SimState& sim, i32 army,
                                           const osc::lua::CategoryMatcher& category,
                                           const sim::Vector3& at, f32 reach, Alliance alliance) {
    std::vector<const sim::Unit*> out;
    constexpr f32 kSlack = sim::EntityRegistry::COLLIDER_REACH;
    for (const sim::Entity* e :
         sim.entity_registry().units_in_radius(at.x, at.z, reach * 1.4143f + kSlack)) {
        const auto& u = static_cast<const sim::Unit&>(*e);
        if (u.destroyed() || u.is_dying()) continue;
        const f32 hx = u.footprint_size_x() * 0.5f;
        const f32 hz = u.footprint_size_z() * 0.5f;
        const auto& p = u.position();
        if (p.x + hx < at.x - reach || p.x - hx > at.x + reach || p.z + hz < at.z - reach ||
            p.z - hz > at.z + reach)
            continue;
        const i32 other = u.army();
        const Alliance is = other == army || sim.is_ally(army, other) ? Alliance::Ally
                            : sim.is_enemy(army, other)               ? Alliance::Enemy
                                                                      : Alliance::Neutral;
        if (is != alliance) continue;
        if (other != army &&
            (army < 0 || !sim.get_blip_snapshot(u.entity_id(), static_cast<u32>(army))))
            continue;
        if (category.matches(u.category_bits())) out.push_back(&u);
    }
    return out;
}

} // namespace

// brain:PickBestAttackVector(platoon, squad, alliance, compareType, category
// [, scoreScript, scoreFunc]) -> {px, py, pz, vx, vy, vz} or nil: of the
// attack vectors (SetUpAttackVectorsToArmy's), the one best for the squad,
// as Moho's CAiBrain::PickBestAttackVector (faf-re). A vector is its point
// stepped once along it, and counts where every unit of the squad could
// stand (and the scorer, `import(scoreScript)[scoreFunc](squadX, squadZ,
// x, z)`, if given, agrees). Closest and Furthest rank them from the squad
// with no category; given one they take the first (Moho looks for a unit
// of the army outside the category it gathered by, and so finds none).
// HighestValue and LeastDefended score what stands about the squad --
// every vector alike -- and fall back on Closest when that is nothing.
// Nil with no current enemy, no such squad, or no vector.
static int brain_PickBestAttackVector(lua_State* L) {
    const int n = lua_gettop(L);
    if (n < 6 || n > 8)
        return luaL_error(L, "%s\n  expected between %d and %d args, but got %d",
                          "CAiBrain:PickBestAttackVector(platoon, squad, alliance, "
                          "compareType, category[, scoreScript, scoreFunc])",
                          6, 8, n);
    auto* brain = check_brain(L);
    auto* platoon = check_platoon(L, 2);
    auto* sim = get_sim(L);
    const auto name_at = [L](int i) -> std::string_view {
        if (lua_type(L, i) != LUA_TSTRING && lua_type(L, i) != LUA_TNUMBER) {
            luaL_typerror(L, i, "string");
            return {};
        }
        return lua_tostring(L, i);
    };
    const std::string squad{name_at(3)};
    const int squad_class = sim::Platoon::squad_class(squad);
    const int alliance = parse_enum(name_at(4), "ALLIANCE_",
                                    std::array<std::string_view, 3>{"Neutral", "Ally", "Enemy"});
    const int compare = parse_enum(
        name_at(5), "COMPARE_",
        std::array<std::string_view, 4>{"Closest", "Furthest", "HighestValue", "LeastDefended"});
    if (squad_class < 0 || alliance < 0 || compare < 0)
        return luaL_error(L, "Invalid enum value %s",
                          squad_class < 0 ? squad.c_str()
                          : alliance < 0  ? lua_tostring(L, 4)
                                          : lua_tostring(L, 5));
    if (n == 7)
        spdlog::warn("CAiBrain::PickBestAttackVector: Expected 6 or 8 arguments, got 7 instead.");
    const bool has_category = lua_istable(L, 6);
    const osc::lua::CategoryMatcher category(L, 6);
    int scorer = 0;
    if (n == 8) {
        const std::string script{name_at(7)};
        const std::string func{name_at(8)};
        lua_pushstring(L, "import");
        lua_rawget(L, LUA_GLOBALSINDEX);
        lua_pushstring(L, script.c_str());
        if (lua_pcall(L, 1, 1, 0) != 0)
            return luaL_error(L,
                              "Error loading user-supplied callback in "
                              "CAiBrain::PickBestAttackVector: %s",
                              lua_tostring(L, -1));
        lua_pushstring(L, func.c_str());
        lua_gettable(L, -2);
        lua_remove(L, -2);
        scorer = lua_gettop(L);
    }
    if (!brain || !platoon || !sim || brain->current_enemy_index() < 0) {
        lua_pushnil(L);
        return 1;
    }

    // The squad: its live units, and where they stand on average.
    std::vector<const sim::Unit*> members;
    sim::Vector3 centre{0, 0, 0};
    for (u32 id : platoon->unit_ids()) {
        const auto* e = sim->entity_registry().find(id);
        if (!e || e->destroyed() || !e->is_unit()) continue;
        if (sim::Platoon::squad_class(platoon->get_unit_squad(id)) != squad_class) continue;
        members.push_back(static_cast<const sim::Unit*>(e));
        centre.x += e->position().x;
        centre.y += e->position().y;
        centre.z += e->position().z;
    }
    if (members.empty()) {
        lua_pushnil(L);
        return 1;
    }
    const f32 count = static_cast<f32>(members.size());
    centre = {centre.x / count, centre.y / count, centre.z / count};

    const auto fits = [&](const sim::Vector3& at) {
        const auto* grid = sim->pathfinding_grid();
        if (!grid) return true;
        u32 gx = 0;
        u32 gz = 0;
        grid->world_to_grid(at.x, at.z, gx, gz);
        return std::all_of(members.begin(), members.end(), [&](const sim::Unit* u) {
            return u->is_dying() || grid->is_passable_for(gx, gz, u->layer(), u->naval_draft(),
                                                          u->is_amphibious() || u->is_hover());
        });
    };
    const auto approves = [&](const sim::Vector3& at) {
        if (scorer == 0) return true;
        lua_pushvalue(L, scorer);
        lua_pushnumber(L, centre.x);
        lua_pushnumber(L, centre.z);
        lua_pushnumber(L, at.x);
        lua_pushnumber(L, at.z);
        if (lua_pcall(L, 4, 1, 0) != 0)
            luaL_error(L,
                       "Error running user-supplied callback in "
                       "CAiBrain::PickBestAttackVector: %s",
                       lua_tostring(L, -1));
        const bool yes = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        return yes;
    };
    const auto value_about = [&](f32 reach) {
        const auto units = units_around(*sim, brain->index(), category, centre, reach,
                                        static_cast<Alliance>(alliance));
        f32 sum = 0;
        for (const sim::Unit* u : units)
            sum += static_cast<f32>(u->build_cost_mass() + u->build_cost_energy());
        return std::pair<f32, f32>{sum, static_cast<f32>(units.size())};
    };

    const auto pick = [&](Compare how) -> std::pair<const sim::ArmyBrain::AttackVector*, f32> {
        const sim::ArmyBrain::AttackVector* best = nullptr;
        f32 best_score = -1.0f;
        for (const auto& v : brain->attack_vectors()) {
            const sim::Vector3 at{v.position.x + v.direction.x, v.position.y + v.direction.y,
                                  v.position.z + v.direction.z};
            if (!fits(at) || !approves(at)) continue;
            bool keep = false;
            f32 score = 0;
            switch (how) {
            case Compare::Closest:
            case Compare::Furthest:
                score = has_category ? std::numeric_limits<f32>::infinity() : dist_sq(centre, at);
                keep = best_score < 0.0f ||
                       (how == Compare::Closest ? score < best_score : score > best_score);
                break;
            case Compare::HighestValue:
                score = value_about(0.0f).first;
                keep = score > best_score || best_score < 0.0f ||
                       (best && score == best_score &&
                        dist_sq(best->position, centre) > dist_sq(at, centre));
                break;
            case Compare::LeastDefended:
                score = value_about(32.0f).second;
                keep = score > best_score || best_score < 0.0f;
                break;
            }
            if (keep) {
                best = &v;
                best_score = score;
            }
        }
        return {best, best_score};
    };
    auto [best, score] = pick(static_cast<Compare>(compare));
    if ((compare == static_cast<int>(Compare::HighestValue) ||
         compare == static_cast<int>(Compare::LeastDefended)) &&
        score == 0.0f)
        best = pick(Compare::Closest).first;
    // Moho hands back nothing for a vector at the origin.
    if (!best || (best->position.x == 0 && best->position.y == 0 && best->position.z == 0)) {
        lua_pushnil(L);
        return 1;
    }
    push_attack_vector(L, *best);
    return 1;
}
static int brain_SetGreaterOf(lua_State*) { return 0; }

// brain:CheckBlockingTerrain(startPos, endPos, arcType): whether the terrain
// stands between two points for a shot along a straight line ('none') or a
// 'low' or high arc -- Moho's CAiBrain::CheckBlockingTerrain (faf-re). A
// position that isn't a table reads as the origin, as Moho's does: retail's
// CheckNavalPathing passes an end it never set.
static int brain_CheckBlockingTerrain(lua_State* L) {
    if (lua_gettop(L) != 4) {
        luaL_error(L, "%s\n  expected %d args, but got %d",
                   "CAiBrain:CheckBlockingTerrain( startPos, endPos, arcType )", 4, lua_gettop(L));
    }
    const auto point = [L](int idx) {
        std::array<f32, 3> p{0, 0, 0};
        if (!lua_istable(L, idx)) return p;
        for (int i = 0; i < 3; ++i) {
            lua_rawgeti(L, idx, i + 1);
            p[static_cast<size_t>(i)] = static_cast<f32>(lua_tonumber(L, -1));
            lua_pop(L, 1);
        }
        return p;
    };
    const auto a = point(2);
    const auto b = point(3);
    if (!lua_isstring(L, 4)) luaL_typerror(L, 4, "string");
    const std::string_view arc_name = lua_tostring(L, 4);
    const auto named = [&](std::string_view name) {
        return std::equal(arc_name.begin(), arc_name.end(), name.begin(), name.end(),
                          [](char x, char y) {
                              return std::tolower(static_cast<unsigned char>(x)) ==
                                     std::tolower(static_cast<unsigned char>(y));
                          });
    };
    const map::ShotArc arc = named("none")  ? map::ShotArc::Straight
                             : named("low") ? map::ShotArc::Low
                                            : map::ShotArc::High;
    auto* sim = get_sim(L);
    const auto* terrain = sim ? sim->terrain() : nullptr;
    lua_pushboolean(L, terrain && map::terrain_blocks_shot(terrain->heightmap(), a[0], a[1], a[2],
                                                           b[0], b[1], b[2], arc));
    return 1;
}

// clang-format off
const MethodEntry aibrain_methods[] = {
    // Real implementations
    {"GetArmyIndex",                brain_GetArmyIndex},
    {"GetFactionIndex",             brain_GetFactionIndex},
    {"IsOpponentAIRunning",         brain_IsOpponentAIRunning},
    {"GetNoRushTicks",              brain_GetNoRushTicks},
    {"NumCurrentlyBuilding",        brain_NumCurrentlyBuilding},
    {"GetAvailableFactories",       brain_GetAvailableFactories},
    {"GetListOfUnits",              brain_GetListOfUnits},
    {"GetUnitsAroundPoint",         brain_GetUnitsAroundPoint},
    {"GetArmyStartPos",             brain_GetArmyStartPos},
    {"GetEconomyIncome",            brain_GetEconomyIncome},
    {"GetEconomyRequested",         brain_GetEconomyRequested},
    {"GetEconomyTrend",             brain_GetEconomyTrend},
    {"GetEconomyStored",            brain_GetEconomyStored},
    {"GetEconomyStoredRatio",       brain_GetEconomyStoredRatio},
    {"GiveResource",                brain_GiveResource},
    {"TakeResource",                brain_TakeResource},
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
    {"ForkThread",                  brain_ForkThread},
    {"GetPersonality",              brain_GetPersonality},
    {"AssignThreatAtPosition",      brain_AssignThreatAtPosition},
    {"ExecutePlan",                 brain_ExecutePlan},
    {"EvaluatePlan",                brain_EvaluatePlan},
    {"GetStartVector3f",            brain_GetStartVector3f},
    {"GetMapWaterRatio",            brain_GetMapWaterRatio},
    {"SetConstantEvaluate",         brain_SetConstantEvaluate},
    {"SetRepeatExecution",          brain_SetRepeatExecution},
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
    {"GetAttackVectors",            brain_GetAttackVectors},
    {"PickBestAttackVector",        brain_PickBestAttackVector},
    {"FindPlaceToBuild",            brain_FindPlaceToBuild},
    {"CanBuildStructureAt",         brain_CanBuildStructureAt},
    {"CreateResourceBuildingNearest", brain_CreateResourceBuildingNearest},
    {"CreateUnitNearSpot",          brain_CreateUnitNearSpot},
    {"BuildUnit",                   brain_BuildUnit},
    {"BuildStructure",              brain_BuildStructure},
    {"DecideWhatToBuild",           brain_DecideWhatToBuild},
    {"FindUpgradeBP",               brain_FindUpgradeBP},
    {"MakePlatoon",                 brain_MakePlatoon},
    {"GetPlatoonUniquelyNamed",     brain_GetPlatoonUniquelyNamed},
    {"GetHighestThreatPosition",    brain_GetHighestThreatPosition},
    {"SetGreaterOf",                brain_SetGreaterOf},
    {"GetArmySkinName",             brain_GetArmySkinName},
    {"GetCurrentEnemy",             brain_GetCurrentEnemy},
    {"SetCurrentEnemy",             brain_SetCurrentEnemy},
    {"GetNumUnitsAroundPoint",      brain_GetNumUnitsAroundPoint},
    {"GetPlatoonsList",             brain_GetPlatoonsList},
    {"CheckBlockingTerrain",    brain_CheckBlockingTerrain},
    {"CanBuildPlatoon",         brain_CanBuildPlatoon},
    {"BuildPlatoon",            brain_BuildPlatoon},
    {nullptr, nullptr},
};
// clang-format on

} // namespace osc::lua
