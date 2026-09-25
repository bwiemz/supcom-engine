// Platoons: moho.platoon_methods (orders, targeting and threat).
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

/// platoon:GetFactionIndex() — the owning army's faction index, as the
/// brain reports it (retail platoon.lua AI threads call it).
static int platoon_GetFactionIndex(lua_State* L) {
    auto* platoon = check_platoon(L);
    auto* sim = get_sim(L);
    auto* brain = (platoon && sim) ? sim->get_army(platoon->army_index()) : nullptr;
    lua_pushnumber(L, brain ? brain->faction() : 1);
    return 1;
}

/// Count live platoon units matching the category at stack index 2; with
/// `around`, only those within radius (index 4) of position (index 3).
static int platoon_count_matching(lua_State* L, bool around) {
    auto* platoon = check_platoon(L);
    auto* sim = get_sim(L);
    if (!platoon || !sim || !lua_istable(L, 2)) {
        lua_pushnumber(L, 0);
        return 1;
    }
    f32 cx = 0, cz = 0, radius_sq = 0;
    if (around) {
        if (!lua_istable(L, 3)) { lua_pushnumber(L, 0); return 1; }
        lua_rawgeti(L, 3, 1);
        cx = static_cast<f32>(lua_tonumber(L, -1));
        lua_rawgeti(L, 3, 3);
        cz = static_cast<f32>(lua_tonumber(L, -1));
        lua_pop(L, 2);
        const f32 r = static_cast<f32>(luaL_optnumber(L, 4, 0));
        radius_sq = r * r;
    }
    int count = 0;
    const CategoryMatcher category(L, 2);
    for (u32 id : platoon->unit_ids()) {
        auto* e = sim->entity_registry().find(id);
        if (!e || e->destroyed() || !e->is_unit()) continue;
        auto* unit = static_cast<sim::Unit*>(e);
        if (!category.matches(unit->category_bits())) continue;
        if (around) {
            const f32 dx = unit->position().x - cx;
            const f32 dz = unit->position().z - cz;
            if (dx * dx + dz * dz > radius_sq) continue;
        }
        ++count;
    }
    lua_pushnumber(L, count);
    return 1;
}

/// platoon:GetSquadPosition(squad) — mean position of the squad's live units
/// (nil if it has none).
static int platoon_GetSquadPosition(lua_State* L) {
    auto* platoon = check_platoon(L);
    auto* sim = get_sim(L);
    const std::string squad = lua_type(L, 2) == LUA_TSTRING ? lua_tostring(L, 2) : "";
    sim::Vector3 sum{0, 0, 0};
    int n = 0;
    if (platoon && sim) {
        for (u32 id : platoon->unit_ids()) {
            auto* e = sim->entity_registry().find(id);
            if (!e || e->destroyed()) continue;
            if (!squad.empty() && platoon->get_unit_squad(id) != squad) continue;
            sum.x += e->position().x;
            sum.y += e->position().y;
            sum.z += e->position().z;
            ++n;
        }
    }
    if (n == 0) {
        lua_pushnil(L);
        return 1;
    }
    push_vector3(L, {sum.x / n, sum.y / n, sum.z / n});
    return 1;
}

/// platoon:CanAttackTarget(squad, target) — can any unit of the squad (all
/// squads if squad is nil) fire on the target's layer?
static int platoon_CanAttackTarget(lua_State* L) {
    auto* platoon = check_platoon(L);
    auto* sim = get_sim(L);
    auto* target = check_entity(L, 3);
    bool can = false;
    if (platoon && sim && target && !target->destroyed() && target->is_unit()) {
        const std::string squad =
            lua_type(L, 2) == LUA_TSTRING ? lua_tostring(L, 2) : "";
        const u8 target_bit =
            sim::layer_to_bit(static_cast<sim::Unit*>(target)->layer());
        for (u32 id : platoon->unit_ids()) {
            if (!squad.empty() && platoon->get_unit_squad(id) != squad) continue;
            auto* e = sim->entity_registry().find(id);
            if (!e || e->destroyed() || !e->is_unit()) continue;
            for (const auto& w : static_cast<sim::Unit*>(e)->weapons()) {
                if (w->enabled && !w->fire_on_death &&
                    (w->fire_target_layer_caps & target_bit) != 0) {
                    can = true;
                    break;
                }
            }
            if (can) break;
        }
    }
    lua_pushboolean(L, can ? 1 : 0);
    return 1;
}

/// platoon:PlatoonCategoryCount(category)
static int platoon_PlatoonCategoryCount(lua_State* L) {
    return platoon_count_matching(L, /*around=*/false);
}

/// platoon:PlatoonCategoryCountAroundPosition(category, position, radius)
static int platoon_PlatoonCategoryCountAroundPosition(lua_State* L) {
    return platoon_count_matching(L, /*around=*/true);
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

/// Whether `name` asks for no formation (retail passes these for a squad
/// that moves loose).
static bool no_formation(const std::string& name) {
    return name.empty() || name == "NoFormation" || name == "None" || name == "none";
}

// platoon:MoveToLocation(position, useTransports), AggressiveMoveToLocation,
// and MoveToTarget(unit): a move for every unit, queued after its current
// orders (retail's AI queues one per waypoint of the route it chose, after a
// Stop). Units move in the formation they were assigned (the platoon's
// override first), each group laid out in its slots (M204).
static int platoon_MoveToLocation(lua_State* L) {
    auto* platoon = check_platoon(L);
    auto* sim = get_sim(L);
    if (!platoon || !sim || !lua_istable(L, 2)) {
        lua_pushnil(L);
        return 1;
    }

    sim::Vector3 pos{};
    if (const auto* target = check_entity(L, 2)) {
        pos = target->position(); // MoveToTarget(unit)
    } else {
        for (int i = 1; i <= 3; ++i) {
            lua_rawgeti(L, 2, i);
            const auto v = lua_isnumber(L, -1) ? static_cast<f32>(lua_tonumber(L, -1)) : 0.0f;
            lua_pop(L, 1);
            (i == 1 ? pos.x : i == 2 ? pos.y : pos.z) = v;
        }
    }

    u32 cmd_id = sim->next_command_id();
    sim::UnitCommand cmd;
    cmd.type = sim::CommandType::Move;
    cmd.target_pos = pos;
    cmd.command_id = cmd_id;

    // One order per formation, in order of first appearance.
    std::vector<std::pair<std::string, std::vector<u32>>> groups;
    for (u32 id : platoon->unit_ids()) {
        auto* e = sim->entity_registry().find(id);
        if (!e || e->destroyed() || !e->is_unit()) continue;
        std::string formation = platoon->unit_formation(id);
        if (no_formation(formation)) formation.clear();
        auto it = std::find_if(groups.begin(), groups.end(),
                               [&](const auto& g) { return g.first == formation; });
        if (it == groups.end()) {
            groups.emplace_back(formation, std::vector<u32>{});
            it = groups.end() - 1;
        }
        it->second.push_back(id);
    }
    for (auto& [formation, ids] : groups) {
        sim::UnitCommand order = cmd;
        order.formation = formation;
        sim->route_command(ids, order, false);
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
    const std::optional<osc::lua::CategoryMatcher> category =
        cat_idx > 0 ? std::optional<osc::lua::CategoryMatcher>(std::in_place, L, cat_idx)
                    : std::nullopt;

    // Get platoon center
    auto center = platoon->get_position(sim->entity_registry());

    // Find the brain for this platoon's army
    auto* brain = sim->get_army(platoon->army_index());
    if (!brain) { lua_pushnil(L); return 1; }
    i32 my_army = brain->index();

    f32 best_dist = 1e30f;
    sim::Entity* best = nullptr;

    sim->entity_registry().for_each_unit([&](const sim::Entity& e) {
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
        if (category && !category->matches(unit->category_bits())) return;

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
    const std::optional<osc::lua::CategoryMatcher> category =
        cat_idx > 0 ? std::optional<osc::lua::CategoryMatcher>(std::in_place, L, cat_idx)
                    : std::nullopt;

    f32 total = 0;
    for (u32 id : platoon->unit_ids()) {
        auto* e = sim->entity_registry().find(id);
        if (!e || e->destroyed() || !e->is_unit()) continue;
        auto* unit = static_cast<sim::Unit*>(e);

        if (category && !category->matches(unit->category_bits())) continue;

        total += get_unit_threat_for_type(unit, threat_type);
    }

    lua_pushnumber(L, total);
    return 1;
}

// platoon:CalculatePlatoonThreatAroundPosition(threatType, category, position, radius)
// Same as CalculatePlatoonThreat but filters units to within radius of position.
static int platoon_CalculatePlatoonThreatAroundPosition(lua_State* L) {
    auto* platoon = check_platoon(L);
    auto* sim = get_sim(L);
    if (!platoon || !sim) {
        lua_pushnumber(L, 0);
        return 1;
    }

    const char* threat_type = lua_isstring(L, 2) ? lua_tostring(L, 2) : "Overall";
    int cat_idx = lua_istable(L, 3) ? 3 : 0;
    const std::optional<osc::lua::CategoryMatcher> category =
        cat_idx > 0 ? std::optional<osc::lua::CategoryMatcher>(std::in_place, L, cat_idx)
                    : std::nullopt;

    // Extract position from arg 4 ({x, y, z} table)
    f32 px = 0, pz = 0;
    if (lua_istable(L, 4)) {
        lua_rawgeti(L, 4, 1);
        px = static_cast<f32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        lua_rawgeti(L, 4, 3);
        pz = static_cast<f32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
    }

    f32 radius = lua_isnumber(L, 5) ? static_cast<f32>(lua_tonumber(L, 5)) : 0;
    f32 radius_sq = radius * radius;

    f32 total = 0;
    for (u32 id : platoon->unit_ids()) {
        auto* e = sim->entity_registry().find(id);
        if (!e || e->destroyed() || !e->is_unit()) continue;
        auto* unit = static_cast<sim::Unit*>(e);

        if (category && !category->matches(unit->category_bits())) continue;

        // Distance filter (2D, ignoring Y)
        if (radius > 0) {
            auto pos = unit->position();
            f32 dx = pos.x - px;
            f32 dz = pos.z - pz;
            if (dx * dx + dz * dz > radius_sq) continue;
        }

        total += get_unit_threat_for_type(unit, threat_type);
    }

    lua_pushnumber(L, total);
    return 1;
}

// platoon:CanFormPlatoon(template, count, location, radius)
// template = {name, plan, {category_expr, min, max, squad, formation}, ...}
// count = multiplier for min/max counts
// location = optional {x, y, z} position
// radius = optional search radius
// Returns: boolean
static int platoon_CanFormPlatoon(lua_State* L) {
    auto* platoon = check_platoon(L);
    auto* sim = get_sim(L);
    if (!platoon || !sim || !lua_istable(L, 2)) {
        lua_pushboolean(L, 0);
        return 1;
    }

    int tmpl = 2;
    int multiplier = lua_isnumber(L, 3)
                         ? static_cast<int>(lua_tonumber(L, 3))
                         : 1;
    if (multiplier < 1) multiplier = 1;

    // Optional location + radius filter
    bool has_location = lua_istable(L, 4) && lua_isnumber(L, 5);
    f32 loc_x = 0, loc_z = 0, radius_sq = 0;
    if (has_location) {
        lua_rawgeti(L, 4, 1);
        loc_x = static_cast<f32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        lua_rawgeti(L, 4, 3);
        loc_z = static_cast<f32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        f32 r = static_cast<f32>(lua_tonumber(L, 5));
        radius_sq = r * r;
    }

    // Iterate template sub-tables starting at index 3
    for (int i = 3; ; i++) {
        lua_rawgeti(L, tmpl, i);
        if (lua_isnil(L, -1)) { lua_pop(L, 1); break; }
        if (!lua_istable(L, -1)) { lua_pop(L, 1); continue; }
        int sub = lua_gettop(L);

        // sub[1] = category expression (Lua table)
        lua_rawgeti(L, sub, 1);
        int cat_idx = lua_gettop(L);
        if (!lua_istable(L, cat_idx)) {
            lua_pop(L, 2); // cat + sub
            continue;
        }

        // sub[2] = min count
        lua_rawgeti(L, sub, 2);
        int min_count = lua_isnumber(L, -1)
                            ? static_cast<int>(lua_tonumber(L, -1)) * multiplier
                            : multiplier;
        lua_pop(L, 1);

        // Count matching units in pool
        const osc::lua::CategoryMatcher category(L, cat_idx);
        int matched = 0;
        for (u32 id : platoon->unit_ids()) {
            auto* e = sim->entity_registry().find(id);
            if (!e || e->destroyed() || !e->is_unit()) continue;
            auto* unit = static_cast<sim::Unit*>(e);

            if (!category.matches(unit->category_bits())) continue;

            if (has_location) {
                auto pos = unit->position();
                f32 dx = pos.x - loc_x;
                f32 dz = pos.z - loc_z;
                if (dx * dx + dz * dz > radius_sq) continue;
            }

            matched++;
        }

        lua_pop(L, 1); // cat_idx
        lua_pop(L, 1); // sub

        if (matched < min_count) {
            lua_pushboolean(L, 0);
            return 1;
        }
    }

    lua_pushboolean(L, 1);
    return 1;
}

// platoon:FormPlatoon(template, count, location, radius)
// Same args as CanFormPlatoon. Extracts matching units from pool
// into a new platoon and returns it.
// Returns: new platoon Lua table
static int platoon_FormPlatoon(lua_State* L) {
    auto* pool = check_platoon(L);
    auto* sim = get_sim(L);
    if (!pool || !sim || !lua_istable(L, 2)) {
        lua_pushnil(L);
        return 1;
    }

    int tmpl = 2;
    int multiplier = lua_isnumber(L, 3)
                         ? static_cast<int>(lua_tonumber(L, 3))
                         : 1;
    if (multiplier < 1) multiplier = 1;

    // Optional location + radius filter
    bool has_location = lua_istable(L, 4) && lua_isnumber(L, 5);
    f32 loc_x = 0, loc_z = 0, radius_sq = 0;
    if (has_location) {
        lua_rawgeti(L, 4, 1);
        loc_x = static_cast<f32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        lua_rawgeti(L, 4, 3);
        loc_z = static_cast<f32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        f32 r = static_cast<f32>(lua_tonumber(L, 5));
        radius_sq = r * r;
    }

    // Get brain to create new platoon
    auto* brain = sim->get_army(pool->army_index());
    if (!brain) { lua_pushnil(L); return 1; }

    // Get plan name from template[2]
    lua_rawgeti(L, tmpl, 2);
    std::string plan = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
    lua_pop(L, 1);

    auto* new_platoon = brain->create_platoon("");
    new_platoon->set_plan_name(plan);

    // Snapshot pool unit IDs (remove_unit modifies the vector)
    auto pool_ids = pool->unit_ids();

    // Track which IDs we've transferred (to remove from pool after)
    std::vector<u32> transferred;

    // Iterate template sub-tables starting at index 3
    for (int i = 3; ; i++) {
        lua_rawgeti(L, tmpl, i);
        if (lua_isnil(L, -1)) { lua_pop(L, 1); break; }
        if (!lua_istable(L, -1)) { lua_pop(L, 1); continue; }
        int sub = lua_gettop(L);

        // sub[1] = category expression
        lua_rawgeti(L, sub, 1);
        int cat_idx = lua_gettop(L);
        if (!lua_istable(L, cat_idx)) {
            lua_pop(L, 2); // cat + sub
            continue;
        }

        // sub[2] = min, sub[3] = max
        lua_rawgeti(L, sub, 2);
        int min_count = lua_isnumber(L, -1)
                            ? static_cast<int>(lua_tonumber(L, -1)) * multiplier
                            : multiplier;
        lua_pop(L, 1);
        // Known gap (roadmap M207): FA's FormPlatoon returns nil when a squad
        // cannot reach its minimum count; this forms a partial platoon. AI
        // scripts usually gate on CanFormPlatoon (which does check minimums),
        // so it only matters when they call FormPlatoon directly.
        static_cast<void>(min_count);

        lua_rawgeti(L, sub, 3);
        int max_count = lua_isnumber(L, -1)
                            ? static_cast<int>(lua_tonumber(L, -1)) * multiplier
                            : multiplier;
        lua_pop(L, 1);

        // sub[4] = squad name, sub[5] = its formation
        lua_rawgeti(L, sub, 4);
        std::string squad = lua_isstring(L, -1) ? lua_tostring(L, -1) : "Unassigned";
        lua_pop(L, 1);
        lua_rawgeti(L, sub, 5);
        const std::string formation = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "";
        lua_pop(L, 1);

        // Find matching units and transfer
        const osc::lua::CategoryMatcher category(L, cat_idx);
        int taken = 0;
        for (u32 id : pool_ids) {
            if (taken >= max_count) break;

            // Skip if already transferred in a previous sub-table
            bool already = false;
            for (u32 t : transferred) {
                if (t == id) { already = true; break; }
            }
            if (already) continue;

            auto* e = sim->entity_registry().find(id);
            if (!e || e->destroyed() || !e->is_unit()) continue;
            auto* unit = static_cast<sim::Unit*>(e);

            if (!category.matches(unit->category_bits())) continue;

            if (has_location) {
                auto pos = unit->position();
                f32 dx = pos.x - loc_x;
                f32 dz = pos.z - loc_z;
                if (dx * dx + dz * dz > radius_sq) continue;
            }

            new_platoon->add_unit(id);
            new_platoon->set_unit_squad(id, squad);
            new_platoon->set_unit_formation(id, formation);
            transferred.push_back(id);
            taken++;
        }

        lua_pop(L, 1); // cat_idx
        lua_pop(L, 1); // sub
    }

    // Remove transferred units from pool
    for (u32 id : transferred) {
        pool->remove_unit(id);
    }

    // Build Lua table for new platoon (same pattern as brain_MakePlatoon)
    lua_newtable(L);
    int plat_tbl = lua_gettop(L);

    lua_pushstring(L, "_c_object");
    lua_pushlightuserdata(L, new_platoon);
    lua_rawset(L, plat_tbl);

    lua_pushstring(L, "PlanName");
    lua_pushstring(L, plan.c_str());
    lua_rawset(L, plat_tbl);

    // Set metatable (cached __osc_platoon_mt or __platoon_class)
    lua_pushstring(L, "__platoon_class");
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_pushstring(L, "__osc_platoon_mt");
        lua_rawget(L, LUA_REGISTRYINDEX);
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            // Create cached metatable
            lua_newtable(L);
            lua_pushstring(L, "__index");
            lua_pushstring(L, "moho");
            lua_rawget(L, LUA_GLOBALSINDEX);
            if (lua_istable(L, -1)) {
                lua_pushstring(L, "platoon_methods");
                lua_rawget(L, -2);
                lua_remove(L, -2);
            } else {
                lua_pop(L, 1);
                lua_pushnil(L);
            }
            lua_settable(L, -3);
            lua_pushstring(L, "__osc_platoon_mt");
            lua_pushvalue(L, -2);
            lua_rawset(L, LUA_REGISTRYINDEX);
        }
    }
    lua_setmetatable(L, plat_tbl);

    // Store lua_table_ref on platoon
    lua_pushvalue(L, plat_tbl);
    new_platoon->set_lua_table_ref(luaL_ref(L, LUA_REGISTRYINDEX));

    // Call OnCreate if available
    lua_pushstring(L, "OnCreate");
    lua_gettable(L, plat_tbl);
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, plat_tbl);
        lua_pushstring(L, plan.c_str());
        if (lua_pcall(L, 2, 0, 0) != 0) {
            spdlog::warn("FormPlatoon OnCreate error: {}", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    } else {
        lua_pop(L, 1);
    }

    spdlog::info("FormPlatoon: id={} army={} plan='{}' units={}",
                 new_platoon->platoon_id(), brain->index(), plan,
                 new_platoon->unit_ids().size());

    // Return the platoon table (defensive push matching brain_MakePlatoon pattern)
    lua_pushvalue(L, plat_tbl);
    return 1;
}

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

// clang-format off
const MethodEntry platoon_methods[] = {
    {"Destroy",                     platoon_Destroy},
    {"GetPlatoonUnits",             platoon_GetPlatoonUnits},
    {"GetFactionIndex",             platoon_GetFactionIndex},
    {"GetSquadPosition",            platoon_GetSquadPosition},
    {"CanAttackTarget",             platoon_CanAttackTarget},
    {"PlatoonCategoryCount",        platoon_PlatoonCategoryCount},
    {"PlatoonCategoryCountAroundPosition", platoon_PlatoonCategoryCountAroundPosition},
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
    {"GetPlatoonThreat",                        platoon_CalculatePlatoonThreat},
    {"CalculatePlatoonThreatAroundPosition",    platoon_CalculatePlatoonThreatAroundPosition},
    {"TurnOffPoolAI",               [](lua_State*) -> int { return 0; }},
    {"CanFormPlatoon",          platoon_CanFormPlatoon},
    {"FormPlatoon",             platoon_FormPlatoon},
    {nullptr, nullptr},
};
// clang-format on

} // namespace osc::lua
