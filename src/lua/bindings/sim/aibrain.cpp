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

// brain:AssignThreatAtPosition(pos, amount, decay) — writes threat to the
// threat map. Stub: our threat map is read-only (computed from units) so we
// just accept and discard the manual override.
static int brain_AssignThreatAtPosition(lua_State*) {
    return 0;
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

// brain:GetMapWaterRatio() → fraction of map that is water (0.0–1.0)
static int brain_GetMapWaterRatio(lua_State* L) {
    // Stub: return 0.0 (land map)
    lua_pushnumber(L, 0.0);
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
    const char* threat_type = (lua_type(L, 5) == LUA_TSTRING) ? lua_tostring(L, 5) : "Overall";

    // Optional armyIdx (arg 6) — filter to specific army instead of enemies
    bool filter_specific = lua_isnumber(L, 6);
    i32 specific_army = filter_specific
                            ? static_cast<i32>(lua_tonumber(L, 6)) - 1 // Lua 1-based
                            : -1;

    const auto units = sim->entity_registry().units_in_radius(px, pz, radius);

    f32 total = 0;
    for (auto* entity : units) {
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
    const char* threat_type = (lua_type(L, 5) == LUA_TSTRING) ? lua_tostring(L, 5) : "Overall";

    // Optional armyIdx (arg 6) — filter to specific army instead of enemies
    bool filter_specific = lua_isnumber(L, 6);
    i32 specific_army = filter_specific
                            ? static_cast<i32>(lua_tonumber(L, 6)) - 1
                            : -1;

    auto ids = sim->entity_registry().collect_in_radius(px, pz, radius);

    // Bucket threats into 32x32 cells. An ordered map, summed in id order:
    // the list goes to scripts, and a hash map's order differs between
    // standard libraries -- the air-scout AI takes the first entry, so
    // Windows and Linux games parted at their first scouting run.
    constexpr f32 CELL_SIZE = 32.0f;
    std::map<std::pair<i32, i32>, f32> cells;

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

        cells[{static_cast<i32>(std::floor(unit->position().x / CELL_SIZE)),
               static_cast<i32>(std::floor(unit->position().z / CELL_SIZE))}] += threat;
    }

    // Return table of {cellX, cellZ, threatValue}: the most threatening
    // first, as the scripts that take entry [1] expect; ties by cell.
    std::vector<std::pair<std::pair<i32, i32>, f32>> ordered(cells.begin(), cells.end());
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const auto& a, const auto& b) { return a.second > b.second; });
    lua_newtable(L);
    int idx = 1;
    for (const auto& [key, threat] : ordered) {
        lua_pushnumber(L, idx++);
        lua_newtable(L);
        lua_pushnumber(L, 1);
        lua_pushnumber(L, key.first * CELL_SIZE + CELL_SIZE * 0.5f);
        lua_rawset(L, -3);
        lua_pushnumber(L, 2);
        lua_pushnumber(L, key.second * CELL_SIZE + CELL_SIZE * 0.5f);
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
    const char* threat_type = (lua_type(L, 4) == LUA_TSTRING) ? lua_tostring(L, 4) : "Overall";

    bool filter_specific = lua_isnumber(L, 5);
    i32 specific_army = filter_specific
                            ? static_cast<i32>(lua_tonumber(L, 5)) - 1
                            : -1;

    f32 best_threat = 0;
    sim::Vector3 best_pos{0, 0, 0};

    sim->entity_registry().for_each_unit([&](const sim::Entity& e) {
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

        const auto units = sim->entity_registry().units_in_radius(px, pz, SAMPLE_SPACING);
        f32 sample_threat = 0;
        for (auto* entity : units) {
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
    auto number_field = [L](int table, const char* key, f32& out) {
        lua_pushstring(L, key);
        lua_rawget(L, table);
        if (lua_isnumber(L, -1)) out = static_cast<f32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
    };

    lua_pushstring(L, "Footprint");
    lua_rawget(L, bp);
    if (lua_istable(L, -1)) {
        number_field(lua_gettop(L), "SizeX", r.size_x);
        number_field(lua_gettop(L), "SizeZ", r.size_z);
    }
    lua_pop(L, 1);

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
        }
        lua_pop(L, 1);
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

static sim::StructurePlacement placement_for(lua_State* L, const sim::SimState& sim,
                                             i32 army) {
    return sim::StructurePlacement(
        sim, army, [L](const std::string& bp_id) { return placement_rules_of(L, bp_id); });
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
// their own reference point. Not yet applied: the enemy-threat cutoff
// (optIgnoreThreatOver) for resource sites.
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
    bool found = false;
    f32 best_x = 0, best_z = 0, best_d2 = 0;
    auto consider = [&](f32 answer_x, f32 answer_z, f32 world_x, f32 world_z,
                        f32 from_x, f32 from_z) {
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

// Not called in FA — named no-ops to replace generic stubs
static int brain_SetArmyStatsTrigger(lua_State*) { return 0; }
static int brain_RemoveArmyStatsTrigger(lua_State*) { return 0; }
static int brain_RemoveEnergyDependingEntity(lua_State*) { return 0; }
static int brain_PBMAddBuildLocation(lua_State*) { return 0; }
static int brain_PBMRemoveBuildLocation(lua_State*) { return 0; }
// brain:SetUpAttackVectorsToArmy([category]): attack vectors on the current
// enemy (the AI sets itself as enemy to find its own bases). Its units of
// the category -- STRUCTURE - MOBILE without one -- grouped into 32x32
// cells, each group a point (its centre) and the heading from this army's
// start to it. Groups in cell order: the list reaches the AI's scripts.
static int brain_SetUpAttackVectorsToArmy(lua_State* L) {
    auto* brain = check_brain(L);
    auto* sim = get_sim(L);
    if (!brain || !sim) return 0;
    const i32 enemy = brain->current_enemy_index();
    std::vector<sim::ArmyBrain::AttackVector> vectors;
    if (enemy < 0) {
        brain->set_attack_vectors({});
        return 0;
    }
    const bool any = !lua_istable(L, 2);
    const sim::CategoryExpr wanted = any ? sim::CategoryExpr{} : sim::compile_category(L, 2);
    struct Group {
        f64 x = 0, y = 0, z = 0;
        u32 count = 0;
    };
    constexpr f32 kCell = 32.0f;
    std::map<std::pair<i32, i32>, Group> groups;
    sim->entity_registry().for_each_unit([&](sim::Entity& e) {
        if (e.destroyed() || e.army() != enemy) return;
        const auto& u = static_cast<const sim::Unit&>(e);
        if (u.is_dying()) return;
        const bool matches = any ? u.has_category("STRUCTURE") && !u.has_category("MOBILE")
                                 : wanted.matches(u.categories());
        if (!matches) return;
        const auto& p = u.position();
        auto& g = groups[{static_cast<i32>(std::floor(p.x / kCell)),
                          static_cast<i32>(std::floor(p.z / kCell))}];
        g.x += p.x;
        g.y += p.y;
        g.z += p.z;
        ++g.count;
    });
    const sim::Vector3 home = brain->start_position();
    for (const auto& [cell, g] : groups) {
        const sim::Vector3 at{static_cast<f32>(g.x / g.count), static_cast<f32>(g.y / g.count),
                              static_cast<f32>(g.z / g.count)};
        const f32 dx = at.x - home.x;
        const f32 dz = at.z - home.z;
        const f32 len = std::sqrt(dx * dx + dz * dz);
        vectors.push_back({at, len > 1e-3f ? sim::Vector3{dx / len, 0.0f, dz / len}
                                           : sim::Vector3{0.0f, 0.0f, 1.0f}});
    }
    brain->set_attack_vectors(std::move(vectors));
    return 0;
}

// brain:GetAttackVectors() -> {{px, py, pz, vx, vy, vz}, ...}, as
// SetUpAttackVectorsToArmy left them.
static int brain_GetAttackVectors(lua_State* L) {
    auto* brain = check_brain(L);
    lua_newtable(L);
    if (!brain) return 1;
    int i = 1;
    for (const auto& v : brain->attack_vectors()) {
        lua_newtable(L);
        const std::pair<const char*, f32> fields[] = {
            {"px", v.position.x},  {"py", v.position.y},  {"pz", v.position.z},
            {"vx", v.direction.x}, {"vy", v.direction.y}, {"vz", v.direction.z}};
        for (const auto& [key, value] : fields) {
            lua_pushstring(L, key);
            lua_pushnumber(L, value);
            lua_rawset(L, -3);
        }
        lua_rawseti(L, -2, i++);
    }
    return 1;
}
static int brain_SetGreaterOf(lua_State*) { return 0; }

// brain:CheckBlockingTerrain(pos, maxRange, threatType)
// No-op stub — returns false (no blocking terrain).
// Called by aiattackutilities.lua for attack path validation.
static int brain_CheckBlockingTerrain(lua_State* L) {
    lua_pushboolean(L, 0);
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
    {"FindPlaceToBuild",            brain_FindPlaceToBuild},
    {"CanBuildStructureAt",         brain_CanBuildStructureAt},
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
