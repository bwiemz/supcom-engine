// A unit's orders (M193): one handler per kind, run from the head of the
// queue each tick (Unit::tick_orders). A handler says whether its order goes
// on (OrderStep).

#include "sim/unit.hpp"
#include "core/dmath.hpp"
#include "sim/air_combat.hpp"
#include "sim/bone_data.hpp"
#include "core/test_status.hpp"
#include "sim/blueprint_categories.hpp"
#include "sim/build_site_props.hpp"
#include "sim/entity_registry.hpp"
#include "sim/sim_state.hpp"
#include "sim/work_range.hpp"
#include "map/terrain.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>
#include <spdlog/spdlog.h>

extern "C" {
#include <lua.h>
}

namespace osc::sim {

namespace {
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

/// Ticks a build or repair waits at its army's unit cap, or paused, before
/// trying again: Moho's tasks return 10 there (a task waits 9 ticks and runs
/// on the 10th), the brain hearing OnUnitCapLimitReached at each try at the cap.
constexpr i32 kTaskRetryTicks = 10;

bool build_blocked_by_lobby_rules(const Unit& builder, const UnitCommand& cmd,
                                  const SimContext& ctx) {
    if (!ctx.sim) return false;

    auto* brain = ctx.sim->get_army(builder.army());
    if (!brain) return false;

    // (The unit cap is no rule of the order's: the unit's making checks it,
    // and the builder waits it out -- start_build, kTaskRetryTicks.)
    if (brain->is_build_restricted(cmd.blueprint_id)) {
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

/// Moho's Unit::Materialize.
void materialize(Unit& target, f32 step) {
    if (target.is_being_built()) {
        const f32 health_ratio =
            target.max_health() > 0.0f ? target.health() / target.max_health() : 0.0f;
        const f32 progressed = std::clamp(target.fraction_complete() + step, 0.0f, 1.0f);
        target.set_fraction_complete(std::max(health_ratio, progressed));
    }
    target.set_health(std::min(target.max_health(), target.health() + target.max_health() * step));
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

// faf-re CAiPathNavigator's UpdateWaterFavorAltFootprintMode: the alt
// footprint while it, and every FAVORSWATER unit given its order, are on
// water and bound for water.
void Unit::set_path_goal(const Vector3& goal, const SimContext& ctx) {
    static const CategoryName kFavorsWater{"FAVORSWATER"};
    if (has_category(kFavorsWater) && ctx.terrain) {
        const map::Terrain& t = *ctx.terrain;
        const f32 water = t.has_water() ? t.water_elevation() : -10000.0f;
        const auto on_water = [&](const Vector3& p) {
            return !(t.get_terrain_height(p.x, p.z) > water);
        };
        const auto destination = [&](const UnitCommand& c) {
            const Entity* target = c.target_id != 0 ? ctx.registry.find(c.target_id) : nullptr;
            return target ? target->position() : c.target_pos;
        };
        using_alt_footprint_ = false;
        if (command_queue_.empty()) {
            using_alt_footprint_ = on_water(position()) && on_water(goal);
        } else {
            const UnitCommand& order = command_queue_.front();
            const auto keeps_to_water = [&](const Unit& u) {
                const UnitCommand* own = nullptr;
                for (const UnitCommand& c : u.command_queue_) {
                    if (c.command_id == order.command_id) {
                        own = &c;
                        break;
                    }
                }
                if (!own) {
                    return true;
                }
                const UnitCommand& current = u.command_queue_.front();
                return on_water(u.position()) && on_water(destination(*own)) &&
                       (current.command_id == order.command_id || on_water(destination(current)));
            };
            bool all = keeps_to_water(*this);
            if (all && order.command_id != 0) {
                ctx.registry.for_each_unit([&](const Entity& e) {
                    const auto& u = static_cast<const Unit&>(e);
                    if (all && &u != this && !u.dying_ && !u.destroyed() &&
                        u.has_category(kFavorsWater)) {
                        all = keeps_to_water(u);
                    }
                });
            }
            using_alt_footprint_ = all;
        }
    }
    if (!uses_alt_footprint()) {
        navigator_.set_goal(goal, ctx.pathfinder, position(), layer_, naval_draft_,
                            is_amphibious() || is_hover());
        return;
    }
    const blueprints::Footprint& fp = footprint();
    const bool afloat =
        (fp.caps & (blueprints::occupancy::kLand | blueprints::occupancy::kSeabed)) == 0;
    navigator_.set_goal(goal, ctx.pathfinder, position(), afloat ? std::string("Water") : layer_,
                        std::max(naval_draft_, fp.min_water_depth), false);
}

bool Unit::ferry_fly(f64 dt, SimContext& ctx, const Vector3& to, bool through) {
    navigator_.set_speed_through_goal(through);
    const Vector3 heading = navigator_.goal();
    if (!ferry_leg_set_ || std::abs(heading.x - to.x) > 1.0f || std::abs(heading.z - to.z) > 1.0f ||
        navigator_.status() == Navigator::Status::WaitingForPath) {
        set_path_goal(to, ctx);
        ferry_leg_set_ = true;
    }
    const bool going = nav_update(dt, ctx.terrain);
    if (!going) ferry_leg_set_ = false;
    return going;
}

bool Unit::approach_update(f64 dt, SimContext& ctx) {
    if (navigator_.status() == Navigator::Status::WaitingForPath) {
        const Vector3 goal = navigator_.goal();
        set_path_goal(goal, ctx);
    }
    return nav_update(dt, ctx.terrain);
}

bool Unit::tick_orders(f64 dt, SimContext& ctx, f32 econ_eff) {
    const bool awaited = std::exchange(arm_awaited_, false);
    if (!awaited && !is_reclaiming() && !is_repairing() && !is_capturing() && !is_building() &&
        (command_queue_.empty() || (command_queue_.front().type != CommandType::BuildMobile &&
                                    command_queue_.front().task_wait <= 0))) {
        aim_builder_arms(nullptr, ctx.L);
    }
    // An attack run ends with its order: Moho's flight resets the combat
    // state once the aircraft has no target to attack.
    // So does a hovering aircraft's circling, which goes on while it works
    // (Moho's hover orientation zeroes the circling's timeout).
    if (air_combat_.flying || air_combat_.state != 0) {
        const bool running =
            (!command_queue_.empty() && command_queue_.front().type == CommandType::Attack &&
             command_queue_.front().engaged) ||
            circles_its_work() || flies_winged_on_guard(*this);
        if (!running) end_attack_run(*this);
    }
    while (!command_queue_.empty()) {
        note_queue_head();
        // Orders run script callbacks, which may destroy this unit (it stays
        // allocated until the tick ends, see EntityRegistry::collect_garbage).
        if (destroyed() || !in_registry()) return false;
        if (!command_queue_.front().begun) {
            // Its scripts may change the queue, or kill the unit: look again.
            begin_order(command_queue_.front(), ctx.L);
            continue;
        }
        UnitCommand& front = command_queue_.front();
        if (front.from_patrol && front.patrol_scan > 0) {
            --front.patrol_scan;
            return true;
        }
        const OrderStep step = run_order(front, dt, ctx, econ_eff);
        if (step == OrderStep::Hold) return true;
        if (step == OrderStep::Gone) return false;
    }
    return true;
}

void Unit::begin_order(UnitCommand& cmd, lua_State* L) {
    cmd.begun = true;
    if (cmd.type == CommandType::Patrol || cmd.type == CommandType::AggressiveMove) {
        cmd.patrol_from = position();
    }
    // Moho's move, patrol, guard and attack tasks, as they are made: an
    // Immobile NeedUnpack unit's attacker drops its desired target, and with
    // it every weapon's (CAiAttackerImpl::SetDesiredTarget). Retail's
    // DefaultProjectileWeapon packs up on OnLostTarget and, packed, calls
    // SetImmobile(false).
    if (!immobile_ || !need_unpack_) return;
    switch (cmd.type) {
    case CommandType::Move:
    case CommandType::Patrol:
    case CommandType::AggressiveMove:
    case CommandType::Guard:
    case CommandType::Attack: break;
    default: return;
    }
    for (size_t i = 0; i < weapons_.size(); ++i) {
        // A script may kill the unit, and with it its weapons.
        if (destroyed() || is_dying()) return;
        weapons_[i]->drop_target(L);
    }
}

OrderStep Unit::run_order(UnitCommand& cmd, f64 dt, SimContext& ctx, f32 econ_eff) {
    switch (cmd.type) {
    case CommandType::Stop: return order_stop();
    case CommandType::Move: return order_move(cmd, dt, ctx);
    case CommandType::Attack: return order_attack(cmd, dt, ctx);
    case CommandType::BuildMobile: return order_build_mobile(cmd, dt, ctx, econ_eff);
    case CommandType::BuildFactory:
    case CommandType::Upgrade: return order_build_in_place(cmd, dt, ctx, econ_eff);
    case CommandType::Patrol:
    case CommandType::AggressiveMove: return order_patrol(cmd, dt, ctx);
    case CommandType::Reclaim: return order_reclaim(cmd, dt, ctx);
    case CommandType::Repair: return order_repair(cmd, dt, ctx, econ_eff);
    case CommandType::Capture: return order_capture(cmd, dt, ctx, econ_eff);
    case CommandType::Guard: return order_guard(cmd, dt, ctx, econ_eff);
    case CommandType::Dive: return order_dive(ctx.L);
    case CommandType::Enhance: return order_enhance(cmd, dt, ctx, econ_eff);
    case CommandType::Script: return order_script(cmd, ctx);
    case CommandType::TransportLoad:
    case CommandType::Dock: return order_transport_load(cmd, dt, ctx);
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
        set_path_goal(cmd.target_pos, ctx);
    }
    if (!nav_update(dt, ctx.terrain, cmd.speed_cap)) {
        command_queue_.pop_front();
        if (is_air_unit() &&
            (command_queue_.empty() || instant_order(command_queue_.front().type))) {
            stop_air();
        }
        return OrderStep::Next;
    }
    return OrderStep::Hold; // Still moving
}

OrderStep Unit::order_attack(UnitCommand& cmd, f64 dt, SimContext& ctx) {
    auto& registry = ctx.registry;
    // Attack: move toward target if out of weapon range, else stop
    if (cmd.target_id == 0) return order_attack_ground(cmd, dt, ctx);
    auto* target = registry.find(cmd.target_id);
    if (!target || target->destroyed()) {
        if (cmd.engaged) end_attack_run(*this); // the run ends with its target
        release_navigator();                    // and the chase
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
        release_navigator();
        command_queue_.pop_front();
        return OrderStep::Next;
    }
    // Check distance to target
    f32 dx = target->position().x - position().x;
    f32 dz = target->position().z - position().z;
    f32 dist2 = dx * dx + dz * dz;
    f32 range2 = best_range * best_range;
    // A guard's or patrol's fight goes only so far (Moho's acquire task,
    // CheckTargetGuardExempt): once the unit has come within reach of its
    // target, it gives up beyond GuardReturnRadius of what it guards, or of
    // where its patrol broke off, and the guard or patrol carries on.
    if (cmd.from_guard || cmd.from_patrol) {
        if (dist2 <= range2 || cmd.engaged) cmd.leash_armed = true;
        if (cmd.leash_armed) {
            const Entity* by = cmd.leash_anchor_id ? registry.find(cmd.leash_anchor_id) : nullptr;
            const Vector3 anchor = by && !by->destroyed() ? by->position() : cmd.leash_anchor_pos;
            const f32 ax = position().x - anchor.x;
            const f32 ay = position().y - anchor.y;
            const f32 az = position().z - anchor.z;
            if (ax * ax + ay * ay + az * az > guard_return_radius_ * guard_return_radius_) {
                if (cmd.engaged) end_attack_run(*this);
                navigator_.abort_move();
                command_queue_.pop_front();
                return OrderStep::Next;
            }
        }
    }
    // A winged aircraft flies at its target and, once within a weapon's
    // reach or its EngageDistance, makes its runs: Moho's attack task hands
    // the target to the attacker and CalcMoveAir flies the combat tactics
    // from then on. It never parks at range.
    if (air_combat_rules_.winged && layer_ == "Air" && ctx.sim) {
        const f32 engage = air_combat_rules_.engage_distance;
        if (!cmd.engaged && (dist2 <= range2 || dist2 < engage * engage)) cmd.engaged = true;
        if (cmd.engaged) {
            navigator_.abort_move();
            fly_attack_run(*this, *target, *ctx.sim, ctx.terrain, static_cast<f32>(dt));
            return OrderStep::Hold;
        }
    }
    // A hovering aircraft is handed its target alike, and from then on
    // circles it (Moho's CalcCirclingOrientation) at its target weapon's
    // reach -- the first weapon that can attack it -- facing it.
    if (circles(*this) && ctx.sim) {
        const f32 engage = air_combat_rules_.engage_distance;
        if (!cmd.engaged && (dist2 <= range2 || dist2 < engage * engage)) cmd.engaged = true;
        if (cmd.engaged) {
            navigator_.abort_move();
            CircleAround around;
            around.center = target->position();
            around.move_goal = target->position();
            for (const auto& w : weapons_) {
                if (w->enabled && w->attacks_on_order() && w->can_pick(*this, *target, ctx.sim)) {
                    around.weapon_radius = w->max_range;
                    break;
                }
            }
            around.target_in_air =
                target->is_unit() && static_cast<const Unit*>(target)->layer() == "Air";
            fly_circling(*this, around, *ctx.sim, ctx.terrain, static_cast<f32>(dt));
            return OrderStep::Hold;
        }
    }
    if (dist2 > range2) {
        // Move toward target
        if (!navigator_.is_moving() || navigator_.goal().x != target->position().x ||
            navigator_.goal().z != target->position().z) {
            set_path_goal(target->position(), ctx);
        }
        nav_update(dt, ctx.terrain);
    } else {
        navigator_.abort_move();
        // Parked, every 9 ticks it asks to face its target AttackAngle off
        // the bow (Moho's attack task, Complete: SetFacing, return 10).
        if (attack_angle_ > 0.0f) {
            if (cmd.facing_clock <= 0) {
                constexpr f32 kDegToRad = 3.14159265358979f / 180.0f;
                const f32 heading = attack_angle_heading(
                    osc::dmath::atan2(dx, dz), quat_yaw(orientation()), attack_angle_ * kDegToRad);
                attack_facing_ = {osc::dmath::sin(heading), 0.0f, osc::dmath::cos(heading)};
                cmd.facing_clock = 8;
            } else {
                --cmd.facing_clock;
            }
        }
    }
    return OrderStep::Hold; // Stay on this command
}

OrderStep Unit::order_attack_ground(UnitCommand& cmd, f64 dt, SimContext& ctx) {
    // Moho's CUnitAttackTargetTask on a ground target (faf-re). The first
    // weapon that can hit the ground there (CAiAttackerImpl::GetTargetWeapon;
    // here the first enabled one) brings its unit within its range, and the
    // attacker's desired target is the point from then on (cmd.engaged:
    // Weapon::update_targeting hands it to every weapon that can hit it).
    // The order never ends by itself. Only when another order follows, and
    // that weapon has fired AttackGroundTries shots at the point, does it go
    // to the back of the queue: Moho's command dispatch sends a ground attack
    // round as it does a patrol.
    const Vector3 at = cmd.target_pos;
    const Weapon* weapon = nullptr;
    for (const auto& w : weapons_) {
        if (w->enabled && w->max_range > 0 && w->attacks_on_order() &&
            w->can_attack_ground(at, ctx.terrain)) {
            weapon = w.get();
            break;
        }
    }
    // A formation's attack (IssueFormAttack; Moho's CreateRespectFormation)
    // with no weapon that can hit the point ends at once (TaskTick's first
    // test), going round like the rest when another order follows; a plain
    // attack's unit goes to the point and stays.
    const bool spent = weapon && cmd.engaged && weapon->ground_from_order &&
                       weapon->has_ground_target && weapon->ground_target.x == at.x &&
                       weapon->ground_target.z == at.z &&
                       static_cast<i64>(weapon->shots_at_target) >= weapon->attack_ground_tries;
    if (!weapon && !cmd.formation.empty() && command_queue_.size() < 2) {
        navigator_.abort_move();
        command_queue_.pop_front();
        return OrderStep::Next;
    }
    if (command_queue_.size() >= 2 && (spent || (!weapon && !cmd.formation.empty()))) {
        navigator_.abort_move();
        auto finished = std::move(cmd); // cmd is the element pop_front destroys
        finished.begun = false;
        finished.engaged = false;
        command_queue_.pop_front();
        command_queue_.push_back(std::move(finished));
        return OrderStep::Hold; // the next order starts next tick
    }

    const f32 dx = at.x - position().x;
    const f32 dz = at.z - position().z;
    const f32 dist2 = dx * dx + dz * dz;
    // A winged aircraft flies at the point and, within its weapon's reach or
    // its EngageDistance, makes its runs over it. With no weapon that can
    // hit it, the order ends there (the attacker can't attack it).
    if (air_combat_rules_.winged && layer_ == "Air" && ctx.sim) {
        const f32 engage = air_combat_rules_.engage_distance;
        const bool reached =
            (weapon && weapon->in_firing_range(*this, at)) || dist2 < engage * engage;
        if (!cmd.engaged && reached) {
            if (!weapon) {
                command_queue_.pop_front();
                return OrderStep::Next;
            }
            cmd.engaged = true;
        }
        if (cmd.engaged) {
            navigator_.abort_move();
            fly_attack_run(*this, at, *ctx.sim, ctx.terrain, static_cast<f32>(dt));
            return OrderStep::Hold;
        }
    }
    // A hovering aircraft, handed the point alike, circles it at its
    // weapon's reach (with none, StartTurnDistance's circle).
    if (circles(*this) && ctx.sim) {
        const f32 engage = air_combat_rules_.engage_distance;
        if (!cmd.engaged &&
            ((weapon && weapon->in_firing_range(*this, at)) || dist2 < engage * engage))
            cmd.engaged = true;
        if (cmd.engaged) {
            navigator_.abort_move();
            CircleAround around;
            around.center = at;
            around.move_goal = at;
            around.weapon_radius = weapon ? weapon->max_range : 0.0f;
            fly_circling(*this, around, *ctx.sim, ctx.terrain, static_cast<f32>(dt));
            return OrderStep::Hold;
        }
    }
    // A structure's attacker takes the point at once; its weapons fire when
    // it is in their range.
    if (!is_mobile()) {
        cmd.engaged = true;
        return OrderStep::Hold;
    }
    if (weapon && weapon->in_firing_range(*this, at)) {
        navigator_.abort_move();
        cmd.engaged = true;
        return OrderStep::Hold;
    }
    // Out of range it goes toward the point; inside its weapon's minimum
    // range it backs off GuardScanRadius from the point (the task's
    // Starting state). With no weapon that can hit the point it goes there
    // and stays.
    Vector3 goal = at;
    if (weapon && dist2 < weapon->min_range * weapon->min_range && dist2 > 0) {
        const f32 dist = std::sqrt(dist2);
        goal = {at.x - dx / dist * guard_scan_radius_, at.y, at.z - dz / dist * guard_scan_radius_};
    }
    const f32 gx = goal.x - position().x;
    const f32 gz = goal.z - position().z;
    if (!weapon && gx * gx + gz * gz <= 1.0f) {
        navigator_.abort_move();
        return OrderStep::Hold;
    }
    if (!navigator_.is_moving() || navigator_.goal().x != goal.x || navigator_.goal().z != goal.z) {
        set_path_goal(goal, ctx);
    }
    nav_update(dt, ctx.terrain);
    return OrderStep::Hold;
}

OrderStep Unit::order_build_mobile(UnitCommand& cmd, f64 dt, SimContext& ctx, f32 econ_eff) {
    auto& registry = ctx.registry;
    auto* L = ctx.L;
    if (build_target_id_ == 0) {
        if (!cmd.site_cleared && cmd.clearing_prop_id == 0) {
            const BuildSiteProp site =
                find_build_site_prop(L, registry, *this, cmd.blueprint_id, cmd.target_pos);
            if (destroyed() || !in_registry()) {
                return OrderStep::Gone;
            }
            if (site.reclaim_id != 0) {
                cmd.clearing_prop_id = site.reclaim_id;
                cmd.clearing_approached = false;
            } else {
                cmd.site_cleared = true;
                if (site.rebuild) {
                    cmd.rebuild_wreck_id = site.wreck_id;
                    cmd.rebuild_bonus = site.bonus;
                }
            }
        }
        if (cmd.clearing_prop_id != 0) {
            UnitCommand reclaim;
            reclaim.type = CommandType::Reclaim;
            reclaim.target_id = cmd.clearing_prop_id;
            reclaim.approached = cmd.clearing_approached;
            const u32 building = cmd.command_id;
            const OrderStep step = reclaim_work(reclaim, dt, ctx);
            if (step == OrderStep::Gone) {
                return OrderStep::Gone;
            }
            if (command_queue_.empty() || command_queue_.front().command_id != building) {
                return OrderStep::Next;
            }
            const bool done = step == OrderStep::Next;
            cmd.clearing_prop_id = done ? 0 : reclaim.target_id;
            cmd.clearing_approached = !done && reclaim.approached;
            return OrderStep::Hold;
        }
        // Phase 1: reach the site. In range when the gap to its skirt
        // is within MaxBuildDistance and the builder is off the
        // skirt (Moho's CUnitMobileBuildTask); else it walks just
        // clear of the skirt, one cell out, and builds from there if
        // in range, or gives up.
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
            set_path_goal(approach_point(*this, cmd.target_pos, half_x + 1, half_z + 1), ctx);
        }
        if (cmd.approached && approach_update(dt, ctx)) return OrderStep::Hold;
        // After the walk only range decides: the pathfinder may have moved
        // its goal onto the site
        const f32 skirt = std::max(cmd.site_skirt_x, cmd.site_skirt_z);
        const bool in_range = work_gap(*this, cmd.target_pos, skirt) <= max_build_distance_;
        if (!reachable() && !(cmd.approached && in_range)) {
            command_queue_.pop_front();
            return OrderStep::Next;
        }
        navigator_.abort_move();
        {
            const Vector3 site{cmd.target_pos.x, cmd.target_pos.y + 0.5f, cmd.target_pos.z};
            aim_builder_arms(&site, L);
            if (destroyed() || !in_registry()) {
                return OrderStep::Gone;
            }
        }

        // Phase 2: Spawn skeleton unit
        if (waits_out_task(cmd)) {
            return OrderStep::Hold;
        }
        if (build_blocked_by_lobby_rules(*this, cmd, ctx)) {
            command_queue_.pop_front();
            return OrderStep::Next;
        }
        if (turns_to_face(cmd.target_pos) || awaits_arm() || waits_paused(cmd)) {
            return OrderStep::Hold;
        }
        const u32 building = cmd.command_id; // cmd may go with the scripts' changes
        const u32 wreck_id = cmd.rebuild_wreck_id;
        const f32 bonus = cmd.rebuild_bonus;
        switch (start_build(cmd, registry, L)) {
        case BuildStart::Started: break;
        case BuildStart::AtCap: return hold_for_unit_cap(building);
        case BuildStart::Failed:
            if (!command_queue_.empty() && command_queue_.front().command_id == building)
                command_queue_.pop_front();
            return OrderStep::Next;
        }
        if (Entity* wreck = wreck_id != 0 ? registry.find(wreck_id) : nullptr;
            wreck && !wreck->destroyed() && ctx.sim) {
            ctx.sim->notify_script_destroy(*wreck);
            if (!wreck->destroyed()) {
                wreck->mark_destroyed();
                registry.unregister_entity(wreck_id);
            }
        }
        if (destroyed() || !in_registry()) {
            return OrderStep::Gone;
        }
        auto* built = registry.find(build_target_id_);
        if (bonus > 0.0f && built && !built->destroyed() && built->is_unit()) {
            materialize(static_cast<Unit&>(*built), bonus);
        }
    }
    // Phase 3: Progress the build
    if (waits_out_task(cmd)) {
        return OrderStep::Hold;
    }
    if (const Entity* site = registry.find(build_target_id_);
        site && !site->destroyed() && waits_paused(cmd)) {
        return OrderStep::Hold;
    }
    if (!progress_build(dt, registry, L, ctx.pathfinding_grid, econ_eff)) {
        command_queue_.pop_front();
        return OrderStep::Next;
    }
    return OrderStep::Hold;
}

OrderStep Unit::order_build_in_place(UnitCommand& cmd, f64 dt, SimContext& ctx, f32 econ_eff) {
    auto& registry = ctx.registry;
    auto* L = ctx.L;
    // A factory whose unit is done keeps the order while it rolls the unit
    // off (holds_for_rolloff), and only then goes on to its next.
    if (cmd.rolloff_wait > 0) {
        if (holds_for_rolloff(cmd.rolloff_wait)) return OrderStep::Hold;
        return end_factory_build_order(cmd);
    }
    if (build_target_id_ == 0) {
        // Factory: spawn immediately at own position
        if (waits_out_task(cmd)) {
            return OrderStep::Hold;
        }
        if (build_blocked_by_lobby_rules(*this, cmd, ctx)) {
            command_queue_.pop_front();
            return OrderStep::Next;
        }
        if (waits_paused(cmd)) {
            return OrderStep::Hold;
        }
        const u32 building = cmd.command_id; // cmd may go with the scripts' changes
        switch (start_build(cmd, registry, L)) {
        case BuildStart::Started: break;
        case BuildStart::AtCap: return hold_for_unit_cap(building);
        case BuildStart::Failed:
            if (!command_queue_.empty() && command_queue_.front().command_id == building)
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
        if (built && factory_build) {
            cmd.rolloff_wait = 2; // the roll-off, above
            return OrderStep::Hold;
        }
        // A failed build (or a finished upgrade) goes at once.
        command_queue_.pop_front();
        return OrderStep::Next;
    }
    return OrderStep::Hold;
}

bool Unit::waits_out_task(UnitCommand& cmd) {
    if (cmd.task_wait <= 0) {
        return false;
    }
    --cmd.task_wait;
    return cmd.task_wait > 0;
}

bool Unit::waits_paused(UnitCommand& cmd) const {
    if (!paused_) {
        return false;
    }
    cmd.task_wait = kTaskRetryTicks;
    return true;
}

OrderStep Unit::hold_for_unit_cap(u32 command_id) {
    if (!command_queue_.empty() && command_queue_.front().command_id == command_id)
        command_queue_.front().task_wait = kTaskRetryTicks;
    return OrderStep::Hold;
}

bool Unit::holds_for_rolloff(i32& wait) const {
    // Moho's CFactoryBuildTask, its unit built: two steps (Starting, then
    // Processing), then 10 ticks at a time while the factory is busy -- its
    // FinishBuildThread and RolloffBody keep it so until the new unit's
    // roll-off move is done.
    if (wait <= 0) return false;
    if (--wait > 0) return true;
    if (busy_) {
        wait = 10;
        return true;
    }
    return false;
}

OrderStep Unit::end_factory_build_order(UnitCommand& cmd) {
    // Moho's command dispatch, a unit built: an order with more to make
    // counts down and builds the next at once; one done goes, or, on a
    // repeating factory, to the back with its count back at its most, the
    // next order starting next tick.
    if (cmd.count > 1) {
        --cmd.count;
        cmd.rolloff_wait = 0;
        cmd.task_wait = 0;
        return OrderStep::Next;
    }
    if (repeat_queue_) {
        auto finished = std::move(cmd); // cmd is the element pop_front destroys
        finished.rolloff_wait = 0;
        finished.count = std::max(finished.max_count, 1);
        command_queue_.pop_front();
        command_queue_.push_back(std::move(finished));
        return OrderStep::Hold;
    }
    command_queue_.pop_front();
    return OrderStep::Next;
}

OrderStep Unit::order_patrol(UnitCommand& cmd, f64 dt, SimContext& ctx) {
    // Moho's CUnitPatrolTask::Execute, every 6 ticks (it returns 7).
    if (cmd.patrol_scan > 0) {
        --cmd.patrol_scan;
    } else {
        cmd.patrol_scan = 5;
        // Moho's patrol task returns 7 with its new task above it, which so
        // starts 6 ticks on; done, it hands straight back to the patrol's Execute.
        // An aircraft goes at once: nothing here flies it while it would wait.
        const auto break_off = [&](UnitCommand order) {
            order.command_id = cmd.command_id;
            order.from_patrol = true;
            order.patrol_scan = is_air_unit() ? 0 : 5;
            cmd.patrol_scan = 0;
            navigator_.abort_move();
            command_queue_.push_front(
                std::move(order)); // cmd stays valid: a deque keeps references
            return OrderStep::Hold;
        };
        if (Unit* pad = find_platform(ctx)) {
            // As Moho's patrol task does (TransportResetReservation): a
            // carrier's landing points go round again from the first, so two
            // patrols breaking off to one carrier moments apart can share a
            // point.
            pad->reset_storage_reservation();
            UnitCommand refuel;
            refuel.type = CommandType::Dock;
            refuel.target_id = pad->entity_id();
            refuel.target_pos = pad->position();
            return break_off(std::move(refuel));
        }
        if (Entity* enemy = find_patrol_target(cmd, ctx)) {
            UnitCommand attack;
            attack.type = CommandType::Attack;
            attack.target_id = enemy->entity_id();
            attack.target_pos = enemy->position();
            // Moho's patrol task keeps where it broke off (GuardedPos): the
            // fight's leash runs from there.
            attack.leash_anchor_pos = position();
            return break_off(std::move(attack));
        }
        if (Entity* work = find_patrol_work(cmd, ctx)) {
            UnitCommand order;
            order.target_id = work->entity_id();
            order.target_pos = work->position();
            if (work->is_unit() && ctx.sim->is_ally(army(), work->army())) {
                order.type = CommandType::Repair;
            } else {
                order.type = CommandType::Reclaim;
                cmd.patrol_claimed.push_back(work->entity_id());
            }
            return break_off(std::move(order));
        }
    }
    if (!navigator_.is_moving() || navigator_.goal().x != cmd.target_pos.x ||
        navigator_.goal().z != cmd.target_pos.z) {
        set_path_goal(cmd.target_pos, ctx);
    }
    if (!nav_update(dt, ctx.terrain, cmd.speed_cap)) {
        // An attack-move is one leg: Moho's dispatch removes the order when
        // its patrol task is done.
        if (cmd.type == CommandType::AggressiveMove) {
            command_queue_.pop_front();
            return OrderStep::Next;
        }
        // Reached patrol point — cycle to back of queue (moved out first:
        // cmd is the element pop_front destroys). The next leg starts next
        // tick: a patrol whose points it already stands on would otherwise
        // go round them for ever within this one.
        auto finished = std::move(cmd);
        finished.patrol_claimed.clear();
        finished.begun = false; // Moho makes each leg a new patrol task
        command_queue_.pop_front();
        command_queue_.push_back(std::move(finished));
        return OrderStep::Hold;
    }
    return OrderStep::Hold;
}

OrderStep Unit::order_reclaim(UnitCommand& cmd, f64 dt, SimContext& ctx) {
    const OrderStep step = reclaim_work(cmd, dt, ctx);
    if (step == OrderStep::Next) {
        command_queue_.pop_front();
    }
    return step;
}

OrderStep Unit::reclaim_work(UnitCommand& cmd, f64 dt, SimContext& ctx) {
    auto& registry = ctx.registry;
    auto* L = ctx.L;
    if (cmd.target_id == 0) {
        if (is_reclaiming()) stop_reclaiming(ctx.L, &ctx.registry);
        end_approach(cmd);
        return OrderStep::Next;
    }
    auto* target = registry.find(cmd.target_id);
    if (!target || target->destroyed() || !target->reclaimable() ||
        (reclaim_target_id_ != cmd.target_id && !reclaim_target_valid(*target))) {
        if (is_reclaiming()) stop_reclaiming(ctx.L, &ctx.registry);
        end_approach(cmd);
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
                    return OrderStep::Next;
                }
                cmd.approached = true;
                set_path_goal(approach_point(*this, target->position(),
                                             target->footprint_size_x() * 0.5f,
                                             target->footprint_size_z() * 0.5f),
                              ctx);
            }
            if (cmd.approached && approach_update(dt, ctx)) return OrderStep::Hold;
            if (gap > max_build_distance_) {
                if (is_reclaiming()) stop_reclaiming(ctx.L, &ctx.registry);
                return OrderStep::Next;
            }
        } else if (gap > max_build_distance_) {
            stop_reclaiming(ctx.L, &ctx.registry);
            return OrderStep::Next;
        }
    }
    navigator_.abort_move();

    // Start reclaim if not already reclaiming this target
    if (reclaim_target_id_ != cmd.target_id) {
        if (is_reclaiming()) stop_reclaiming(ctx.L, &ctx.registry);

        const ReclaimCosts costs = reclaim_costs(L, *this, *target, static_cast<f64>(build_rate_));
        if (costs.mass < 0 || costs.energy < 0 || build_rate_ <= 0) {
            return OrderStep::Next;
        }
        f64 reclaim_time = costs.time;
        if (reclaim_time <= 0) reclaim_time = 0.01;

        reclaim_rate_ = static_cast<f32>(1.0 / reclaim_time);
        begin_reclaim(cmd.target_id, ctx.L, ctx.registry);
        if (destroyed() || !in_registry()) {
            return OrderStep::Gone;
        }

        economy_.reclaim_mass = 0;
        economy_.reclaim_energy = 0;

        spdlog::info("Reclaim start: entity #{} reclaiming #{} "
                     "(mass={:.0f}, energy={:.0f}, time={:.1f}s)",
                     entity_id(), cmd.target_id, costs.mass, costs.energy, reclaim_time);
    }

    if (reclaim_wait_ > 0) {
        const auto* reclaimed = registry.find(cmd.target_id);
        if (reclaim_wait_ == 1 && reclaimed && !reclaim_arm_ready(*reclaimed)) {
            return OrderStep::Hold;
        }
        --reclaim_wait_;
        if (reclaim_wait_ == 0 && reclaimed) {
            const ReclaimCosts costs =
                reclaim_costs(L, *this, *reclaimed, static_cast<f64>(build_rate_));
            economy_.reclaim_mass = costs.mass * static_cast<f64>(reclaim_rate_);
            economy_.reclaim_energy = costs.energy * static_cast<f64>(reclaim_rate_);
        }
        return OrderStep::Hold;
    }

    // Moho's CUnitReclaimTask: a finished unit loses health for nothing, then
    // the task goes on to the wreck its CreateWreckageProp(0) returns.
    auto* reclaimed = registry.find(cmd.target_id);
    if (reclaimed && reclaim_wears_down(*reclaimed)) {
        if (!reclaim_arm_ready(*reclaimed)) {
            return OrderStep::Hold;
        }
        if (wear_down(static_cast<Unit&>(*reclaimed))) {
            return OrderStep::Hold;
        }
        const u32 wreck_id = reclaim_into_wreck(cmd.target_id, registry, L);
        if (destroyed() || !in_registry()) {
            return OrderStep::Gone;
        }
        auto* wreck = wreck_id != 0 ? registry.find(wreck_id) : nullptr;
        if (!wreck) {
            stop_reclaiming(L, &registry);
            return OrderStep::Next;
        }
        const ReclaimCosts costs = reclaim_costs(L, *this, *wreck, static_cast<f64>(build_rate_));
        const f64 reclaim_time = costs.time > 0 ? costs.time : 0.01;
        cmd.target_id = wreck_id;
        reclaim_target_id_ = wreck_id;
        reclaim_rate_ = static_cast<f32>(1.0 / reclaim_time);
        reclaim_wait_ = 1;
        return OrderStep::Hold;
    }

    // Progress reclaim
    if (!progress_reclaim(dt, registry, L)) {
        return OrderStep::Next;
    }
    return OrderStep::Hold;
}

OrderStep Unit::order_repair(UnitCommand& cmd, f64 dt, SimContext& ctx, f32 econ_eff) {
    auto& registry = ctx.registry;
    auto* L = ctx.L;
    // Whatever work this order started on its target ends with it: a repair,
    // or the build of one under construction.
    const auto let_go = [&] {
        if (is_repairing()) stop_repairing(L, registry);
        if (build_target_id_ != 0 && build_target_id_ == cmd.target_id) {
            stop_assisting(L, &registry);
        }
        end_approach(cmd);
    };
    if (cmd.target_id == 0) {
        let_go();
        command_queue_.pop_front();
        return OrderStep::Next;
    }
    auto* rtarget = registry.find(cmd.target_id);
    if (!rtarget || rtarget->destroyed() || !rtarget->is_unit()) {
        let_go();
        command_queue_.pop_front();
        return OrderStep::Next;
    }
    // Already at full health? Done. (One under construction is built first.)
    const bool under_construction = static_cast<const Unit&>(*rtarget).is_being_built();
    if (!under_construction && rtarget->health() >= rtarget->max_health() &&
        !static_cast<Unit*>(rtarget)->shield_needs_repair(registry, L)) {
        let_go();
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
        const bool working =
            repair_target_id_ == cmd.target_id || build_target_id_ == cmd.target_id;
        if (!working) {
            if (!cmd.approached && gap > max_build_distance_) {
                if (effective_speed() <= 0) {
                    command_queue_.pop_front();
                    return OrderStep::Next;
                }
                cmd.approached = true;
                set_path_goal(approach_point(*this, rtarget->position(),
                                             runit.skirt_size_x() * 0.5f + 1,
                                             runit.skirt_size_z() * 0.5f + 1),
                              ctx);
            }
            if (cmd.approached && approach_update(dt, ctx)) return OrderStep::Hold;
            if (gap > max_build_distance_) {
                let_go();
                command_queue_.pop_front();
                return OrderStep::Next;
            }
        } else if (gap > 2 * max_build_distance_) {
            let_go();
            command_queue_.pop_front();
            return OrderStep::Next;
        }
    }
    navigator_.abort_move();

    if (under_construction) {
        if (is_repairing()) stop_repairing(L, registry);
        return order_repair_construction(cmd, dt, ctx, econ_eff, *static_cast<Unit*>(rtarget));
    }

    // Start repair if not already repairing this target
    if (repair_target_id_ != cmd.target_id) {
        if (is_repairing()) stop_repairing(L, registry);
        const Vector3 at = rtarget->position();
        aim_builder_arms(&at, L);
        if (destroyed() || !in_registry()) {
            return OrderStep::Gone;
        }
        if (turns_to_face(at) || awaits_arm() || waits_out_task(cmd) || waits_paused(cmd)) {
            return OrderStep::Hold;
        }
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

OrderStep Unit::order_repair_construction(UnitCommand& cmd, f64 dt, SimContext& ctx, f32 econ_eff,
                                          Unit& target) {
    auto& registry = ctx.registry;
    const u32 order_id = cmd.command_id;
    const u32 tid = target.entity_id();
    const auto done = [&] {
        if (!command_queue_.empty() && &command_queue_.front() == &cmd &&
            command_queue_.front().command_id == order_id)
            command_queue_.pop_front();
        return OrderStep::Next;
    };
    if (build_target_id_ != tid) {
        // Its blueprint's build time and costs set the pace and the bill.
        if (is_building()) {
            stop_assisting(ctx.L, &registry);
        }
        const BuildEconomy costs = blueprint_build_economy(ctx.L, target.unit_id());
        if (costs.time <= 0 || build_rate_ <= 0) return done();
        const Vector3 at = target.position();
        aim_builder_arms(&at, ctx.L);
        if (destroyed() || !in_registry()) {
            return OrderStep::Gone;
        }
        if (turns_to_face(at) || awaits_arm() || waits_out_task(cmd) || waits_paused(cmd)) {
            return OrderStep::Hold;
        }
        build_target_id_ = tid;
        build_command_id_ = cmd.command_id;
        build_released_with_order_ = true;
        build_repairs_ = true;
        build_time_ = costs.time;
        build_cost_mass_ = costs.mass;
        build_cost_energy_ = costs.energy;
        work_progress_ = target.fraction_complete();
        economy_.consumption_mass = costs.mass * static_cast<f64>(build_rate_) / costs.time;
        economy_.consumption_energy = costs.energy * static_cast<f64>(build_rate_) / costs.time;
        economy_.consumption_active = true;
        call_build_callback(ctx.L, "OnStartBuild", registry.find(tid), "Repair");
        if (destroyed() || !in_registry()) {
            return OrderStep::Gone;
        }
    }
    // It builds alongside any builder; whoever completes it finishes it
    // (OnStopBeingBuilt, once).
    if (progress_build(dt, registry, ctx.L, ctx.pathfinding_grid, econ_eff)) return OrderStep::Hold;
    if (destroyed() || !in_registry()) return OrderStep::Gone;
    return done();
}

OrderStep Unit::order_capture(UnitCommand& cmd, f64 dt, SimContext& ctx, f32 econ_eff) {
    auto& registry = ctx.registry;
    auto* L = ctx.L;
    if (cmd.target_id == 0) {
        if (is_capturing()) stop_capturing(L, registry, true);
        end_approach(cmd);
        command_queue_.pop_front();
        return OrderStep::Next;
    }
    auto* ctarget = registry.find(cmd.target_id);
    if (!ctarget || ctarget->destroyed() || !ctarget->is_unit()) {
        if (is_capturing()) stop_capturing(L, registry, true);
        end_approach(cmd);
        command_queue_.pop_front();
        return OrderStep::Next;
    }
    // Already same army (captured by someone else)
    if (ctarget->is_unit() && static_cast<Unit*>(ctarget)->army() == army()) {
        if (is_capturing()) stop_capturing(L, registry, false);
        end_approach(cmd);
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
                set_path_goal(approach_point(*this, ctarget->position(),
                                             cunit.skirt_size_x() * 0.5f,
                                             cunit.skirt_size_z() * 0.5f),
                              ctx);
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
    assist_pending_bp_.clear();
    if (factory_assist_build_) {
        factory_assist_build_ = false;
        assist_rolloff_wait_ = 0;
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

bool Unit::guard_reacts(const Unit* guarded) const {
    // Moho's guard task (its constructor's classification): an engineer
    // guarding an engineer or a factory only helps; anything else takes on
    // enemies if it has a weapon to. (A factory guarding a factory assists
    // before it would ever look.)
    static const CategoryName kEngineer{"ENGINEER"};
    static const CategoryName kFactory{"FACTORY"};
    if (guarded && has_category(kEngineer) &&
        (guarded->has_category(kEngineer) || guarded->has_category(kFactory)))
        return false;
    return scan_weapon() != nullptr;
}

void Unit::walk_to(const Vector3& goal, f64 dt, SimContext& ctx) {
    const Vector3 heading = navigator_.goal();
    if (!navigator_.is_moving() || std::abs(heading.x - goal.x) > 1.0f ||
        std::abs(heading.z - goal.z) > 1.0f) {
        set_path_goal(goal, ctx);
    }
    nav_update(dt, ctx.terrain);
}

std::optional<OrderStep> Unit::guard_engage(UnitCommand& cmd, const Unit* guarded,
                                            const Vector3& ref, f64 dt, SimContext& ctx) {
    if (!guard_reacts(guarded)) return std::nullopt;
    // Back from a fight, it goes home before it looks about again (Moho's
    // Starting state: within its guarded unit's footprint and half its
    // GuardScanRadius of it, or of the point).
    if (cmd.guard_returning) {
        const f32 size =
            guarded ? std::max(guarded->footprint_size_x(), guarded->footprint_size_z()) : 1.0f;
        const f32 home = size + 0.5f * guard_scan_radius_;
        const f32 dx = position().x - ref.x;
        const f32 dy = position().y - ref.y;
        const f32 dz = position().z - ref.z;
        if (dx * dx + dy * dy + dz * dz > home * home) {
            walk_to(ref, dt, ctx);
            return OrderStep::Hold;
        }
        cmd.guard_returning = false;
        cmd.patrol_scan = 5; // this Execute returns 7: the next look is 6 ticks on
        return std::nullopt;
    }
    // It looks every 6 ticks (Moho's Execute returns 7), and not while it
    // helps: Moho's help is a task of its own above the guard's.
    if (is_building() || is_reclaiming() || is_repairing()) return std::nullopt;
    if (cmd.patrol_scan > 0) {
        --cmd.patrol_scan;
        return std::nullopt;
    }
    cmd.patrol_scan = 5;
    Entity* enemy =
        best_enemy(ctx.registry.units_in_radius(position().x, position().z, guard_scan_radius_),
                   guard_scan_radius_, ctx);
    if (!enemy) return std::nullopt;
    // Moho's SetEnemy: its attack task above the guard's, the fight
    // leashed to what it guards.
    UnitCommand attack;
    attack.type = CommandType::Attack;
    attack.target_id = enemy->entity_id();
    attack.target_pos = enemy->position();
    attack.command_id = cmd.command_id;
    attack.from_guard = true;
    attack.leash_anchor_id = guarded ? guarded->entity_id() : 0;
    attack.leash_anchor_pos = ref;
    cmd.guard_returning = true;
    navigator_.abort_move();
    command_queue_.push_front(std::move(attack)); // cmd stays valid: a deque keeps references
    return OrderStep::Next;
}

OrderStep Unit::order_guard_point(UnitCommand& cmd, f64 dt, SimContext& ctx) {
    // A place to guard (IssueGuard at a position): Moho's guard task with
    // no guarded unit, its reference the point.
    if (const auto step = guard_engage(cmd, nullptr, cmd.target_pos, dt, ctx)) return *step;
    // A hovering aircraft with nothing to do flies as a winged one, hunting
    // round its point rather than hanging still (flies_winged_on_guard).
    if (flies_winged_on_guard(*this) && ctx.sim) {
        navigator_.abort_move();
        fly_winged_to(*this, cmd.target_pos, *ctx.sim, ctx.terrain, static_cast<f32>(dt));
        return OrderStep::Hold;
    }
    const f32 dx = cmd.target_pos.x - position().x;
    const f32 dz = cmd.target_pos.z - position().z;
    if (dx * dx + dz * dz > 2.0f * 2.0f) {
        walk_to(cmd.target_pos, dt, ctx);
    } else if (navigator_.is_moving()) {
        navigator_.abort_move();
    }
    return OrderStep::Hold;
}

OrderStep Unit::order_guard(UnitCommand& cmd, f64 dt, SimContext& ctx, f32 econ_eff) {
    auto& registry = ctx.registry;
    auto* L = ctx.L;
    if (cmd.target_id == 0) {
        end_guard_build(registry, L);
        return order_guard_point(cmd, dt, ctx);
    }
    auto* target = registry.find(cmd.target_id);
    if (!target || target->destroyed()) {
        end_guard_build(registry, L);
        release_navigator(); // the walk to it ends too
        command_queue_.pop_front();
        return OrderStep::Next;
    }
    if (!target->is_unit()) {
        end_guard_build(registry, L);
        release_navigator();
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
            // Its unit built, it rolls it off as a build of its own does.
            if (assist_rolloff_wait_ > 0) {
                if (!holds_for_rolloff(assist_rolloff_wait_)) factory_assist_build_ = false;
                return OrderStep::Hold;
            }
            const u32 built_id = build_target_id_;
            const u32 guarded_id = cmd.target_id; // cmd may go with the scripts' changes
            bool built = false;
            if (!progress_build(dt, registry, L, ctx.pathfinding_grid, econ_eff, &built)) {
                // Built for the guarded factory, the unit takes its rally
                // orders: Moho's guard task hands the build task that
                // factory as the one whose orders the unit takes.
                if (built) hand_over_rally_orders(built_id, guarded_id, ctx);
                if (built) assist_rolloff_wait_ = 2;
                else factory_assist_build_ = false; // failed: free again
            }
            return OrderStep::Hold;
        }
        // Its own builds and upgrades come first: Moho's guard task dispatches
        // them from the guarding factory's queue before the guarded one's.
        // Only the head order runs here, so the first of them moves ahead of
        // the guard and runs; the guard carries on when it is done. (Moho's
        // queue shows the guard first all the while.)
        for (size_t i = 1; i < command_queue_.size(); ++i) {
            const CommandType own = command_queue_[i].type;
            if (own != CommandType::BuildFactory && own != CommandType::Upgrade) continue;
            UnitCommand order = std::move(command_queue_[i]);
            command_queue_.erase(command_queue_.begin() + static_cast<std::ptrdiff_t>(i));
            // The erase may leave cmd (the guard) dangling: it is not used again.
            command_queue_.push_front(std::move(order));
            return OrderStep::Next;
        }
        if (!assist_pending_bp_.empty()) {
            if (waits_out_task(cmd) || waits_paused(cmd)) {
                return OrderStep::Hold;
            }
            UnitCommand build;
            build.type = CommandType::BuildFactory;
            build.blueprint_id = assist_pending_bp_;
            const u32 guard_id = cmd.command_id;
            const BuildStart started = start_build(build, registry, L);
            if (started == BuildStart::AtCap) {
                return hold_for_unit_cap(guard_id);
            }
            assist_pending_bp_.clear();
            factory_assist_build_ = started == BuildStart::Started;
            if (factory_assist_build_) {
                build_command_id_ = guard_id;
            }
            return OrderStep::Hold;
        }
        if (waits_out_task(cmd)) {
            return OrderStep::Hold;
        }
        const u32 guarded_factory = cmd.target_id;
        auto& queue = target_unit->command_queue_;
        const bool repeats = target_unit->repeat_queue_;
        for (size_t i = 0; i < queue.size() && L; ++i) {
            if (queue[i].type != CommandType::BuildFactory) continue;
            // Not the guarded factory's own build, unless it has more to make
            // than the one under way, or is all there is and the guarded
            // factory repeats (Moho's CUnitGuardTask).
            const bool spare = queue[i].count > 1;
            if (i == 0 && !spare && !(queue.size() == 1 && repeats)) continue;
            if (!blueprint_can_build(L, blueprint_id(), queue[i].blueprint_id)) continue;
            UnitCommand build;
            build.type = CommandType::BuildFactory;
            build.blueprint_id = queue[i].blueprint_id;
            const UnitCommand taken = queue[i];
            // One of an order with more is counted off it; else the order
            // is taken.
            if (spare) --queue[i].count;
            else queue.erase(queue.begin() + static_cast<std::ptrdiff_t>(i));
            // Taken even if the lobby's rules forbid it, as Moho's build task
            // fails after the take; such an order is dropped, repeating or
            // not, as the guarded factory drops it when it comes to it.
            if (build_blocked_by_lobby_rules(*this, build, ctx)) break;
            // A repeating guarded factory has the order sent round, its count
            // back at its most (Moho's MoveCommandToBackOfQueue)
            if (repeats && !spare) {
                UnitCommand round = taken;
                round.count = std::max(round.max_count, 1);
                queue.push_back(std::move(round));
            }
            if (waits_paused(cmd)) {
                assist_pending_bp_ = build.blueprint_id;
                break;
            }
            const u32 guard_id = cmd.command_id; // cmd may go with the scripts' changes
            const BuildStart started = start_build(build, registry, L);
            if (started == BuildStart::AtCap) {
                // At the army's unit cap the order goes back where it was
                // (Moho's build task holds it, waiting), and the factory
                // tries again later.
                auto* guarded = registry.find(guarded_factory);
                if (guarded && !guarded->destroyed() && guarded->is_unit()) {
                    auto& its = static_cast<Unit*>(guarded)->command_queue_;
                    if (spare) {
                        // Counted back onto its order, if it is still there
                        for (UnitCommand& c : its)
                            if (c.command_id == taken.command_id &&
                                c.type == CommandType::BuildFactory) {
                                ++c.count;
                                break;
                            }
                    } else {
                        if (repeats && !its.empty() && its.back().command_id == taken.command_id) {
                            its.pop_back();
                        }
                        its.insert(its.begin() +
                                       static_cast<std::ptrdiff_t>(std::min(i, its.size())),
                                   taken);
                    }
                }
                return hold_for_unit_cap(guard_id);
            }
            factory_assist_build_ = started == BuildStart::Started;
            if (factory_assist_build_) build_command_id_ = guard_id; // the guard's
            break;
        }
        return OrderStep::Hold;
    }

    const bool task_waits = waits_out_task(cmd);
    // Enemies near it come before helping (Moho's guard task looks for one
    // after factory assist, before build, reclaim and repair help).
    if (const auto step = guard_engage(cmd, target_unit, target_unit->position(), dt, ctx))
        return *step;

    // Help only within reach of the work (M206e): Moho's guard hands
    // it to a repair or reclaim task. A build, a silo or a repair
    // measures the gap to that unit's skirt (MaxBuildDistance to
    // begin, twice that to go on); a reclaim, to its footprint. Out
    // of reach, the unit walks just clear of the work, helping with
    // nothing meanwhile.
    // Where a hovering aircraft flying its guard winged makes for (the work
    // it closes on, else what it guards): its one flight this tick, below.
    std::optional<Vector3> winged_goal;
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
            // A hovering aircraft not yet at work flies winged to it (Moho:
            // guarding, neither Building nor Repairing yet), not by its
            // navigator.
            if (flies_winged_on_guard(*this) && ctx.sim) {
                winged_goal = goal;
                return false;
            }
            const Vector3 heading = navigator_.goal();
            if (!navigator_.is_moving() || std::abs(heading.x - goal.x) > 1.0f ||
                std::abs(heading.z - goal.z) > 1.0f) {
                set_path_goal(goal, ctx);
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
            if (is_building()) stop_assisting(ctx.L, &ctx.registry);
        } else {
            if (build_target_id_ != target_build_id) {
                // Switch to new assist target
                if (is_building()) stop_assisting(ctx.L, &ctx.registry);
                const Vector3 at = guarded_build->position();
                aim_builder_arms(&at, ctx.L);
                if (destroyed() || !in_registry()) {
                    return OrderStep::Gone;
                }
                if (!turns_to_face(at) && !awaits_arm() && !task_waits && !waits_paused(cmd)) {
                    build_target_id_ = target_build_id;
                    build_command_id_ = cmd.command_id;
                    build_released_with_order_ = true;
                    build_repairs_ =
                        target_unit->command_queue_.empty() ||
                        target_unit->command_queue_.front().type != CommandType::BuildMobile;
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
                    call_build_callback(ctx.L, "OnStartBuild", registry.find(target_build_id),
                                        build_repairs_ ? "Repair" : "MobileBuild");
                    if (destroyed() || !in_registry()) {
                        return OrderStep::Gone;
                    }
                }
            }

            // Progress the build with our own build rate
            if (build_target_id_ != 0) {
                if (!progress_build_assist(dt, registry, econ_eff)) {
                    stop_assisting(ctx.L, &ctx.registry);
                }
            }
        }
    } else if (guarded_reclaim && !guarded_reclaim->destroyed() && guarded_reclaim->reclaimable()) {
        // Assist reclaim: contribute reclaim power
        working = true;
        if (is_building()) stop_assisting(ctx.L, &ctx.registry);
        u32 target_reclaim_id = target_unit->reclaim_target_id();
        if (!within_reach(*guarded_reclaim, false, false)) {
            if (is_reclaiming()) stop_reclaiming(ctx.L, &ctx.registry);
        } else {
            if (reclaim_target_id_ != target_reclaim_id) {
                // Switch to new reclaim target
                if (is_reclaiming()) stop_reclaiming(ctx.L, &ctx.registry);

                begin_reclaim(target_reclaim_id, ctx.L, ctx.registry);
                if (destroyed() || !in_registry()) {
                    return OrderStep::Gone;
                }

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
                    stop_reclaiming(ctx.L, &ctx.registry);
                }
            }
        }
    } else if (repairs && target_unit->silo_building() && !target_unit->is_paused()) {
        // Assist a silo's missile: this unit's build power on it, at
        // its share of the cost (retail's UpdateConsumptionValues
        // for a SiloBuildingAmmo focus). A paused silo's helpers
        // wait, paying nothing; so do helpers out of reach.
        working = true;
        if (is_building()) stop_assisting(ctx.L, &ctx.registry);
        if (is_reclaiming()) stop_reclaiming(ctx.L, &ctx.registry);
        if (within_reach(*target_unit, true, false)) {
            assisting_silo_ = true;
            if (!paused_) {
                const SiloBuild& missile = target_unit->silo_build();
                const f64 per_second = static_cast<f64>(build_rate_) / missile.build_time;
                economy_.consumption_energy = missile.energy * per_second;
                economy_.consumption_mass = missile.mass * per_second;
                economy_.consumption_active = true;
                target_unit->assist_silo_build(build_rate_, dt, econ_eff);
            }
        }
    } else {
        // Target not building/reclaiming — stop if we were
        if (is_building()) stop_assisting(ctx.L, &ctx.registry);
        if (is_reclaiming()) stop_reclaiming(ctx.L, &ctx.registry);

        // Auto-repair: if target is damaged and we have build_rate
        if (repairs && (target_unit->health() < target_unit->max_health() ||
                        target_unit->shield_needs_repair(registry, L))) {
            working = true;
            if (!within_reach(*target_unit, true, repair_target_id_ == cmd.target_id)) {
                if (is_repairing()) stop_repairing(L, registry);
            } else {
                if (repair_target_id_ != cmd.target_id) {
                    if (is_repairing()) stop_repairing(L, registry);
                    const Vector3 at = target->position();
                    aim_builder_arms(&at, L);
                    if (destroyed() || !in_registry()) {
                        return OrderStep::Gone;
                    }
                    if (!turns_to_face(at) && !awaits_arm() && !task_waits && !waits_paused(cmd)) {
                        UnitCommand repair_cmd;
                        repair_cmd.type = CommandType::Repair;
                        repair_cmd.target_id = cmd.target_id;
                        repair_cmd.target_pos = target->position();
                        start_repair(repair_cmd, registry, L);
                    }
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
    // A hovering aircraft with nothing to do flies as a winged one round
    // what it guards (or the wreck or unit it reclaims or captures for it,
    // which don't make it hover), never hanging still (flies_winged_on_guard).
    if (flies_winged_on_guard(*this) && ctx.sim && !destroyed() && in_registry()) {
        const Entity* focus = is_reclaiming()  ? registry.find(reclaim_target_id_)
                              : is_capturing() ? registry.find(capture_target_id_)
                                               : nullptr;
        const Vector3 goal =
            winged_goal ? *winged_goal
            : focus && !focus->destroyed()
                ? focus->position()
                : approach_point(*this, target->position(), target_unit->skirt_size_x() * 0.5f,
                                 target_unit->skirt_size_z() * 0.5f);
        navigator_.abort_move();
        fly_winged_to(*this, goal, *ctx.sim, ctx.terrain, static_cast<f32>(dt));
        return OrderStep::Hold;
    }
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
                set_path_goal(goal, ctx);
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

namespace {

/// How long a transport waits at the pickup for its units (Moho's
/// CUnitLoadUnits).
constexpr i32 kPickupTimeoutTicks = 300;
/// A beam up's ticks (CUnitCallTransport).
constexpr i32 kBeamUpTicks = 10;

/// self:method(entity, number), for OnStartTransportBeamUp(transport, bone).
void call_with_entity_and_number(lua_State* L, const Unit& self, const char* method,
                                 const Entity& entity, f64 number) {
    if (!L || self.lua_table_ref() < 0) return;
    lua_rawgeti(L, LUA_REGISTRYINDEX, self.lua_table_ref());
    const int tbl = lua_gettop(L);
    lua_pushstring(L, method);
    lua_gettable(L, tbl);
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, tbl);
        if (entity.lua_table_ref() >= 0) lua_rawgeti(L, LUA_REGISTRYINDEX, entity.lua_table_ref());
        else lua_pushnil(L);
        lua_pushnumber(L, number);
        if (lua_pcall(L, 3, 0, 0) != 0) {
            spdlog::warn("{} error: {}", method, lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    } else {
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
}

/// The shorter-way blend of two orientations, `t` of the way from a to b.
Quaternion blend_orientation(const Quaternion& a, Quaternion b, f32 t) {
    if (a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w < 0) b = {-b.x, -b.y, -b.z, -b.w};
    Quaternion q{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t,
                 a.w + (b.w - a.w) * t};
    const f32 len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (len > 0) q = {q.x / len, q.y / len, q.z / len, q.w / len};
    return q;
}

bool loads_itself(const UnitCommand& cmd, u32 id) {
    return cmd.type == CommandType::TransportLoad && cmd.target_id == id;
}

} // namespace

bool Unit::calls_transport(u32 transport_id) const {
    return transport_id != entity_id() && !command_queue_.empty() &&
           loads_itself(command_queue_.front(), transport_id);
}

OrderStep Unit::order_transport_load(UnitCommand& cmd, f64 dt, SimContext& ctx) {
    // Moho dispatches Dock and TransportLoadUnits alike. At an air staging
    // platform the unit refuels (M206r); a carrier takes a load order's
    // units in to store them (M206s), and a Dock there refuels too. A Dock
    // at anything else ends.
    static const CategoryName kCarrier{"CARRIER"};
    if (cmd.target_id != entity_id()) {
        const Entity* e = ctx.registry.find(cmd.target_id);
        const auto* target =
            e && !e->destroyed() && e->is_unit() ? static_cast<const Unit*>(e) : nullptr;
        if (target && target->has_category(kCarrier))
            return cmd.type == CommandType::Dock ? order_refuel(cmd, dt, ctx)
                                                 : order_carrier_landing(cmd, dt, ctx);
        if (target && target->is_staging_platform()) return order_refuel(cmd, dt, ctx);
        if (cmd.type == CommandType::Dock) {
            set_unit_state("Refueling", false);
            command_queue_.pop_front();
            return OrderStep::Next;
        }
    }
    if (cmd.target_id != 0 && cmd.target_id == entity_id()) {
        if (cmd.type == CommandType::Dock) { // at itself: nothing to do
            command_queue_.pop_front();
            return OrderStep::Next;
        }
        return has_category(kCarrier) ? order_carrier_retrieve(cmd, ctx)
                                      : order_transport_pickup(cmd, dt, ctx);
    }
    return order_call_transport(cmd, dt, ctx);
}

OrderStep Unit::order_transport_pickup(UnitCommand& cmd, f64 dt, SimContext& ctx) {
    auto& registry = ctx.registry;
    auto* L = ctx.L;
    const u32 me = entity_id();
    // Ending ran scripts, which may have cleared the queue (and cmd with it)
    // or replaced it: the order goes only if it is still the head.
    const u32 order_id = cmd.command_id;
    const auto finish_order = [&] {
        if (!command_queue_.empty() && &command_queue_.front() == &cmd &&
            command_queue_.front().command_id == order_id)
            command_queue_.pop_front();
        return OrderStep::Next;
    };
    if (pickup_phase_ == PickupPhase::None) {
        set_unit_state("TransportLoading", true);
        call_lua_method(L, "OnStartTransportLoading");
        if (destroyed() || !in_registry()) return OrderStep::Gone;
        pickup_phase_ = PickupPhase::Holding;
        pickup_ticks_ = 0;
    }
    if (pickup_phase_ == PickupPhase::Holding) {
        // The units ordered aboard: the army's with a load order onto us in
        // their queue. Moho holds until each has it at its head.
        struct Candidate {
            Unit* unit;
            f32 metric;
            f32 d2;
        };
        std::vector<Candidate> wanted;
        bool holding = false;
        registry.for_each_unit([&](Entity& e) {
            if (e.destroyed() || !e.is_unit() || e.army() != army() || e.entity_id() == me) return;
            auto& u = static_cast<Unit&>(e);
            if (u.is_dying() || u.transport_id() != 0) return;
            const bool ordered =
                std::any_of(u.command_queue_.begin(), u.command_queue_.end(),
                            [&](const UnitCommand& c) { return loads_itself(c, me); });
            if (!ordered) return;
            if (!u.calls_transport(me)) {
                holding = true;
                return;
            }
            const f32 dx = position().x - u.position().x;
            const f32 dy = position().y - u.position().y;
            const f32 dz = position().z - u.position().z;
            wanted.push_back({&u, u.load_metric(), dx * dx + dy * dy + dz * dz});
        });
        if (holding) return OrderStep::Hold;
        // The largest first, the nearer on ties (then the older, so it is
        // the same everywhere).
        std::sort(wanted.begin(), wanted.end(), [](const Candidate& a, const Candidate& b) {
            if (a.metric != b.metric) return a.metric > b.metric;
            if (a.d2 != b.d2) return a.d2 < b.d2;
            return a.unit->entity_id() < b.unit->entity_id();
        });
        TransportSlots* slots = transport_slots();
        const bool slotted = slots && slots->has_points();
        i32 room = transport_capacity_ > 0
                       ? transport_capacity_ - static_cast<i32>(cargo_ids_.size())
                       : std::numeric_limits<i32>::max();
        Vector3 centre{0.0f, 0.0f, 0.0f};
        bool full = false;
        for (const Candidate& c : wanted) {
            const bool fits = slotted ? slots
                                            ->assign(c.unit->entity_id(), c.unit->transport_class(),
                                                     c.unit->transport_attach_bone())
                                            .has_value()
                                      : room-- > 0;
            if (!fits) {
                full = true;
                continue;
            }
            pickup_ids_.push_back(c.unit->entity_id());
            centre = {centre.x + c.unit->position().x, centre.y + c.unit->position().y,
                      centre.z + c.unit->position().z};
        }
        if (full) {
            call_lua_method(L, "OnTransportFull");
            if (destroyed() || !in_registry()) return OrderStep::Gone;
        }
        if (pickup_ids_.empty()) {
            finish_pickup(false, L);
            if (destroyed() || !in_registry()) return OrderStep::Gone;
            return finish_order();
        }
        const f32 inv = 1.0f / static_cast<f32>(pickup_ids_.size());
        pickup_center_ = {centre.x * inv, centre.y * inv, centre.z * inv};
        // It faces from where it is toward the centre (TransportAddPickupUnits).
        const f32 fx = pickup_center_.x - position().x;
        const f32 fz = pickup_center_.z - position().z;
        pickup_facing_ =
            fx * fx + fz * fz > 1e-6f ? euler_to_quat(dmath::atan2(fx, fz), 0, 0) : orientation();
        if (is_air_unit()) {
            call_lua_method(L, "OnTransportOrdered");
            if (destroyed() || !in_registry()) return OrderStep::Gone;
            pickup_phase_ = PickupPhase::Flying;
        } else {
            navigator_.abort_move();
            pickup_center_ = position();
            pickup_phase_ = PickupPhase::Waiting;
        }
    }
    if (pickup_phase_ == PickupPhase::Flying) {
        const Vector3 goal = navigator_.goal();
        if (!navigator_.is_moving() || std::abs(goal.x - pickup_center_.x) > 1.0f ||
            std::abs(goal.z - pickup_center_.z) > 1.0f) {
            set_path_goal(pickup_center_, ctx);
        }
        if (nav_update(dt, ctx.terrain)) return OrderStep::Hold;
        navigator_.abort_move();
        pickup_phase_ = PickupPhase::Landing;
    }
    // Over the centre, it comes down to its hover height; only then is it at
    // the pickup (Moho's move there is onto the land layer).
    const bool low = hover_low(dt, ctx);
    if (destroyed() || !in_registry()) return OrderStep::Gone;
    if (pickup_phase_ == PickupPhase::Landing) {
        if (!low) return OrderStep::Hold;
        pickup_phase_ = PickupPhase::Waiting;
    }
    // At the pickup: hover while the units come aboard, as long as the wait
    // allows.
    ++pickup_ticks_;
    std::erase_if(pickup_ids_, [&](u32 id) {
        const Entity* e = registry.find(id);
        const auto* u =
            e && !e->destroyed() && e->is_unit() ? static_cast<const Unit*>(e) : nullptr;
        if (u && u->transport_id() == me) return true;                   // aboard
        if (u && !u->is_dying() && u->calls_transport(me)) return false; // still coming
        if (transport_slots_) transport_slots_->release(id);
        return true;
    });
    if (!pickup_ids_.empty() && pickup_ticks_ <= kPickupTimeoutTicks) return OrderStep::Hold;
    finish_pickup(pickup_ticks_ <= kPickupTimeoutTicks, L);
    if (destroyed() || !in_registry()) return OrderStep::Gone;
    return finish_order();
}

void Unit::finish_pickup(bool completed, lua_State* L) {
    // Units that never came give their slots up (Moho removes them at the
    // timeout).
    if (transport_slots_)
        for (const u32 id : pickup_ids_) transport_slots_->release(id);
    pickup_ids_.clear();
    pickup_phase_ = PickupPhase::None;
    pickup_ticks_ = 0;
    call_lua_method(L, "OnStopTransportLoading");
    set_unit_state("TransportLoading", false);
    if (!completed && !destroyed() && in_registry()) call_lua_method(L, "OnTransportAborted");
}

void Unit::abandon_beam_up(const map::Terrain* terrain, lua_State* L) {
    beam_up_ticks_ = 0;
    stand_on_ground(terrain, position(), euler_to_quat(quat_yaw(orientation()), 0.0f, 0.0f));
    note_snap();
    call_lua_method(L, "OnStopTransportBeamUp");
}

void Unit::hold_altitude(f64 dt, const map::Terrain* terrain, f32 altitude) {
    if (!is_air_unit() || !terrain) return;
    const f32 climb = climb_rate_ * static_cast<f32>(dt);
    f32 alt = current_altitude_;
    if (alt < altitude) alt = std::min(alt + climb, altitude);
    else if (alt > altitude) alt = std::max(alt - climb, altitude);
    current_altitude_ = alt;
    current_airspeed_ = 0.0f;
    air_velocity_ = {};
    Vector3 at = position();
    at.y = air_floor(terrain, at.x, at.z) + alt;
    set_position(at);
}

bool Unit::hover_low(f64 dt, SimContext& ctx) {
    if (!is_air_unit()) {
        return true;
    }
    if (current_altitude_ > transport_hover_height_) {
        set_unit_state("MovingDown", true);
        set_vert_event("Down", ctx.L);
        if (destroyed() || !in_registry()) {
            return false;
        }
    }
    hold_altitude(dt, ctx.terrain, transport_hover_height_);
    if (current_altitude_ != transport_hover_height_) {
        return false;
    }
    set_unit_state("MovingDown", false);
    set_vert_event("Hover", ctx.L);
    return !destroyed() && in_registry();
}

OrderStep Unit::order_call_transport(UnitCommand& cmd, f64 dt, SimContext& ctx) {
    auto& registry = ctx.registry;
    auto* L = ctx.L;
    const u32 me = entity_id();
    // Scripts run on the way (the beam, the attach) may clear the queue (and
    // cmd with it) or replace it: the order goes only if it is still the head.
    const u32 order_id = cmd.command_id;
    const auto finish_order = [&] {
        if (!command_queue_.empty() && &command_queue_.front() == &cmd &&
            command_queue_.front().command_id == order_id)
            command_queue_.pop_front();
        return OrderStep::Next;
    };
    const auto end = [&] {
        if (beam_up_ticks_ > 0) {
            abandon_beam_up(ctx.terrain, L);
            if (destroyed() || !in_registry()) return OrderStep::Gone;
        }
        release_navigator(); // its walk to the transport ends with it
        return finish_order();
    };
    if (cmd.target_id == 0 || transport_id_ != 0) return end();
    Entity* target = registry.find(cmd.target_id);
    if (!target || target->destroyed() || !target->is_unit()) return end();
    auto* transport = static_cast<Unit*>(target);
    if (transport->is_dying()) return end();
    const u32 tid = transport->entity_id();

    // The transport runs the pickup once its head is the load order; one
    // without it queued is given it (Moho's command is the transport's too).
    // Once the unit has a slot, a transport no longer loading ends its order.
    const bool running = !transport->command_queue_.empty() &&
                         loads_itself(transport->command_queue_.front(), tid) &&
                         transport->pickup_phase_ != PickupPhase::None &&
                         transport->pickup_phase_ != PickupPhase::Holding;
    if (!running && cmd.started) return end();
    if (!running) {
        if (std::none_of(transport->command_queue_.begin(), transport->command_queue_.end(),
                         [&](const UnitCommand& c) { return loads_itself(c, tid); })) {
            UnitCommand load;
            load.type = CommandType::TransportLoad;
            load.target_id = tid;
            load.target_pos = transport->position();
            load.command_id = cmd.command_id;
            transport->command_queue_.push_back(load);
        }
        navigator_.abort_move();
        return OrderStep::Hold;
    }
    // A unit the transport gave no slot is left behind (TransportIsUnitAssignedForPickup).
    const auto& assigned = transport->pickup_ids_;
    if (std::find(assigned.begin(), assigned.end(), me) == assigned.end()) return end();
    cmd.started = true;

    const TransportSlots* slots = transport->built_transport_slots();
    const TransportSlots::Slot* slot = slots ? slots->slot_of(me) : nullptr;
    const Vector3 bone_at =
        slot ? transport->bone_world_position(slot->bone) : transport->position();

    if (beam_up_ticks_ > 0) {
        if (beam_up_ticks_ <= 1) {
            beam_up_ticks_ = 0;
            call_lua_method(L, "OnStopTransportBeamUp");
            if (destroyed() || !in_registry()) return OrderStep::Gone;
            if (transport->destroyed() || !transport->in_registry() || transport->is_dying()) {
                // The transport went while the unit rose: it comes back down.
                stand_on_ground(ctx.terrain, position(),
                                euler_to_quat(quat_yaw(orientation()), 0.0f, 0.0f));
                note_snap();
                return finish_order();
            }
            attach_to_transport(transport, registry, L);
            if (destroyed() || !in_registry()) return OrderStep::Gone;
            return finish_order();
        }
        // From where it stood to the bone, less its own height, easing in
        // and out (cos(t pi / 10) / 2 + 1/2 as t runs 10 to 2).
        const f32 blend =
            dmath::cos(static_cast<f32>(beam_up_ticks_) * 3.14159265f * 0.1f) * 0.5f + 0.5f;
        const Quaternion bone_facing =
            slot
                ? quat_multiply(transport->orientation(), transport->bone_pose(slot->bone).rotation)
                : transport->orientation();
        const Vector3 to{bone_at.x, bone_at.y - size_y_, bone_at.z};
        set_position({beam_from_.x + (to.x - beam_from_.x) * blend,
                      beam_from_.y + (to.y - beam_from_.y) * blend,
                      beam_from_.z + (to.z - beam_from_.z) * blend});
        set_orientation(blend_orientation(beam_from_orientation_, bone_facing, blend));
        --beam_up_ticks_;
        return OrderStep::Hold;
    }

    const auto walk_to = [&](const Vector3& at) {
        const Vector3 goal = navigator_.goal();
        if (!navigator_.is_moving() || std::abs(goal.x - at.x) > 1.0f ||
            std::abs(goal.z - at.z) > 1.0f) {
            set_path_goal(at, ctx);
        }
        nav_update(dt, ctx.terrain);
    };
    if (!transport->pickup_ready()) {
        // Wait about the centre, twice its bone's offset out
        // (TransportGetPickupUnitPos).
        Vector3 wait = transport->pickup_center_;
        if (slot && transport->bone_data()) {
            const Vector3 model = transport->bone_pose(slot->bone).position;
            const f32 scale = transport->bone_data()->model_scale;
            const Vector3 offset = quat_rotate(transport->pickup_facing_,
                                               {model.x * scale, model.y * scale, model.z * scale});
            wait = {wait.x + offset.x * 2.0f, wait.y, wait.z + offset.z * 2.0f};
        }
        walk_to(wait);
        return OrderStep::Hold;
    }
    // The transport is there: under the bone, and within twice its footprint
    // of it, beam up.
    const f32 reach = 2.0f * std::max(transport->footprint_size_x(), transport->footprint_size_z());
    const f32 dx = bone_at.x - position().x;
    const f32 dz = bone_at.z - position().z;
    if (dx * dx + dz * dz > reach * reach) {
        walk_to({bone_at.x, position().y, bone_at.z});
        return OrderStep::Hold;
    }
    navigator_.abort_move();
    ground_speed_ = 0;
    beam_from_ = position();
    beam_from_orientation_ = orientation();
    beam_up_ticks_ = kBeamUpTicks;
    call_with_entity_and_number(L, *this, "OnStartTransportBeamUp", *transport,
                                slot ? static_cast<f64>(slot->bone) : -1.0);
    if (destroyed() || !in_registry()) return OrderStep::Gone;
    return OrderStep::Hold;
}

OrderStep Unit::order_transport_unload(UnitCommand& cmd, f64 dt, SimContext& ctx) {
    // A carrier launches what it keeps in storage instead (M206q).
    if (cmd.launch_wait >= 0 || launches_on_unload()) return order_carrier_launch(cmd, ctx);
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
    if (is_staging_platform()) return order_staging_release(cmd, ctx);

    // Move to drop position
    constexpr f32 unload_range = 5.0f;
    f32 udx = cmd.target_pos.x - position().x;
    f32 udz = cmd.target_pos.z - position().z;
    f32 udist2 = udx * udx + udz * udz;
    if (udist2 > unload_range * unload_range) {
        set_unit_state("TransportUnloading", true);
        if (!navigator_.is_moving() || navigator_.goal().x != cmd.target_pos.x ||
            navigator_.goal().z != cmd.target_pos.z) {
            set_path_goal(cmd.target_pos, ctx);
        }
        // Flown, for an aircraft (the ground navigator had dragged transports
        // along the ground).
        nav_update(dt, ctx.terrain);
        return OrderStep::Hold;
    }
    navigator_.abort_move();
    set_unit_state("TransportUnloading", true);

    // Over the drop: down to its hover height, then its cargo is set down
    // where it fits. Scripts ran, which may have cleared the queue (and cmd
    // with it): the order goes only if it is still the head.
    const u32 order_id = cmd.command_id;
    const std::vector<u32> ids = cmd.unload_ids;
    if (!unload_step(dt, ctx, ids)) return OrderStep::Hold;
    if (destroyed() || !in_registry()) return OrderStep::Gone;
    set_unit_state("TransportUnloading", false);
    if (!command_queue_.empty() && &command_queue_.front() == &cmd &&
        command_queue_.front().command_id == order_id)
        command_queue_.pop_front();
    return OrderStep::Next;
}

OrderStep Unit::order_staging_release(UnitCommand& cmd, SimContext& ctx) {
    // A staging platform stays put (Moho's CUnitUnloadUnits for one): it lets
    // its aircraft go where they sit, and sends them to the drop point,
    // their queues cleared. Scripts ran: the order goes only if it is still
    // the head.
    const u32 order_id = cmd.command_id;
    std::vector<u32> released;
    for (const u32 id : cmd.unload_ids.empty() ? cargo_ids_ : cmd.unload_ids)
        if (std::find(cargo_ids_.begin(), cargo_ids_.end(), id) != cargo_ids_.end())
            released.push_back(id);
    UnitCommand move;
    move.type = CommandType::Move;
    move.target_pos = cmd.target_pos;
    detach_cargo(released, ctx.registry, ctx.L, ctx.terrain);
    if (destroyed() || !in_registry()) return OrderStep::Gone;
    for (const u32 id : released) {
        Entity* e = ctx.registry.find(id);
        if (!e || e->destroyed() || !e->is_unit()) continue;
        auto* unit = static_cast<Unit*>(e);
        if (unit->transport_id() == 0) unit->push_command(move, true);
    }
    set_unit_state("TransportUnloading", false);
    if (!command_queue_.empty() && &command_queue_.front() == &cmd &&
        command_queue_.front().command_id == order_id)
        command_queue_.pop_front();
    return OrderStep::Next;
}

bool Unit::unload_step(f64 dt, SimContext& ctx, const std::vector<u32>& ids) {
    if (is_air_unit() && vert_event_ != "Hover") {
        hover_low(dt, ctx);
        return false;
    }
    // Those whose footprint fits the ground where they hang; the rest stay
    // aboard (Moho's TransportDetachUnit asks FitsAt of an aircraft's cargo).
    std::vector<u32> down;
    for (const u32 id : ids.empty() ? cargo_ids_ : ids) {
        if (std::find(cargo_ids_.begin(), cargo_ids_.end(), id) == cargo_ids_.end()) continue;
        const Entity* e = ctx.registry.find(id);
        if (!e || e->destroyed() || !e->is_unit()) continue;
        const auto& cargo = static_cast<const Unit&>(*e);
        if (is_air_unit() && ctx.pathfinding_grid && !cargo.footprint_fits(*ctx.pathfinding_grid))
            continue;
        down.push_back(id);
    }
    detach_cargo(down, ctx.registry, ctx.L, ctx.terrain);
    return true;
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
        release_navigator(); // a back-off under way ends with it
        command_queue_.pop_front();
        return OrderStep::Next;
    }
    Vector3 at = cmd.target_pos;
    if (cmd.target_id != 0) {
        const Entity* target = registry.find(cmd.target_id);
        if (!target || target->destroyed()) {
            release_navigator();
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
            set_path_goal({at.x + ox * back, at.y, at.z + oz * back}, ctx);
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
            set_path_goal(at, ctx);
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

namespace {

/// What a script set on unit `u`'s table as `key` (a number), or 0.
f32 unit_lua_number(lua_State* L, const Unit& u, const char* key) {
    if (!L || u.lua_table_ref() < 0) return 0.0f;
    lua_rawgeti(L, LUA_REGISTRYINDEX, u.lua_table_ref());
    lua_pushstring(L, key);
    lua_gettable(L, -2);
    const f32 v = lua_type(L, -1) == LUA_TNUMBER ? static_cast<f32>(lua_tonumber(L, -1)) : 0.0f;
    lua_pop(L, 2);
    return v;
}

/// Moho's CostFraction: what share of `cost` a donation of `value` pays; a
/// half when the cost is unknown (the binary's fixed step).
f32 cost_fraction(f32 value, f32 cost) {
    return cost > 0.0f ? value / cost : 0.5f;
}

} // namespace

void Unit::donate_sacrifice(Unit& target, lua_State* L) {
    // Moho's CUnitSacrificeTask: this unit's own build cost times its
    // blueprint's SacrificeMassMult and SacrificeEnergyMult (both 0 by
    // default), and whichever resource buys less decides the step.
    const f32 mass = blueprint_economy_number(L, blueprint_id(), "BuildCostMass", 0.0f) *
                     blueprint_economy_number(L, blueprint_id(), "SacrificeMassMult", 0.0f);
    const f32 energy = blueprint_economy_number(L, blueprint_id(), "BuildCostEnergy", 0.0f) *
                       blueprint_economy_number(L, blueprint_id(), "SacrificeEnergyMult", 0.0f);
    if (target.is_enhancing()) {
        // An enhancing unit banks it in its script's work item.
        const f32 step =
            std::min(cost_fraction(mass, unit_lua_number(L, target, "WorkItemBuildCostMass")),
                     cost_fraction(energy, unit_lua_number(L, target, "WorkItemBuildCostEnergy")));
        target.set_work_progress(std::min(1.0f, target.work_progress() + step));
        if (L && target.lua_table_ref() >= 0) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, target.lua_table_ref());
            lua_pushstring(L, "WorkProgress");
            lua_pushnumber(L, target.work_progress());
            lua_rawset(L, -3);
            lua_pop(L, 1);
        }
        return;
    }
    const f32 step = std::min(
        cost_fraction(mass,
                      blueprint_economy_number(L, target.blueprint_id(), "BuildCostMass", 0.0f)),
        cost_fraction(energy,
                      blueprint_economy_number(L, target.blueprint_id(), "BuildCostEnergy", 0.0f)));
    if (step == 0.0f) return;
    materialize(target, step);
}

namespace {

bool sacrifice_upgrading(const Unit& u) {
    return u.has_unit_state("Upgrading") || u.is_enhancing() ||
           (!u.command_queue().empty() && u.command_queue().front().type == CommandType::Upgrade);
}

Unit* live_unit(EntityRegistry& registry, u32 id) {
    Entity* e = id != 0 ? registry.find(id) : nullptr;
    if (!e || e->destroyed() || !e->is_unit()) {
        return nullptr;
    }
    return static_cast<Unit*>(e);
}

} // namespace

void Unit::destroy_through_script(EntityRegistry& registry, lua_State* L) {
    const u32 id = entity_id();
    call_lua_method(L, "Destroy");
    Entity* self = registry.find(id);
    if (self && !self->destroyed()) {
        mark_destroyed();
        registry.unregister_entity(id);
    }
}

// Moho's IAiCommandDispatchImpl hands the order to a CUnitSacrificeTask only
// for these targets; faf-re CUnitSacrificeTask.cpp for the task.
OrderStep Unit::order_sacrifice(UnitCommand& cmd, f64 dt, SimContext& ctx) {
    auto& registry = ctx.registry;
    auto* L = ctx.L;
    const auto give_up = [&] {
        end_approach(cmd);
        command_queue_.pop_front();
        return OrderStep::Next;
    };
    if (!cmd.approached) {
        Unit* target = live_unit(registry, cmd.target_id);
        if (!target) {
            return give_up();
        }
        if (!target->is_being_built() && !sacrifice_upgrading(*target) &&
            target->health() >= target->max_health()) {
            Unit* focus =
                target->is_building() ? live_unit(registry, target->build_target_id()) : nullptr;
            if (!focus) {
                return give_up();
            }
            cmd.target_id = focus->entity_id();
        }
    }
    if (cmd.approached && !cmd.started && approach_update(dt, ctx)) {
        return OrderStep::Hold;
    }
    Unit* target = live_unit(registry, cmd.target_id);
    if (!target || target->layer() == "Air" || target->is_dying()) {
        if (cmd.started) {
            sacrifice_order_ = 0;
            destroy_through_script(registry, L);
            return OrderStep::Gone;
        }
        return give_up();
    }
    if (!cmd.approached) {
        Unit* go_to = target;
        Unit* creator =
            target->is_being_built() ? live_unit(registry, target->creator_id()) : nullptr;
        if (creator && creator->has_category("FACTORY")) {
            go_to = creator;
        } else if (sacrifice_upgrading(*target)) {
            if (Unit* next = live_unit(registry, target->build_target_id())) {
                cmd.target_id = next->entity_id();
                go_to = next;
            }
        }
        if (effective_speed() <= 0) {
            return give_up();
        }
        cmd.approached = true;
        set_path_goal(approach_point(*this, go_to->position(), go_to->skirt_size_x() * 0.5f,
                                    go_to->skirt_size_z() * 0.5f), ctx);
        return OrderStep::Hold;
    }
    if (!cmd.started) {
        if (work_gap(*this, target->position(), footprint_extent(*target)) > max_build_distance_) {
            return give_up();
        }
        if (target->is_enhancing() &&
            (target->is_paused() || std::max(target->economy().mass_requested(),
                                             target->economy().energy_requested()) <= 0.0)) {
            return give_up();
        }
        cmd.started = true;
        cmd.sacrifice_wait = 9;
        sacrifice_order_ = cmd.command_id;
        call_lua_method_with_entity(L, "OnStartSacrifice", target);
        return OrderStep::Hold;
    }
    if (--cmd.sacrifice_wait > 0) {
        return OrderStep::Hold;
    }
    sacrifice_order_ = 0;
    donate_sacrifice(*target, L);
    call_lua_method_with_entity(L, "OnStopSacrifice", target);
    if (!destroyed() && in_registry()) {
        destroy_through_script(registry, L);
    }
    return OrderStep::Gone;
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
        // Room is what the slots would still hold with those already
        // boarding aboard (M206l): a trial copy takes their slots, then each
        // new unit's that fits. A transport without attach points counts
        // its Class1Capacity (0: no limit).
        const TransportSlots* slots = transport_slots();
        std::optional<TransportSlots> trial;
        if (slots && slots->has_points()) {
            trial.emplace(*slots);
            registry.for_each_unit([&](const Entity& e) {
                if (e.destroyed() || !e.is_unit() || e.army() != army()) return;
                const auto& u = static_cast<const Unit&>(e);
                if (u.is_dying() || u.transport_id() != 0 || u.command_queue().empty()) return;
                const UnitCommand& wait = u.command_queue().front();
                if (wait.type == CommandType::WaitForFerry && wait.assigned_id == entity_id())
                    (void)trial->assign(u.entity_id(), u.transport_class(),
                                        u.transport_attach_bone());
            });
        }
        i32 room = trial ? std::numeric_limits<i32>::max()
                   : transport_capacity() > 0
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
                if (trial &&
                    !trial->assign(u.entity_id(), u.transport_class(), u.transport_attach_bone()))
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
            if (!ferry_fly(dt, ctx, point(ferry_index_), true)) ++ferry_index_;
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
        if (!unload_step(dt, ctx, {})) return OrderStep::Hold;
        if (destroyed() || !in_registry()) return OrderStep::Gone;
        ferry_phase_ = FerryPhase::Back;
        ferry_index_ = route - 1;
        return OrderStep::Hold;
    }
    // Back along the waypoints, then to the beacon to load again.
    if (ferry_index_ > 1) {
        if (!ferry_fly(dt, ctx, point(ferry_index_ - 1), true)) --ferry_index_;
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
        set_unit_state("WaitForFerry", false);
        release_navigator(); // its walk to the beacon ends too
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
                set_path_goal(ferry->position(), ctx);
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
        set_path_goal(beacon->position(), ctx);
    }
    if (!nav_update(dt, ctx.terrain) && effective_speed() > 0 &&
        navigator_.status() == Navigator::Status::Idle) {
        // As near as it gets.
        set_unit_state("WaitForFerry", true);
    }
    return OrderStep::Hold;
}

} // namespace osc::sim
