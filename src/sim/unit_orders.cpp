// A unit's orders (M193): one handler per kind, run from the head of the
// queue each tick (Unit::tick_orders). A handler says whether its order goes
// on (OrderStep).

#include "sim/unit.hpp"
#include "core/test_status.hpp"
#include "sim/blueprint_categories.hpp"
#include "sim/entity_registry.hpp"
#include "sim/sim_state.hpp"
#include "sim/work_range.hpp"
#include "map/terrain.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <spdlog/spdlog.h>

extern "C" {
#include <lua.h>
}

namespace osc::sim {

namespace {
std::unordered_set<std::string> read_blueprint_categories(lua_State* L, const std::string& bp_id) {
    std::unordered_set<std::string> categories;
    if (!L || bp_id.empty()) return categories;

    lua_pushstring(L, "__blueprints");
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return categories;
    }

    lua_pushstring(L, bp_id.c_str());
    lua_gettable(L, -2);
    collect_blueprint_categories(L, lua_gettop(L), categories);
    lua_pop(L, 2); // blueprint entry + __blueprints
    return categories;
}

/// A number in unit blueprint `bp_id`'s Economy table, or `fallback`.
f32 blueprint_economy_number(lua_State* L, const std::string& bp_id, const char* field,
                             f32 fallback) {
    if (!L || bp_id.empty()) return fallback;
    std::string key = bp_id;
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    f32 value = fallback;
    const int top = lua_gettop(L);
    lua_pushstring(L, "__blueprints");
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (lua_istable(L, -1)) {
        lua_pushstring(L, key.c_str());
        lua_rawget(L, -2);
        if (lua_istable(L, -1)) {
            lua_pushstring(L, "Economy");
            lua_rawget(L, -2);
            if (lua_istable(L, -1)) {
                lua_pushstring(L, field);
                lua_rawget(L, -2);
                if (lua_type(L, -1) == LUA_TNUMBER) value = static_cast<f32>(lua_tonumber(L, -1));
            }
        }
    }
    lua_settop(L, top);
    return value;
}

bool build_blocked_by_lobby_rules(const Unit& builder, const UnitCommand& cmd,
                                  const SimContext& ctx) {
    if (!ctx.sim) return false;

    auto* brain = ctx.sim->get_army(builder.army());
    if (!brain) return false;

    if (brain->unit_cap() >= 0 && brain->get_unit_cost_total(ctx.registry) >= brain->unit_cap()) {
        spdlog::info("Build blocked: army {} unit cap {} reached for {}", builder.army(),
                     brain->unit_cap(), cmd.blueprint_id);
        return true;
    }

    auto categories = read_blueprint_categories(ctx.L, cmd.blueprint_id);
    if (!categories.empty() && brain->is_build_restricted(categories)) {
        spdlog::info("Build blocked: army {} restricted blueprint {}", builder.army(),
                     cmd.blueprint_id);
        return true;
    }

    return false;
}

/// What reclaiming `target` takes and yields, as Moho asks the reclaimer's
/// script: GetReclaimCosts(target) -> seconds, energy, mass (retail's Unit
/// answers from a unit's build costs and asks a prop, whose Prop.lua answers
/// from its blueprint's reclaim values and time multipliers). Without an
/// answer: the values set on the target's table (MaxMassReclaim,
/// MaxEnergyReclaim, TimeReclaim), with the time scaled as before.
struct ReclaimCosts {
    f64 time = 0;
    f64 energy = 0;
    f64 mass = 0;
};

ReclaimCosts reclaim_costs(lua_State* L, const Unit& reclaimer, const Entity& target,
                           f64 build_rate) {
    ReclaimCosts costs;
    if (!L || target.lua_table_ref() < 0) return costs;
    const int top = lua_gettop(L);
    if (reclaimer.lua_table_ref() >= 0) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, reclaimer.lua_table_ref());
        const int self = lua_gettop(L);
        lua_pushstring(L, "GetReclaimCosts");
        lua_gettable(L, self);
        if (lua_isfunction(L, -1)) {
            lua_pushvalue(L, self);
            lua_rawgeti(L, LUA_REGISTRYINDEX, target.lua_table_ref());
            if (lua_pcall(L, 2, 3, 0) == 0 && lua_isnumber(L, -3)) {
                costs.time = lua_tonumber(L, -3);
                costs.energy = std::fabs(lua_tonumber(L, -2));
                costs.mass = std::fabs(lua_tonumber(L, -1));
                lua_settop(L, top);
                return costs;
            }
            if (lua_isstring(L, -1)) spdlog::warn("GetReclaimCosts error: {}", lua_tostring(L, -1));
        }
        lua_settop(L, top);
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, target.lua_table_ref());
    const int tbl = lua_gettop(L);
    f64 time_mult = 1;
    const auto number = [&](const char* key, f64& out) {
        lua_pushstring(L, key);
        lua_rawget(L, tbl);
        if (lua_isnumber(L, -1)) out = lua_tonumber(L, -1);
        lua_pop(L, 1);
    };
    number("MaxMassReclaim", costs.mass);
    number("MaxEnergyReclaim", costs.energy);
    number("TimeReclaim", time_mult);
    lua_settop(L, top);
    if (build_rate > 0)
        costs.time = time_mult * std::max(costs.mass, costs.energy) / build_rate / 10.0;
    return costs;
}

} // namespace

u32 Unit::ferry_beacon(SimContext& ctx, UnitCommand& head) {
    auto& registry = ctx.registry;
    if (head.beacon_id != 0) {
        const Entity* beacon = registry.find(head.beacon_id);
        if (beacon && !beacon->destroyed()) return head.beacon_id;
        head.beacon_id = 0;
    }
    // A transport of the army ferrying from here keeps one already: an order
    // given to several shares its beacon, as Moho's shared command does.
    u32 shared = 0;
    registry.for_each_unit([&](const Entity& e) {
        if (shared != 0 || e.destroyed() || !e.is_unit() || e.army() != army() ||
            e.entity_id() == entity_id())
            return;
        for (const UnitCommand& c : static_cast<const Unit&>(e).command_queue()) {
            if (c.type != CommandType::Ferry || c.beacon_id == 0) continue;
            const Entity* beacon = registry.find(c.beacon_id);
            const f32 dx = c.target_pos.x - head.target_pos.x;
            const f32 dz = c.target_pos.z - head.target_pos.z;
            if (beacon && !beacon->destroyed() && dx * dx + dz * dz <= 1.0f) {
                shared = c.beacon_id;
                return;
            }
        }
    });
    if (shared != 0) {
        head.beacon_id = shared;
        return shared;
    }
    // Else a new one, of its blueprint's AI.BeaconName.
    lua_State* L = ctx.L;
    if (!L || !ctx.sim) return 0;
    std::string key = blueprint_id();
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::string beacon_bp;
    const int top = lua_gettop(L);
    lua_pushstring(L, "__blueprints");
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (lua_istable(L, -1)) {
        lua_pushstring(L, key.c_str());
        lua_rawget(L, -2);
        if (lua_istable(L, -1)) {
            lua_pushstring(L, "AI");
            lua_rawget(L, -2);
            if (lua_istable(L, -1)) {
                lua_pushstring(L, "BeaconName");
                lua_rawget(L, -2);
                if (lua_type(L, -1) == LUA_TSTRING) beacon_bp = lua_tostring(L, -1);
            }
        }
    }
    lua_settop(L, top);
    if (beacon_bp.empty()) return 0;
    lua_pushstring(L, "CreateUnitHPR");
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (!lua_isfunction(L, -1)) {
        lua_settop(L, top);
        return 0;
    }
    const f32 y = ctx.terrain
                      ? ctx.terrain->get_terrain_height(head.target_pos.x, head.target_pos.z)
                      : head.target_pos.y;
    lua_pushstring(L, beacon_bp.c_str());
    lua_pushnumber(L, army() + 1);
    lua_pushnumber(L, head.target_pos.x);
    lua_pushnumber(L, y);
    lua_pushnumber(L, head.target_pos.z);
    lua_pushnumber(L, 0);
    lua_pushnumber(L, 0);
    lua_pushnumber(L, 0);
    u32 made = 0;
    if (lua_pcall(L, 8, 1, 0) == 0) {
        if (lua_istable(L, -1)) {
            lua_pushstring(L, "EntityId");
            lua_rawget(L, -2);
            if (lua_isnumber(L, -1)) made = static_cast<u32>(lua_tonumber(L, -1));
        }
    } else {
        const char* err = lua_tostring(L, -1);
        const std::string message =
            std::string("ferry beacon ") + beacon_bp + ": " + (err ? err : "(unknown)");
        spdlog::warn("{}", message);
        if (test_status::count_lua_failures()) test_status::record_failure(message);
    }
    lua_settop(L, top);
    if (made == 0) return 0;
    head.beacon_id = made;
    ctx.sim->track_ferry_beacon(made);
    // Its script hears the route set (no retail script listens).
    call_lua_method(L, "OnFerryPointSet");
    return made;
}

bool Unit::ferry_fly(f64 dt, SimContext& ctx, const Vector3& to) {
    const Vector3 heading = navigator_.goal();
    if (!ferry_leg_set_ || std::abs(heading.x - to.x) > 1.0f || std::abs(heading.z - to.z) > 1.0f ||
        navigator_.status() == Navigator::Status::WaitingForPath) {
        navigator_.set_goal(to, ctx.pathfinder, position(), layer_, naval_draft_,
                            is_amphibious() || is_hover());
        ferry_leg_set_ = true;
    }
    const bool going = nav_update(dt, ctx.terrain);
    if (!going) ferry_leg_set_ = false;
    return going;
}

bool Unit::approach_update(f64 dt, SimContext& ctx) {
    if (navigator_.status() == Navigator::Status::WaitingForPath) {
        const Vector3 goal = navigator_.goal();
        navigator_.set_goal(goal, ctx.pathfinder, position(), layer_, naval_draft_,
                            is_amphibious() || is_hover());
    }
    return nav_update(dt, ctx.terrain);
}

bool Unit::tick_orders(f64 dt, SimContext& ctx, f32 econ_eff) {
    while (!command_queue_.empty()) {
        // Orders run script callbacks, which may destroy this unit (it stays
        // allocated until the tick ends, see EntityRegistry::collect_garbage).
        if (destroyed() || !in_registry()) return false;
        const OrderStep step = run_order(command_queue_.front(), dt, ctx, econ_eff);
        if (step == OrderStep::Hold) return true;
        if (step == OrderStep::Gone) return false;
    }
    return true;
}

OrderStep Unit::run_order(UnitCommand& cmd, f64 dt, SimContext& ctx, f32 econ_eff) {
    switch (cmd.type) {
    case CommandType::Stop: return order_stop();
    case CommandType::Move: return order_move(cmd, dt, ctx);
    case CommandType::Attack: return order_attack(cmd, dt, ctx);
    case CommandType::BuildMobile: return order_build_mobile(cmd, dt, ctx, econ_eff);
    case CommandType::BuildFactory:
    case CommandType::Upgrade: return order_build_in_place(cmd, dt, ctx, econ_eff);
    case CommandType::Patrol: return order_patrol(cmd, dt, ctx);
    case CommandType::Reclaim: return order_reclaim(cmd, dt, ctx);
    case CommandType::Repair: return order_repair(cmd, dt, ctx, econ_eff);
    case CommandType::Capture: return order_capture(cmd, dt, ctx, econ_eff);
    case CommandType::Guard: return order_guard(cmd, dt, ctx, econ_eff);
    case CommandType::Dive: return order_dive(ctx.L);
    case CommandType::Enhance: return order_enhance(cmd, dt, ctx, econ_eff);
    case CommandType::TransportLoad: return order_transport_load(cmd, dt, ctx);
    case CommandType::TransportUnload: return order_transport_unload(cmd, dt, ctx);
    case CommandType::Nuke:
    case CommandType::Tactical:
    case CommandType::Overcharge: return order_launch(cmd, dt, ctx);
    case CommandType::Sacrifice: return order_sacrifice(cmd, dt, ctx);
    case CommandType::Teleport: return order_teleport(cmd, ctx.L);
    case CommandType::Ferry: return order_ferry(cmd, dt, ctx);
    case CommandType::WaitForFerry: return order_wait_for_ferry(cmd, dt, ctx);
    default: // an order no handler runs: dropped
        command_queue_.pop_front();
        return OrderStep::Next;
    }
}

OrderStep Unit::order_stop() {
    navigator_.abort_move();
    command_queue_.pop_front();
    return OrderStep::Next;
}

OrderStep Unit::order_move(UnitCommand& cmd, f64 dt, SimContext& ctx) {
    if (!navigator_.is_moving() || navigator_.goal().x != cmd.target_pos.x ||
        navigator_.goal().z != cmd.target_pos.z) {
        navigator_.set_goal(cmd.target_pos, ctx.pathfinder, position(), layer_, naval_draft_,
                            is_amphibious() || is_hover());
    }
    if (!nav_update(dt, ctx.terrain, cmd.speed_cap)) {
        command_queue_.pop_front();
        return OrderStep::Next;
    }
    return OrderStep::Hold; // Still moving
}

OrderStep Unit::order_attack(UnitCommand& cmd, f64 dt, SimContext& ctx) {
    auto& registry = ctx.registry;
    // Attack: move toward target if out of weapon range, else stop
    if (cmd.target_id == 0) {
        command_queue_.pop_front();
        return OrderStep::Next;
    }
    auto* target = registry.find(cmd.target_id);
    if (!target || target->destroyed()) {
        command_queue_.pop_front();
        return OrderStep::Next;
    }
    // Find max weapon range
    f32 best_range = 0;
    for (auto& w : weapons_) {
        if (w->enabled && !w->fire_on_death && !w->manual_fire)
            best_range = std::max(best_range, w->max_range);
    }
    if (best_range <= 0) {
        command_queue_.pop_front();
        return OrderStep::Next;
    }
    // Check distance to target
    f32 dx = target->position().x - position().x;
    f32 dz = target->position().z - position().z;
    f32 dist2 = dx * dx + dz * dz;
    f32 range2 = best_range * best_range;
    if (dist2 > range2) {
        // Move toward target
        if (!navigator_.is_moving() || navigator_.goal().x != target->position().x ||
            navigator_.goal().z != target->position().z) {
            navigator_.set_goal(target->position(), ctx.pathfinder, position(), layer_,
                                naval_draft_, is_amphibious() || is_hover());
        }
        nav_update(dt, ctx.terrain);
    } else {
        navigator_.abort_move();
    }
    return OrderStep::Hold; // Stay on this command
}

OrderStep Unit::order_build_mobile(UnitCommand& cmd, f64 dt, SimContext& ctx, f32 econ_eff) {
    auto& registry = ctx.registry;
    auto* L = ctx.L;
    if (build_target_id_ == 0) {
        // Phase 1: reach the site. In range when the gap to its skirt
        // is within MaxBuildDistance and the builder is off the
        // skirt (Moho's CUnitMobileBuildTask); else it walks just
        // clear of the skirt, one cell out, and builds from there or
        // gives up.
        if (cmd.site_skirt_x <= 0) {
            const auto [sx, sz] = blueprint_skirt(L, cmd.blueprint_id);
            cmd.site_skirt_x = sx;
            cmd.site_skirt_z = sz;
        }
        const f32 half_x = cmd.site_skirt_x * 0.5f;
        const f32 half_z = cmd.site_skirt_z * 0.5f;
        const auto reachable = [&] {
            const f32 skirt = std::max(cmd.site_skirt_x, cmd.site_skirt_z);
            const f32 room = footprint_extent(*this) * 0.5f;
            const bool on_site = std::abs(position().x - cmd.target_pos.x) <= half_x + room &&
                                 std::abs(position().z - cmd.target_pos.z) <= half_z + room;
            return !on_site && work_gap(*this, cmd.target_pos, skirt) <= max_build_distance_;
        };
        if (!cmd.approached && !reachable()) {
            if (effective_speed() <= 0) {
                command_queue_.pop_front();
                return OrderStep::Next;
            }
            cmd.approached = true;
            navigator_.set_goal(approach_point(*this, cmd.target_pos, half_x + 1, half_z + 1),
                                ctx.pathfinder, position(), layer_, naval_draft_,
                                is_amphibious() || is_hover());
        }
        if (cmd.approached && approach_update(dt, ctx)) return OrderStep::Hold;
        if (!reachable()) {
            command_queue_.pop_front();
            return OrderStep::Next;
        }
        navigator_.abort_move();

        // Phase 2: Spawn skeleton unit
        if (build_blocked_by_lobby_rules(*this, cmd, ctx)) {
            command_queue_.pop_front();
            return OrderStep::Next;
        }
        if (!start_build(cmd, registry, L)) {
            command_queue_.pop_front();
            return OrderStep::Next;
        }
    }
    // Phase 3: Progress the build
    if (!progress_build(dt, registry, L, ctx.pathfinding_grid, econ_eff)) {
        command_queue_.pop_front();
        return OrderStep::Next;
    }
    return OrderStep::Hold;
}

OrderStep Unit::order_build_in_place(UnitCommand& cmd, f64 dt, SimContext& ctx, f32 econ_eff) {
    auto& registry = ctx.registry;
    auto* L = ctx.L;
    if (build_target_id_ == 0) {
        // Factory: spawn immediately at own position
        if (build_blocked_by_lobby_rules(*this, cmd, ctx)) {
            command_queue_.pop_front();
            return OrderStep::Next;
        }
        if (!start_build(cmd, registry, L)) {
            command_queue_.pop_front();
            return OrderStep::Next;
        }
    }
    bool built = false;
    const u32 order_id = cmd.command_id;
    const u32 target = build_target_id_;
    const bool factory_build = cmd.type == CommandType::BuildFactory; // not an upgrade
    if (!progress_build(dt, registry, L, ctx.pathfinding_grid, econ_eff, &built)) {
        if (built && factory_build) hand_over_rally_orders(target, entity_id(), ctx);
        // Finishing ran scripts, which may have cleared the queue (and cmd
        // with it) or replaced it.
        if (command_queue_.empty() || &command_queue_.front() != &cmd ||
            command_queue_.front().command_id != order_id)
            return OrderStep::Next;
        // A factory repeating its queue sends a finished build order to the
        // back, and starts the next one next tick (Moho's command dispatch;
        // an order for n units is n orders here, so each goes back alone,
        // which builds them in Moho's order). A failed build still goes.
        if (built && repeat_queue_ && cmd.type == CommandType::BuildFactory) {
            auto finished = std::move(cmd); // cmd is the element pop_front destroys
            command_queue_.pop_front();
            command_queue_.push_back(std::move(finished));
            return OrderStep::Hold;
        }
        command_queue_.pop_front();
        return OrderStep::Next;
    }
    return OrderStep::Hold;
}

OrderStep Unit::order_patrol(UnitCommand& cmd, f64 dt, SimContext& ctx) {
    if (!navigator_.is_moving() || navigator_.goal().x != cmd.target_pos.x ||
        navigator_.goal().z != cmd.target_pos.z) {
        navigator_.set_goal(cmd.target_pos, ctx.pathfinder, position(), layer_, naval_draft_,
                            is_amphibious() || is_hover());
    }
    if (!nav_update(dt, ctx.terrain)) {
        // Reached patrol point — cycle to back of queue (moved out first:
        // cmd is the element pop_front destroys)
        auto finished = std::move(cmd);
        command_queue_.pop_front();
        command_queue_.push_back(std::move(finished));
        return OrderStep::Next;
    }
    return OrderStep::Hold;
}

OrderStep Unit::order_reclaim(UnitCommand& cmd, f64 dt, SimContext& ctx) {
    auto& registry = ctx.registry;
    auto* L = ctx.L;
    if (cmd.target_id == 0) {
        if (is_reclaiming()) stop_reclaiming();
        command_queue_.pop_front();
        return OrderStep::Next;
    }
    auto* target = registry.find(cmd.target_id);
    if (!target || target->destroyed() || !target->reclaimable()) {
        if (is_reclaiming()) stop_reclaiming();
        command_queue_.pop_front();
        return OrderStep::Next;
    }

    // Reach: the gap to its footprint within MaxBuildDistance (Moho's
    // CUnitReclaimTask). Out of reach, or standing on it, the unit
    // walks up to it, and reclaims from there or gives up.
    {
        const f32 extent = footprint_extent(*target);
        const f32 gap = work_gap(*this, target->position(), extent);
        const f32 rdx = target->position().x - position().x;
        const f32 rdz = target->position().z - position().z;
        const bool on_top = rdx * rdx + rdz * rdz < 1.0f;
        if (reclaim_target_id_ != cmd.target_id) {
            if (!cmd.approached && (gap > max_build_distance_ || on_top)) {
                if (effective_speed() <= 0) {
                    command_queue_.pop_front();
                    return OrderStep::Next;
                }
                cmd.approached = true;
                navigator_.set_goal(approach_point(*this, target->position(),
                                                   target->footprint_size_x() * 0.5f,
                                                   target->footprint_size_z() * 0.5f),
                                    ctx.pathfinder, position(), layer_, naval_draft_,
                                    is_amphibious() || is_hover());
            }
            if (cmd.approached && approach_update(dt, ctx)) return OrderStep::Hold;
            if (gap > max_build_distance_) {
                if (is_reclaiming()) stop_reclaiming();
                command_queue_.pop_front();
                return OrderStep::Next;
            }
        } else if (gap > max_build_distance_) {
            stop_reclaiming();
            command_queue_.pop_front();
            return OrderStep::Next;
        }
    }
    navigator_.abort_move();

    // Start reclaim if not already reclaiming this target
    if (reclaim_target_id_ != cmd.target_id) {
        if (is_reclaiming()) stop_reclaiming();

        const ReclaimCosts costs = reclaim_costs(L, *this, *target, static_cast<f64>(build_rate_));
        const f64 max_mass = costs.mass, max_energy = costs.energy;
        if (std::max(max_mass, max_energy) <= 0 || build_rate_ <= 0) {
            command_queue_.pop_front();
            return OrderStep::Next;
        }
        f64 reclaim_time = costs.time;
        if (reclaim_time <= 0) reclaim_time = 0.01;

        reclaim_target_id_ = cmd.target_id;
        reclaim_rate_ = static_cast<f32>(1.0 / reclaim_time);

        // Set production rates (resources gained by reclaiming)
        economy_.production_mass = max_mass * static_cast<f64>(reclaim_rate_);
        economy_.production_energy = max_energy * static_cast<f64>(reclaim_rate_);
        economy_.production_active = true;

        spdlog::info("Reclaim start: entity #{} reclaiming #{} "
                     "(mass={:.0f}, energy={:.0f}, time={:.1f}s)",
                     entity_id(), cmd.target_id, max_mass, max_energy, reclaim_time);
    }

    // Progress reclaim
    if (!progress_reclaim(dt, registry, L)) {
        command_queue_.pop_front();
        return OrderStep::Next;
    }
    return OrderStep::Hold;
}

OrderStep Unit::order_repair(UnitCommand& cmd, f64 dt, SimContext& ctx, f32 econ_eff) {
    auto& registry = ctx.registry;
    auto* L = ctx.L;
    if (cmd.target_id == 0) {
        if (is_repairing()) stop_repairing(L, registry);
        command_queue_.pop_front();
        return OrderStep::Next;
    }
    auto* rtarget = registry.find(cmd.target_id);
    if (!rtarget || rtarget->destroyed() || !rtarget->is_unit()) {
        if (is_repairing()) stop_repairing(L, registry);
        command_queue_.pop_front();
        return OrderStep::Next;
    }
    // Already at full health? Done.
    if (rtarget->health() >= rtarget->max_health()) {
        if (is_repairing()) stop_repairing(L, registry);
        command_queue_.pop_front();
        return OrderStep::Next;
    }

    // Reach: the gap to its skirt within MaxBuildDistance to begin,
    // twice that to go on (Moho's CUnitRepairTask). Out of reach, the
    // unit walks just clear of its skirt, and repairs from there or
    // gives up.
    {
        const auto& runit = static_cast<const Unit&>(*rtarget);
        const f32 gap = work_gap(*this, rtarget->position(), skirt_extent(runit));
        if (repair_target_id_ != cmd.target_id) {
            if (!cmd.approached && gap > max_build_distance_) {
                if (effective_speed() <= 0) {
                    command_queue_.pop_front();
                    return OrderStep::Next;
                }
                cmd.approached = true;
                navigator_.set_goal(approach_point(*this, rtarget->position(),
                                                   runit.skirt_size_x() * 0.5f + 1,
                                                   runit.skirt_size_z() * 0.5f + 1),
                                    ctx.pathfinder, position(), layer_, naval_draft_,
                                    is_amphibious() || is_hover());
            }
            if (cmd.approached && approach_update(dt, ctx)) return OrderStep::Hold;
            if (gap > max_build_distance_) {
                if (is_repairing()) stop_repairing(L, registry);
                command_queue_.pop_front();
                return OrderStep::Next;
            }
        } else if (gap > 2 * max_build_distance_) {
            stop_repairing(L, registry);
            command_queue_.pop_front();
            return OrderStep::Next;
        }
    }
    navigator_.abort_move();

    // Start repair if not already repairing this target
    if (repair_target_id_ != cmd.target_id) {
        if (is_repairing()) stop_repairing(L, registry);
        if (!start_repair(cmd, registry, L)) {
            command_queue_.pop_front();
            return OrderStep::Next;
        }
    }

    // Progress repair
    if (!progress_repair(dt, registry, L, econ_eff)) {
        command_queue_.pop_front();
        return OrderStep::Next;
    }
    return OrderStep::Hold;
}

OrderStep Unit::order_capture(UnitCommand& cmd, f64 dt, SimContext& ctx, f32 econ_eff) {
    auto& registry = ctx.registry;
    auto* L = ctx.L;
    if (cmd.target_id == 0) {
        if (is_capturing()) stop_capturing(L, registry, true);
        command_queue_.pop_front();
        return OrderStep::Next;
    }
    auto* ctarget = registry.find(cmd.target_id);
    if (!ctarget || ctarget->destroyed() || !ctarget->is_unit()) {
        if (is_capturing()) stop_capturing(L, registry, true);
        command_queue_.pop_front();
        return OrderStep::Next;
    }
    // Already same army (captured by someone else)
    if (ctarget->is_unit() && static_cast<Unit*>(ctarget)->army() == army()) {
        if (is_capturing()) stop_capturing(L, registry, false);
        command_queue_.pop_front();
        return OrderStep::Next;
    }

    // Reach: fixed, not MaxBuildDistance (Moho's CUnitCaptureTask). A
    // footprint gap over 5 sends the unit just clear of its target's
    // skirt; there it captures within 10, or gives up. A moving
    // target is lost beyond 10.
    {
        const auto& cunit = static_cast<const Unit&>(*ctarget);
        const f32 gap = work_gap(*this, ctarget->position(), footprint_extent(*ctarget));
        if (capture_target_id_ != cmd.target_id) {
            if (!cmd.approached && gap > kCaptureReach) {
                if (effective_speed() <= 0) {
                    command_queue_.pop_front();
                    return OrderStep::Next;
                }
                cmd.approached = true;
                navigator_.set_goal(approach_point(*this, ctarget->position(),
                                                   cunit.skirt_size_x() * 0.5f,
                                                   cunit.skirt_size_z() * 0.5f),
                                    ctx.pathfinder, position(), layer_, naval_draft_,
                                    is_amphibious() || is_hover());
            }
            if (cmd.approached && approach_update(dt, ctx)) return OrderStep::Hold;
            if (gap > kCaptureHold) {
                if (is_capturing()) stop_capturing(L, registry, true);
                command_queue_.pop_front();
                return OrderStep::Next;
            }
        } else if (gap > kCaptureHold && cunit.effective_speed() > 0) {
            stop_capturing(L, registry, true);
            command_queue_.pop_front();
            return OrderStep::Next;
        }
    }
    navigator_.abort_move();

    // Start capture if not already capturing this target
    if (capture_target_id_ != cmd.target_id) {
        if (is_capturing()) stop_capturing(L, registry, true);
        if (!start_capture(cmd, registry, L)) {
            command_queue_.pop_front();
            return OrderStep::Next;
        }
    }

    // Progress capture
    if (!progress_capture(dt, registry, L, econ_eff)) {
        command_queue_.pop_front();
        return OrderStep::Next;
    }
    return OrderStep::Hold;
}

void Unit::end_guard_build(EntityRegistry& registry, lua_State* L) {
    if (factory_assist_build_) {
        factory_assist_build_ = false;
        cancel_factory_build(registry, L);
    } else if (is_building()) {
        stop_assisting();
    }
}

namespace {
/// A factory's initial rally point: its blueprint's Economy.InitialRallyX/Z
/// (Moho's defaults 0 and 5), turned with the factory.
Vector3 initial_rally_point(const Unit& u, lua_State* L) {
    const Vector3 local{blueprint_economy_number(L, u.blueprint_id(), "InitialRallyX", 0.0f), 0.0f,
                        blueprint_economy_number(L, u.blueprint_id(), "InitialRallyZ", 5.0f)};
    const Vector3 offset = quat_rotate(u.orientation(), local);
    return {u.position().x + offset.x, u.position().y + offset.y, u.position().z + offset.z};
}
} // namespace

const std::vector<UnitCommand>& Unit::validated_rally_orders(lua_State* L, SimState* sim) {
    if (rally_orders_.empty() && keeps_rally_orders()) {
        UnitCommand rally;
        rally.type = CommandType::Move;
        rally.target_pos = initial_rally_point(*this, L);
        rally.command_id = sim ? sim->next_command_id() : 0;
        rally_orders_.push_back(rally);
    }
    return rally_orders_;
}

bool Unit::rally_point(lua_State* L, Vector3& out) const {
    if (!rally_orders_.empty()) {
        out = rally_orders_.front().target_pos;
        return true;
    }
    if (!keeps_rally_orders()) return false;
    out = initial_rally_point(*this, L);
    return true;
}

void Unit::hand_over_rally_orders(u32 built_id, u32 rally_id, SimContext& ctx) {
    if (is_mobile()) return; // a mobile factory's units take none (Moho)
    auto* built_entity = ctx.registry.find(built_id);
    auto* rally_entity = ctx.registry.find(rally_id);
    if (!built_entity || built_entity->destroyed() || !built_entity->is_unit()) return;
    if (!rally_entity || rally_entity->destroyed() || !rally_entity->is_unit()) return;
    auto& built = static_cast<Unit&>(*built_entity);
    auto& rally = static_cast<Unit&>(*rally_entity);
    // Aircraft and ships don't take a rally order to board a transport.
    const bool air_or_naval = built.has_category("AIR") || built.has_category("NAVAL");
    for (const UnitCommand& order : rally.validated_rally_orders(ctx.L, ctx.sim)) {
        if (order.type == CommandType::TransportLoad && air_or_naval) continue;
        built.command_queue_.push_back(order);
    }
}

OrderStep Unit::order_guard(UnitCommand& cmd, f64 dt, SimContext& ctx, f32 econ_eff) {
    auto& registry = ctx.registry;
    auto* L = ctx.L;
    if (cmd.target_id == 0) {
        end_guard_build(registry, L);
        command_queue_.pop_front();
        return OrderStep::Next;
    }
    auto* target = registry.find(cmd.target_id);
    if (!target || target->destroyed()) {
        end_guard_build(registry, L);
        command_queue_.pop_front();
        return OrderStep::Next;
    }
    if (!target->is_unit()) {
        end_guard_build(registry, L);
        command_queue_.pop_front();
        return OrderStep::Next;
    }
    auto* target_unit = static_cast<Unit*>(target);

    // A factory guarding a factory takes work from its queue (Moho's guard
    // task for an immobile FACTORY, TryDispatchFactoryOrUpgradeFromGuardQueues).
    // Each time it is free: the first of the guarded factory's build orders
    // it can build, other than the one being built, leaves that queue
    // (repeating, it goes to the back) and this factory builds the unit
    // itself (M206h). Moho also takes a repeating assister's pick of a
    // queue's only order, whose count it then restarts; without counts or
    // repeat queues here, that would only build a duplicate, so the head
    // is never taken.
    if (!is_mobile() && has_category("FACTORY") && target_unit->has_category("FACTORY")) {
        if (factory_assist_build_) {
            const u32 built_id = build_target_id_;
            const u32 guarded_id = cmd.target_id; // cmd may go with the scripts' changes
            bool built = false;
            if (!progress_build(dt, registry, L, ctx.pathfinding_grid, econ_eff, &built)) {
                factory_assist_build_ = false; // built (or failed): free again
                // Built for the guarded factory, the unit takes its rally
                // orders: Moho's guard task hands the build task that
                // factory as the one whose orders the unit takes.
                if (built) hand_over_rally_orders(built_id, guarded_id, ctx);
            }
            return OrderStep::Hold;
        }
        auto& queue = target_unit->command_queue_;
        for (size_t i = 0; i < queue.size() && L; ++i) {
            if (queue[i].type != CommandType::BuildFactory) continue;
            if (i == 0) continue; // the guarded factory's own build
            if (!blueprint_can_build(L, blueprint_id(), queue[i].blueprint_id)) continue;
            UnitCommand build;
            build.type = CommandType::BuildFactory;
            build.blueprint_id = queue[i].blueprint_id;
            const UnitCommand taken = queue[i];
            queue.erase(queue.begin() + static_cast<std::ptrdiff_t>(i));
            // Taken even if the lobby's rules forbid it, as Moho's build task
            // fails after the take; such an order is dropped, repeating or
            // not, as the guarded factory drops it when it comes to it.
            if (build_blocked_by_lobby_rules(*this, build, ctx)) break;
            if (repeat_queue_) queue.push_back(taken);
            factory_assist_build_ = start_build(build, registry, L);
            break;
        }
        return OrderStep::Hold;
    }

    // Help only within reach of the work (M206e): Moho's guard hands
    // it to a repair or reclaim task. A build, a silo or a repair
    // measures the gap to that unit's skirt (MaxBuildDistance to
    // begin, twice that to go on); a reclaim, to its footprint. Out
    // of reach, the unit walks just clear of the work, helping with
    // nothing meanwhile.
    const auto within_reach = [&](const Entity& work, bool skirt, bool helping) {
        f32 extent = footprint_extent(work);
        f32 half_x = work.footprint_size_x() * 0.5f;
        f32 half_z = work.footprint_size_z() * 0.5f;
        if (skirt && work.is_unit()) {
            const auto& wu = static_cast<const Unit&>(work);
            extent = skirt_extent(wu);
            half_x = wu.skirt_size_x() * 0.5f + 1;
            half_z = wu.skirt_size_z() * 0.5f + 1;
        }
        const f32 limit = helping && skirt ? 2 * max_build_distance_ : max_build_distance_;
        if (work_gap(*this, work.position(), extent) <= limit) {
            navigator_.abort_move();
            return true;
        }
        if (effective_speed() > 0) {
            const Vector3 goal = approach_point(*this, work.position(), half_x, half_z);
            const Vector3 heading = navigator_.goal();
            if (!navigator_.is_moving() || std::abs(heading.x - goal.x) > 1.0f ||
                std::abs(heading.z - goal.z) > 1.0f) {
                navigator_.set_goal(goal, ctx.pathfinder, position(), layer_, naval_draft_,
                                    is_amphibious() || is_hover());
            }
            nav_update(dt, ctx.terrain);
        }
        return false;
    };
    bool working = false;
    // Who helps (Moho's guard dispatches repair and reclaim tasks): a
    // unit that repairs helps builds, silos and repairs; one that
    // reclaims, reclaims. Others (a tank guarding an engineer, a
    // factory assisting a factory) only follow.
    const bool repairs = has_category("REPAIR") && build_rate_ > 0;
    const bool reclaims = has_category("RECLAIM") && build_rate_ > 0;

    // Assist: if target is building, contribute build power
    const Entity* guarded_build = repairs && target_unit->is_building()
                                      ? registry.find(target_unit->build_target_id())
                                      : nullptr;
    const Entity* guarded_reclaim = reclaims && target_unit->is_reclaiming()
                                        ? registry.find(target_unit->reclaim_target_id())
                                        : nullptr;
    if (guarded_build && !guarded_build->destroyed()) {
        working = true;
        u32 target_build_id = target_unit->build_target_id();
        if (!within_reach(*guarded_build, true, build_target_id_ == target_build_id)) {
            if (is_building()) stop_assisting();
        } else {
            if (build_target_id_ != target_build_id) {
                // Switch to new assist target
                if (is_building()) stop_assisting();

                build_target_id_ = target_build_id;
                build_time_ = target_unit->build_time();
                build_cost_mass_ = target_unit->build_cost_mass();
                build_cost_energy_ = target_unit->build_cost_energy();
                work_progress_ = guarded_build->fraction_complete();

                if (build_time_ > 0 && build_rate_ > 0) {
                    economy_.consumption_mass =
                        build_cost_mass_ * static_cast<f64>(build_rate_) / build_time_;
                    economy_.consumption_energy =
                        build_cost_energy_ * static_cast<f64>(build_rate_) / build_time_;
                    economy_.consumption_active = true;
                }

                spdlog::info("Guard assist: entity #{} assisting #{} "
                             "building target #{}",
                             entity_id(), cmd.target_id, target_build_id);
            }

            // Progress the build with our own build rate
            if (build_target_id_ != 0) {
                if (!progress_build_assist(dt, registry, econ_eff)) {
                    stop_assisting();
                }
            }
        }
    } else if (guarded_reclaim && !guarded_reclaim->destroyed() && guarded_reclaim->reclaimable()) {
        // Assist reclaim: contribute reclaim power
        working = true;
        if (is_building()) stop_assisting();
        u32 target_reclaim_id = target_unit->reclaim_target_id();
        if (!within_reach(*guarded_reclaim, false, false)) {
            if (is_reclaiming()) stop_reclaiming();
        } else {
            if (reclaim_target_id_ != target_reclaim_id) {
                // Switch to new reclaim target
                if (is_reclaiming()) stop_reclaiming();

                reclaim_target_id_ = target_reclaim_id;

                // Compute own reclaim rate based on own build_rate
                // (assister contributes speed but NOT duplicate resources
                //  — only the primary reclaimer sets production rates)
                const ReclaimCosts costs =
                    reclaim_costs(L, *this, *guarded_reclaim, static_cast<f64>(build_rate_));
                if (std::max(costs.mass, costs.energy) > 0 && build_rate_ > 0) {
                    f64 reclaim_time = costs.time;
                    if (reclaim_time <= 0) reclaim_time = 0.01;
                    reclaim_rate_ = static_cast<f32>(1.0 / reclaim_time);
                } else {
                    reclaim_rate_ = 0;
                }

                spdlog::info("Guard reclaim assist: entity #{} "
                             "assisting #{} reclaiming #{}",
                             entity_id(), cmd.target_id, target_reclaim_id);
            }

            if (reclaim_target_id_ != 0) {
                if (!progress_reclaim_assist(dt, registry)) {
                    stop_reclaiming();
                }
            }
        }
    } else if (repairs && target_unit->silo_building() && !target_unit->is_paused()) {
        // Assist a silo's missile: this unit's build power on it, at
        // its share of the cost (retail's UpdateConsumptionValues
        // for a SiloBuildingAmmo focus). A paused silo's helpers
        // wait, paying nothing; so do helpers out of reach.
        working = true;
        if (is_building()) stop_assisting();
        if (is_reclaiming()) stop_reclaiming();
        if (within_reach(*target_unit, true, false)) {
            const SiloBuild& missile = target_unit->silo_build();
            const f64 per_second = static_cast<f64>(build_rate_) / missile.build_time;
            economy_.consumption_energy = missile.energy * per_second;
            economy_.consumption_mass = missile.mass * per_second;
            economy_.consumption_active = true;
            assisting_silo_ = true;
            target_unit->assist_silo_build(build_rate_, dt, econ_eff);
        }
    } else {
        // Target not building/reclaiming — stop if we were
        if (is_building()) stop_assisting();
        if (is_reclaiming()) stop_reclaiming();

        // Auto-repair: if target is damaged and we have build_rate
        if (repairs && target_unit->health() < target_unit->max_health()) {
            working = true;
            if (!within_reach(*target_unit, true, repair_target_id_ == cmd.target_id)) {
                if (is_repairing()) stop_repairing(L, registry);
            } else {
                if (repair_target_id_ != cmd.target_id) {
                    if (is_repairing()) stop_repairing(L, registry);
                    UnitCommand repair_cmd;
                    repair_cmd.type = CommandType::Repair;
                    repair_cmd.target_id = cmd.target_id;
                    repair_cmd.target_pos = target->position();
                    start_repair(repair_cmd, registry, L);
                }
                if (repair_target_id_ != 0) {
                    progress_repair(dt, registry, L, econ_eff);
                }
            }
        } else {
            if (is_repairing()) stop_repairing(L, registry);
        }
    }

    // Otherwise follow: an engineer stays put within twice its
    // MaxBuildDistance of what it guards (Moho's CUnitGuardTask),
    // other units within 10; past that, back just clear of it.
    if (!working && !destroyed() && in_registry()) {
        const f32 follow = has_category("ENGINEER") ? 2 * max_build_distance_ : 10.0f;
        const f32 gdx = target->position().x - position().x;
        const f32 gdz = target->position().z - position().z;
        if (gdx * gdx + gdz * gdz > follow * follow) {
            const Vector3 goal =
                approach_point(*this, target->position(), target_unit->skirt_size_x() * 0.5f,
                               target_unit->skirt_size_z() * 0.5f);
            const Vector3 heading = navigator_.goal();
            if (!navigator_.is_moving() || std::abs(heading.x - goal.x) > 1.0f ||
                std::abs(heading.z - goal.z) > 1.0f) {
                navigator_.set_goal(goal, ctx.pathfinder, position(), layer_, naval_draft_,
                                    is_amphibious() || is_hover());
            }
            nav_update(dt, ctx.terrain);
        } else if (navigator_.is_moving()) {
            navigator_.abort_move();
        }
    }

    return OrderStep::Hold; // Guard is persistent
}

OrderStep Unit::order_dive(lua_State* L) {
    // Moho's SetNewTargetLayer (M206o): a sub at or making for the surface
    // dives, one under or making for it surfaces; its layer changes when it
    // gets there (tick_dive). Scripts hear it (OnMotionVertEventChange), and
    // may clear or replace the queue: the order goes only if still the head.
    // Only a submarine dives: any other unit ignores the order (Moho's UI
    // offers it to units with RULEUCC_Dive, but a script may give it to any).
    const UnitCommand* head = &command_queue_.front();
    const u32 order_id = head->command_id;
    const bool under = layer_ == "Sub" || layer_ == "Seabed";
    if (motion_type_ == "RULEUMT_SurfacingSub") {
        if (vert_motion_ == VertMotion::Down || (vert_motion_ == VertMotion::None && under))
            start_surfacing(L);
        else if (vert_motion_ == VertMotion::Up || layer_ == "Water") start_dive(L);
    }
    if (destroyed() || !in_registry()) return OrderStep::Gone;
    if (!command_queue_.empty() && &command_queue_.front() == head &&
        command_queue_.front().command_id == order_id)
        command_queue_.pop_front();
    return OrderStep::Next;
}

OrderStep Unit::order_enhance(UnitCommand& cmd, f64 dt, SimContext& ctx, f32 econ_eff) {
    auto* L = ctx.L;
    if (!enhancing_) {
        auto* store = ctx.sim ? ctx.sim->blueprint_store() : nullptr;
        if (!start_enhance(cmd, L, store)) {
            command_queue_.pop_front();
            return OrderStep::Next;
        }
    }
    if (!progress_enhance(dt, L, econ_eff)) {
        command_queue_.pop_front();
        return OrderStep::Next;
    }
    return OrderStep::Hold;
}

OrderStep Unit::order_transport_load(UnitCommand& cmd, f64 dt, SimContext& ctx) {
    auto& registry = ctx.registry;
    auto* L = ctx.L;
    // Cargo unit loads into transport (target_id = transport entity)
    if (cmd.target_id == 0 || transport_id_ != 0) {
        set_unit_state("TransportLoading", false);
        command_queue_.pop_front();
        return OrderStep::Next;
    }
    auto* target = registry.find(cmd.target_id);
    if (!target || target->destroyed() || !target->is_unit()) {
        set_unit_state("TransportLoading", false);
        command_queue_.pop_front();
        return OrderStep::Next;
    }
    auto* transport = static_cast<Unit*>(target);

    // Check capacity before moving
    if (transport->transport_capacity() > 0 &&
        static_cast<i32>(transport->cargo_ids().size()) >= transport->transport_capacity()) {
        set_unit_state("TransportLoading", false);
        command_queue_.pop_front();
        return OrderStep::Next;
    }

    // Move toward transport
    set_unit_state("TransportLoading", true);
    constexpr f32 load_range = 5.0f;
    f32 ldx = transport->position().x - position().x;
    f32 ldz = transport->position().z - position().z;
    f32 ldist2 = ldx * ldx + ldz * ldz;
    if (ldist2 > load_range * load_range) {
        if (!navigator_.is_moving() || navigator_.goal().x != transport->position().x ||
            navigator_.goal().z != transport->position().z) {
            navigator_.set_goal(transport->position(), ctx.pathfinder, position(), layer_,
                                naval_draft_, is_amphibious() || is_hover());
        }
        navigator_.update(*this, effective_speed(), dt, ctx.terrain);
        return OrderStep::Hold;
    }
    navigator_.abort_move();

    // Attach to transport
    attach_to_transport(transport, registry, L);
    set_unit_state("TransportLoading", false);
    command_queue_.pop_front();
    return OrderStep::Next;
}

OrderStep Unit::order_transport_unload(UnitCommand& cmd, f64 dt, SimContext& ctx) {
    auto& registry = ctx.registry;
    auto* L = ctx.L;
    // Transport drops its cargo at target position: the order's
    // (IssueTransportUnloadSpecific) or all of it. With none of it aboard,
    // the order ends.
    const bool any_aboard =
        cmd.unload_ids.empty()
            ? !cargo_ids_.empty()
            : std::any_of(cmd.unload_ids.begin(), cmd.unload_ids.end(), [&](u32 id) {
                  return std::find(cargo_ids_.begin(), cargo_ids_.end(), id) != cargo_ids_.end();
              });
    if (!any_aboard) {
        set_unit_state("TransportUnloading", false);
        command_queue_.pop_front();
        return OrderStep::Next;
    }

    // Move to drop position
    constexpr f32 unload_range = 5.0f;
    f32 udx = cmd.target_pos.x - position().x;
    f32 udz = cmd.target_pos.z - position().z;
    f32 udist2 = udx * udx + udz * udz;
    if (udist2 > unload_range * unload_range) {
        set_unit_state("TransportUnloading", true);
        if (!navigator_.is_moving() || navigator_.goal().x != cmd.target_pos.x ||
            navigator_.goal().z != cmd.target_pos.z) {
            navigator_.set_goal(cmd.target_pos, ctx.pathfinder, position(), layer_, naval_draft_,
                                is_amphibious() || is_hover());
        }
        navigator_.update(*this, effective_speed(), dt, ctx.terrain);
        return OrderStep::Hold;
    }
    navigator_.abort_move();

    if (cmd.unload_ids.empty()) detach_all_cargo(registry, L);
    else detach_cargo(cmd.unload_ids, registry, L);
    set_unit_state("TransportUnloading", false);
    command_queue_.pop_front();
    return OrderStep::Next;
}

OrderStep Unit::order_launch(UnitCommand& cmd, f64 dt, SimContext& ctx) {
    auto& registry = ctx.registry;
    auto* L = ctx.L;
    // A launch: its weapon takes the order's target (see
    // Weapon::take_order_target) and fires when it has a missile,
    // and the weapon's script spends it. The order waits in range
    // until then, and ends when the missile leaves. An OverCharge is
    // the same with the unit's OverCharge weapon, at a unit: in
    // range, the weapon is switched on (its script's OnEnableWeapon)
    // and its script fires it, drawing its energy.
    const bool overcharge = cmd.type == CommandType::Overcharge;
    Weapon* weapon =
        overcharge ? overcharge_weapon() : launch_weapon(cmd.type == CommandType::Nuke);
    if (!weapon || cmd.launched || (overcharge && cmd.target_id == 0)) {
        command_queue_.pop_front();
        return OrderStep::Next;
    }
    Vector3 at = cmd.target_pos;
    if (cmd.target_id != 0) {
        const Entity* target = registry.find(cmd.target_id);
        if (!target || target->destroyed()) {
            command_queue_.pop_front();
            return OrderStep::Next;
        }
        at = target->position();
    }
    const f32 dx = at.x - position().x;
    const f32 dz = at.z - position().z;
    const f32 dist2 = dx * dx + dz * dz;
    cmd.in_band = dist2 >= weapon->min_range * weapon->min_range &&
                  dist2 <= weapon->max_range * weapon->max_range;
    // Too close: a launcher that can move backs off along the line
    // from its target through itself, to 1.1 x its minimum range
    // (Moho's CUnitFireAtTask); one that can't gives up.
    if (dist2 < weapon->min_range * weapon->min_range) {
        if (immobile_ || effective_speed() <= 0) {
            command_queue_.pop_front();
            return OrderStep::Next;
        }
        if (!navigator_.is_moving()) {
            const f32 dist = std::sqrt(dist2);
            const f32 ox = dist > 1e-3f ? -dx / dist : 0.0f;
            const f32 oz = dist > 1e-3f ? -dz / dist : -1.0f;
            const f32 back = weapon->min_range * 1.1f;
            navigator_.set_goal({at.x + ox * back, at.y, at.z + oz * back}, ctx.pathfinder,
                                position(), layer_, naval_draft_, is_amphibious() || is_hover());
        }
        nav_update(dt, ctx.terrain);
        return OrderStep::Hold;
    }
    if (dist2 > weapon->max_range * weapon->max_range) {
        // Out of range: a launcher that can move goes closer; a
        // silo can't fire at it.
        if (immobile_ || effective_speed() <= 0) {
            command_queue_.pop_front();
            return OrderStep::Next;
        }
        if (!navigator_.is_moving() || navigator_.goal().x != at.x || navigator_.goal().z != at.z) {
            navigator_.set_goal(at, ctx.pathfinder, position(), layer_, naval_draft_,
                                is_amphibious() || is_hover());
        }
        nav_update(dt, ctx.terrain);
    } else {
        navigator_.abort_move();
        if (overcharge && !cmd.started) {
            cmd.started = true;
            overcharge_armed_ = true;
            weapon->call_script(L, "OnEnableWeapon");
            if (destroyed() || !in_registry()) return OrderStep::Gone;
        } else if (!overcharge) {
            // With no missile, the order asks the silo for one when
            // it is neither building one of the kind nor full (Moho's
            // fire-at task); a silo that can't build one ends it.
            const bool nuke = cmd.type == CommandType::Nuke;
            if (silo_ammo(nuke) <= 0 && silo_build_count(nuke) == 0 &&
                silo_ammo(nuke) < silo_max_storage(nuke)) {
                if (!silo_weapon(nuke)) {
                    command_queue_.pop_front();
                    return OrderStep::Next;
                }
                order_silo_build(nuke);
            }
            // A nuke's unit hears OnNukeLaunched as it fires.
            if (nuke && !cmd.started && silo_ammo(true) > 0) {
                cmd.started = true;
                call_lua_method(L, "OnNukeLaunched");
                if (destroyed() || !in_registry()) return OrderStep::Gone;
            }
        }
    }
    return OrderStep::Hold;
}

OrderStep Unit::order_sacrifice(UnitCommand& cmd, f64 dt, SimContext& ctx) {
    auto& registry = ctx.registry;
    auto* L = ctx.L;
    // Move to target, then sacrifice (transfer mass value, kill self)
    if (cmd.target_id == 0) {
        command_queue_.pop_front();
        return OrderStep::Next;
    }
    auto* target = registry.find(cmd.target_id);
    if (!target || target->destroyed() || !target->is_unit()) {
        call_lua_method(L, "OnStopSacrifice");
        command_queue_.pop_front();
        return OrderStep::Next;
    }
    auto* target_unit = static_cast<Unit*>(target);
    // Move into range
    constexpr f32 sacrifice_range = 5.0f;
    f32 sdx = target->position().x - position().x;
    f32 sdz = target->position().z - position().z;
    f32 sdist2 = sdx * sdx + sdz * sdz;
    if (sdist2 > sacrifice_range * sacrifice_range) {
        if (!navigator_.is_moving()) {
            navigator_.set_goal(target->position(), ctx.pathfinder, position(), layer_,
                                naval_draft_, is_amphibious() || is_hover());
        }
        navigator_.update(*this, effective_speed(), dt, ctx.terrain);
        // Fire OnStartSacrifice on first tick
        if (!has_unit_state("Sacrificing")) {
            set_unit_state("Sacrificing", true);
            call_lua_method_with_entity(L, "OnStartSacrifice", target);
        }
        return OrderStep::Hold;
    }
    navigator_.abort_move();
    // Transfer build progress: sacrifice unit's mass value → target build
    if (target_unit->is_being_built()) {
        f32 mass_value = static_cast<f32>(build_cost_mass_);
        f32 progress_add = mass_value / static_cast<f32>(target_unit->build_cost_mass() > 0
                                                             ? target_unit->build_cost_mass()
                                                             : 1.0);
        f32 new_progress = std::min(1.0f, target_unit->work_progress() + progress_add);
        target_unit->set_work_progress(new_progress);
    }
    // Fire OnStopSacrifice then kill self
    call_lua_method_with_entity(L, "OnStopSacrifice", target);
    set_unit_state("Sacrificing", false);
    set_health(0);
    mark_destroyed();
    {
        u32 eid = entity_id();
        registry.unregister_entity(eid);
    }
    return OrderStep::Gone; // unit is dead, stop processing
}

OrderStep Unit::order_teleport(UnitCommand& cmd, lua_State* L) {
    // Moho hands the teleport to the script: OnTeleportUnit(teleporter,
    // location, orientation) charges it (an economy event sized from
    // the blueprint's cost) and Warp()s the unit when it completes.
    // The order waits for the warp, so what is queued behind it waits
    // too; removed before, the script hears OnFailedTeleport. Only a
    // unit without the handler moves at once.
    if (!cmd.started) {
        cmd.started = true;
        const Vector3 to = cmd.target_pos;
        teleport_snap_ = snap_serial();
        teleporting_ = true;
        if (!call_on_teleport_unit(L, to)) {
            teleporting_ = false;
            set_position(to);
            note_snap();
            command_queue_.pop_front();
            return OrderStep::Next;
        }
        if (destroyed() || !in_registry()) return OrderStep::Gone;
        return OrderStep::Hold;
    }
    if (snap_serial() != teleport_snap_) {
        teleporting_ = false; // warped
        command_queue_.pop_front();
        return OrderStep::Next;
    }
    return OrderStep::Hold;
}

OrderStep Unit::order_ferry(UnitCommand& cmd, f64 dt, SimContext& ctx) {
    auto& registry = ctx.registry;
    auto* L = ctx.L;
    // A ferry route (Moho's CUnitFerryTask): the leading Ferry orders,
    // which stay queued. The first is where it loads, at a beacon;
    // the last where it unloads; those between are waypoints, flown
    // out and back.
    const u32 beacon_id = ferry_beacon(ctx, cmd);
    if (destroyed() || !in_registry()) return OrderStep::Gone;
    if (beacon_id == 0) {
        command_queue_.pop_front();
        return OrderStep::Next;
    }
    // A new route (Moho makes a new task for each order) starts from
    // loading, even one given in the tick its last was cleared.
    if (beacon_id != ferry_for_) {
        ferry_phase_ = FerryPhase::Load;
        ferry_index_ = 0;
        ferry_leg_set_ = false;
        ferry_for_ = beacon_id;
    }
    set_unit_state("Ferrying", true);
    const Vector3 home = registry.find(beacon_id)->position();
    i32 route = 0;
    while (route < static_cast<i32>(command_queue_.size()) &&
           command_queue_[static_cast<size_t>(route)].type == CommandType::Ferry)
        ++route;
    const auto point = [&](i32 i) {
        return command_queue_[static_cast<size_t>(std::clamp(i, 0, route - 1))].target_pos;
    };

    if (ferry_phase_ == FerryPhase::Load) {
        // The army's land units waiting at the beacon, as many as it
        // has room for, board it; with none left and cargo aboard, it
        // sets out. Meanwhile it keeps to the beacon.
        i32 boarding = 0;
        registry.for_each_unit([&](const Entity& e) {
            if (e.destroyed() || !e.is_unit() || e.army() != army()) return;
            const auto& u = static_cast<const Unit&>(e);
            if (u.is_dying() || u.transport_id() != 0 || u.command_queue().empty()) return;
            const UnitCommand& wait = u.command_queue().front();
            if (wait.type == CommandType::WaitForFerry && wait.assigned_id == entity_id())
                ++boarding;
        });
        // (A capacity of 0 is unknown, and not a limit, as for a load
        // order: retail transports count attach points, not read here.)
        i32 room = transport_capacity() > 0
                       ? transport_capacity() - static_cast<i32>(cargo_ids_.size()) - boarding
                       : std::numeric_limits<i32>::max();
        if (room > 0) {
            registry.for_each_unit([&](Entity& e) {
                if (room <= 0 || e.destroyed() || !e.is_unit() || e.army() != army()) return;
                auto& u = static_cast<Unit&>(e);
                if (u.is_dying() || u.transport_id() != 0 || u.command_queue().empty() ||
                    !u.has_category("LAND") || !u.has_unit_state("WaitForFerry"))
                    return;
                if (u.has_category("COMMAND") && !has_category("CANTRANSPORTCOMMANDER")) return;
                const UnitCommand& wait = u.command_queue().front();
                if (wait.type != CommandType::WaitForFerry || wait.target_id != beacon_id ||
                    wait.assigned_id != 0)
                    return;
                u.board_ferry(entity_id());
                --room;
                ++boarding;
            });
        }
        if (boarding == 0 && !cargo_ids_.empty()) {
            ferry_phase_ = FerryPhase::Out;
            ferry_index_ = 1;
            ferry_leg_set_ = false;
        } else {
            ferry_fly(dt, ctx, home);
            return OrderStep::Hold;
        }
    }
    if (ferry_phase_ == FerryPhase::Out) {
        // Out along the waypoints, then to the drop-off.
        if (ferry_index_ < route - 1) {
            if (!ferry_fly(dt, ctx, point(ferry_index_))) ++ferry_index_;
            return OrderStep::Hold;
        }
        ferry_phase_ = FerryPhase::Unload;
    }
    if (ferry_phase_ == FerryPhase::Unload) {
        constexpr f32 unload_range = 5.0f;
        const Vector3 drop = point(route - 1);
        const f32 udx = drop.x - position().x;
        const f32 udz = drop.z - position().z;
        if (udx * udx + udz * udz > unload_range * unload_range && !cargo_ids_.empty()) {
            ferry_fly(dt, ctx, drop);
            return OrderStep::Hold;
        }
        navigator_.abort_move();
        ferry_leg_set_ = false;
        detach_all_cargo(registry, L);
        if (destroyed() || !in_registry()) return OrderStep::Gone;
        ferry_phase_ = FerryPhase::Back;
        ferry_index_ = route - 1;
        return OrderStep::Hold;
    }
    // Back along the waypoints, then to the beacon to load again.
    if (ferry_index_ > 1) {
        if (!ferry_fly(dt, ctx, point(ferry_index_ - 1))) --ferry_index_;
        return OrderStep::Hold;
    }
    if (!ferry_fly(dt, ctx, home)) {
        ferry_phase_ = FerryPhase::Load;
        ferry_index_ = 0;
    }
    return OrderStep::Hold;
}

OrderStep Unit::order_wait_for_ferry(UnitCommand& cmd, f64 dt, SimContext& ctx) {
    auto& registry = ctx.registry;
    auto* L = ctx.L;
    // Waiting for a ferry (Moho's CUnitWaitForFerryTask): walk to the
    // beacon and wait there. A ferry with room takes the unit
    // (assigned_id), and it boards as for a load order.
    const Entity* beacon = registry.find(cmd.target_id);
    if (!beacon || beacon->destroyed()) {
        command_queue_.pop_front();
        return OrderStep::Next;
    }
    if (cmd.assigned_id != 0) {
        Entity* e = registry.find(cmd.assigned_id);
        auto* ferry = e && !e->destroyed() && e->is_unit() ? static_cast<Unit*>(e) : nullptr;
        const bool ferrying_here = ferry && !ferry->is_dying() && !ferry->command_queue().empty() &&
                                   ferry->command_queue().front().type == CommandType::Ferry &&
                                   ferry->command_queue().front().beacon_id == cmd.target_id;
        if (!ferrying_here) {
            cmd.assigned_id = 0;
        } else {
            constexpr f32 load_range = 5.0f;
            const f32 ldx = ferry->position().x - position().x;
            const f32 ldz = ferry->position().z - position().z;
            if (ldx * ldx + ldz * ldz <= load_range * load_range) {
                navigator_.abort_move();
                set_unit_state("WaitForFerry", false);
                attach_to_transport(ferry, registry, L);
                command_queue_.pop_front();
                return OrderStep::Next;
            }
            const Vector3 heading = navigator_.goal();
            if (!navigator_.is_moving() || std::abs(heading.x - ferry->position().x) > 1.0f ||
                std::abs(heading.z - ferry->position().z) > 1.0f) {
                navigator_.set_goal(ferry->position(), ctx.pathfinder, position(), layer_,
                                    naval_draft_, is_amphibious() || is_hover());
            }
            nav_update(dt, ctx.terrain);
            return OrderStep::Hold;
        }
    }
    constexpr f32 wait_range = 4.0f;
    const f32 bdx = beacon->position().x - position().x;
    const f32 bdz = beacon->position().z - position().z;
    if (has_unit_state("WaitForFerry") || bdx * bdx + bdz * bdz <= wait_range * wait_range) {
        navigator_.abort_move();
        set_unit_state("WaitForFerry", true);
        return OrderStep::Hold;
    }
    if (!navigator_.is_moving()) {
        navigator_.set_goal(beacon->position(), ctx.pathfinder, position(), layer_, naval_draft_,
                            is_amphibious() || is_hover());
    }
    if (!nav_update(dt, ctx.terrain) && effective_speed() > 0 &&
        navigator_.status() == Navigator::Status::Idle) {
        // As near as it gets.
        set_unit_state("WaitForFerry", true);
    }
    return OrderStep::Hold;
}

} // namespace osc::sim
