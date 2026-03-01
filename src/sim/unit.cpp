#include "sim/unit.hpp"
#include "sim/entity_registry.hpp"
#include "sim/sim_state.hpp"
#include "map/pathfinding_grid.hpp"

#include <algorithm>
#include <spdlog/spdlog.h>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

namespace osc::sim {

void Unit::add_weapon(std::unique_ptr<Weapon> w) {
    weapons_.push_back(std::move(w));
}

Weapon* Unit::get_weapon(i32 index) {
    if (index < 0 || index >= static_cast<i32>(weapons_.size()))
        return nullptr;
    return weapons_[index].get();
}

void Unit::push_command(const UnitCommand& cmd, bool clear_existing) {
    // Note: block_command_queue_ is a UI-only flag in FA's original engine.
    // Issue*() C++ functions always bypass it; only the player input handler
    // respects it.  We store the flag for IsUnitState queries but do NOT
    // block push_command here.
    if (clear_existing) {
        command_queue_.clear();
        navigator_.abort_move();
    }
    command_queue_.push_back(cmd);
}

void Unit::clear_commands() {
    command_queue_.clear();
    navigator_.abort_move();
}

void Unit::update(f64 dt, SimContext& ctx) {
    if (destroyed()) return;
    auto& registry = ctx.registry;
    auto* L = ctx.L;

    // Cargo position following: if loaded on a transport, skip all processing
    if (transport_id_ != 0) {
        auto* transport_entity = registry.find(transport_id_);
        if (transport_entity && !transport_entity->destroyed()) {
            set_position(transport_entity->position());
        } else {
            // Transport gone — auto-detach and clean up stale cargo entry
            if (transport_entity && transport_entity->is_unit()) {
                static_cast<Unit*>(transport_entity)->remove_cargo(entity_id());
            }
            transport_id_ = 0;
            set_unit_state("Attached", false);
        }
        return; // Skip commands and weapons while loaded
    }

    // Compute economy efficiency for this unit's army
    f32 econ_eff = 1.0f;
    if (army() >= 0 && static_cast<u32>(army()) < SimContext::MAX_EFFICIENCY_ARMIES) {
        const auto& ae = ctx.army_efficiency[static_cast<u32>(army())];
        econ_eff = static_cast<f32>(std::min(ae.mass, ae.energy));
    }

    // Process head command
    while (!command_queue_.empty()) {
        auto& cmd = command_queue_.front();

        switch (cmd.type) {
        case CommandType::Stop:
            navigator_.abort_move();
            command_queue_.pop_front();
            continue;

        case CommandType::Move:
            if (!navigator_.is_moving() ||
                navigator_.goal().x != cmd.target_pos.x ||
                navigator_.goal().z != cmd.target_pos.z) {
                navigator_.set_goal(cmd.target_pos, ctx.pathfinder, position(), layer_);
            }
            if (!navigator_.update(*this, effective_speed(), dt, ctx.terrain)) {
                command_queue_.pop_front();
                continue;
            }
            goto done_commands; // Still moving — break out of while

        case CommandType::Attack: {
            // Attack: move toward target if out of weapon range, else stop
            if (cmd.target_id == 0) {
                command_queue_.pop_front();
                continue;
            }
            auto* target = registry.find(cmd.target_id);
            if (!target || target->destroyed()) {
                command_queue_.pop_front();
                continue;
            }
            // Find max weapon range
            f32 best_range = 0;
            for (auto& w : weapons_) {
                if (w->enabled && !w->fire_on_death && !w->manual_fire)
                    best_range = std::max(best_range, w->max_range);
            }
            if (best_range <= 0) {
                command_queue_.pop_front();
                continue;
            }
            // Check distance to target
            f32 dx = target->position().x - position().x;
            f32 dz = target->position().z - position().z;
            f32 dist2 = dx * dx + dz * dz;
            f32 range2 = best_range * best_range;
            if (dist2 > range2) {
                // Move toward target
                if (!navigator_.is_moving() ||
                    navigator_.goal().x != target->position().x ||
                    navigator_.goal().z != target->position().z) {
                    navigator_.set_goal(target->position(), ctx.pathfinder, position(), layer_);
                }
                navigator_.update(*this, effective_speed(), dt, ctx.terrain);
            } else {
                navigator_.abort_move();
            }
            goto done_commands; // Stay on this command
        }

        case CommandType::BuildMobile: {
            if (build_target_id_ == 0) {
                // Phase 1: Move to build site
                f32 dx = cmd.target_pos.x - position().x;
                f32 dz = cmd.target_pos.z - position().z;
                f32 dist2 = dx * dx + dz * dz;
                f32 build_range = 6.0f;

                if (dist2 > build_range * build_range) {
                    if (!navigator_.is_moving() ||
                        navigator_.goal().x != cmd.target_pos.x ||
                        navigator_.goal().z != cmd.target_pos.z) {
                        navigator_.set_goal(cmd.target_pos, ctx.pathfinder, position(), layer_);
                    }
                    navigator_.update(*this, effective_speed(), dt, ctx.terrain);
                    goto done_commands;
                }
                navigator_.abort_move();

                // Phase 2: Spawn skeleton unit
                if (!start_build(cmd, registry, L)) {
                    command_queue_.pop_front();
                    continue;
                }
            }
            // Phase 3: Progress the build
            if (!progress_build(dt, registry, L, ctx.pathfinding_grid, econ_eff)) {
                command_queue_.pop_front();
                continue;
            }
            goto done_commands;
        }

        case CommandType::BuildFactory: {
            if (build_target_id_ == 0) {
                // Factory: spawn immediately at own position
                if (!start_build(cmd, registry, L)) {
                    command_queue_.pop_front();
                    continue;
                }
            }
            if (!progress_build(dt, registry, L, ctx.pathfinding_grid, econ_eff)) {
                command_queue_.pop_front();
                continue;
            }
            goto done_commands;
        }

        case CommandType::Patrol: {
            if (!navigator_.is_moving() ||
                navigator_.goal().x != cmd.target_pos.x ||
                navigator_.goal().z != cmd.target_pos.z) {
                navigator_.set_goal(cmd.target_pos, ctx.pathfinder, position(), layer_);
            }
            if (!navigator_.update(*this, effective_speed(), dt, ctx.terrain)) {
                // Reached patrol point — cycle to back of queue
                auto finished = cmd;
                command_queue_.pop_front();
                command_queue_.push_back(finished);
                continue;
            }
            goto done_commands;
        }

        case CommandType::Reclaim: {
            if (cmd.target_id == 0) {
                if (is_reclaiming()) stop_reclaiming();
                command_queue_.pop_front();
                continue;
            }
            auto* target = registry.find(cmd.target_id);
            if (!target || target->destroyed()) {
                if (is_reclaiming()) stop_reclaiming();
                command_queue_.pop_front();
                continue;
            }

            // Move to target
            constexpr f32 reclaim_range = 5.0f;
            f32 rdx = target->position().x - position().x;
            f32 rdz = target->position().z - position().z;
            f32 rdist2 = rdx * rdx + rdz * rdz;
            if (rdist2 > reclaim_range * reclaim_range) {
                if (!navigator_.is_moving() ||
                    navigator_.goal().x != target->position().x ||
                    navigator_.goal().z != target->position().z) {
                    navigator_.set_goal(target->position(), ctx.pathfinder, position(), layer_);
                }
                navigator_.update(*this, effective_speed(), dt, ctx.terrain);
                goto done_commands;
            }
            navigator_.abort_move();

            // Start reclaim if not already reclaiming this target
            if (reclaim_target_id_ != cmd.target_id) {
                if (is_reclaiming()) stop_reclaiming();

                // Read reclaim values from target's Lua table
                f64 max_mass = 0, max_energy = 0, time_mult = 1;
                if (target->lua_table_ref() >= 0) {
                    lua_rawgeti(L, LUA_REGISTRYINDEX, target->lua_table_ref());
                    int tbl = lua_gettop(L);

                    lua_pushstring(L, "MaxMassReclaim");
                    lua_rawget(L, tbl);
                    if (lua_isnumber(L, -1)) max_mass = lua_tonumber(L, -1);
                    lua_pop(L, 1);

                    lua_pushstring(L, "MaxEnergyReclaim");
                    lua_rawget(L, tbl);
                    if (lua_isnumber(L, -1)) max_energy = lua_tonumber(L, -1);
                    lua_pop(L, 1);

                    lua_pushstring(L, "TimeReclaim");
                    lua_rawget(L, tbl);
                    if (lua_isnumber(L, -1)) time_mult = lua_tonumber(L, -1);
                    lua_pop(L, 1);

                    lua_pop(L, 1); // tbl
                }

                f64 max_value = std::max(max_mass, max_energy);
                if (max_value <= 0 || build_rate_ <= 0) {
                    command_queue_.pop_front();
                    continue;
                }

                f64 reclaim_time = time_mult * max_value
                                   / static_cast<f64>(build_rate_) / 10.0;
                if (reclaim_time <= 0) reclaim_time = 0.01;

                reclaim_target_id_ = cmd.target_id;
                reclaim_rate_ = static_cast<f32>(1.0 / reclaim_time);

                // Set production rates (resources gained by reclaiming)
                economy_.production_mass =
                    max_mass * static_cast<f64>(reclaim_rate_);
                economy_.production_energy =
                    max_energy * static_cast<f64>(reclaim_rate_);
                economy_.production_active = true;

                spdlog::info("Reclaim start: entity #{} reclaiming #{} "
                             "(mass={:.0f}, energy={:.0f}, time={:.1f}s)",
                             entity_id(), cmd.target_id,
                             max_mass, max_energy, reclaim_time);
            }

            // Progress reclaim
            if (!progress_reclaim(dt, registry, L)) {
                command_queue_.pop_front();
                continue;
            }
            goto done_commands;
        }

        case CommandType::Upgrade: {
            // Upgrade: structure builds its replacement at own position
            // Reuses start_build/progress_build/finish_build with "Upgrade" order
            if (build_target_id_ == 0) {
                if (!start_build(cmd, registry, L)) {
                    command_queue_.pop_front();
                    continue;
                }
            }
            if (!progress_build(dt, registry, L, ctx.pathfinding_grid, econ_eff)) {
                command_queue_.pop_front();
                continue;
            }
            goto done_commands;
        }

        case CommandType::Repair: {
            if (cmd.target_id == 0) {
                if (is_repairing()) stop_repairing(L, registry);
                command_queue_.pop_front();
                continue;
            }
            auto* rtarget = registry.find(cmd.target_id);
            if (!rtarget || rtarget->destroyed() || !rtarget->is_unit()) {
                if (is_repairing()) stop_repairing(L, registry);
                command_queue_.pop_front();
                continue;
            }
            // Already at full health? Done.
            if (rtarget->health() >= rtarget->max_health()) {
                if (is_repairing()) stop_repairing(L, registry);
                command_queue_.pop_front();
                continue;
            }

            // Move to target if out of range
            constexpr f32 repair_range = 6.0f;
            f32 rdx = rtarget->position().x - position().x;
            f32 rdz = rtarget->position().z - position().z;
            f32 rdist2 = rdx * rdx + rdz * rdz;
            if (rdist2 > repair_range * repair_range) {
                if (!navigator_.is_moving() ||
                    navigator_.goal().x != rtarget->position().x ||
                    navigator_.goal().z != rtarget->position().z) {
                    navigator_.set_goal(rtarget->position(), ctx.pathfinder, position(), layer_);
                }
                navigator_.update(*this, effective_speed(), dt, ctx.terrain);
                goto done_commands;
            }
            navigator_.abort_move();

            // Start repair if not already repairing this target
            if (repair_target_id_ != cmd.target_id) {
                if (is_repairing()) stop_repairing(L, registry);
                if (!start_repair(cmd, registry, L)) {
                    command_queue_.pop_front();
                    continue;
                }
            }

            // Progress repair
            if (!progress_repair(dt, registry, L, econ_eff)) {
                command_queue_.pop_front();
                continue;
            }
            goto done_commands;
        }

        case CommandType::Capture: {
            if (cmd.target_id == 0) {
                if (is_capturing()) stop_capturing(L, registry, true);
                command_queue_.pop_front();
                continue;
            }
            auto* ctarget = registry.find(cmd.target_id);
            if (!ctarget || ctarget->destroyed() || !ctarget->is_unit()) {
                if (is_capturing()) stop_capturing(L, registry, true);
                command_queue_.pop_front();
                continue;
            }
            // Already same army (captured by someone else)
            if (ctarget->is_unit() &&
                static_cast<Unit*>(ctarget)->army() == army()) {
                if (is_capturing()) stop_capturing(L, registry, false);
                command_queue_.pop_front();
                continue;
            }

            // Move to target if out of range
            constexpr f32 capture_range = 6.0f;
            f32 cdx = ctarget->position().x - position().x;
            f32 cdz = ctarget->position().z - position().z;
            f32 cdist2 = cdx * cdx + cdz * cdz;
            if (cdist2 > capture_range * capture_range) {
                if (!navigator_.is_moving() ||
                    navigator_.goal().x != ctarget->position().x ||
                    navigator_.goal().z != ctarget->position().z) {
                    navigator_.set_goal(ctarget->position(), ctx.pathfinder, position(), layer_);
                }
                navigator_.update(*this, effective_speed(), dt, ctx.terrain);
                goto done_commands;
            }
            navigator_.abort_move();

            // Start capture if not already capturing this target
            if (capture_target_id_ != cmd.target_id) {
                if (is_capturing()) stop_capturing(L, registry, true);
                if (!start_capture(cmd, registry, L)) {
                    command_queue_.pop_front();
                    continue;
                }
            }

            // Progress capture
            if (!progress_capture(dt, registry, L, econ_eff)) {
                command_queue_.pop_front();
                continue;
            }
            goto done_commands;
        }

        case CommandType::Guard: {
            if (cmd.target_id == 0) {
                if (is_building()) stop_assisting();
                command_queue_.pop_front();
                continue;
            }
            auto* target = registry.find(cmd.target_id);
            if (!target || target->destroyed()) {
                if (is_building()) stop_assisting();
                command_queue_.pop_front();
                continue;
            }
            if (!target->is_unit()) {
                if (is_building()) stop_assisting();
                command_queue_.pop_front();
                continue;
            }
            auto* target_unit = static_cast<Unit*>(target);

            // Follow: stay within guard_range of the target
            constexpr f32 guard_range = 10.0f;
            f32 gdx = target->position().x - position().x;
            f32 gdz = target->position().z - position().z;
            f32 gdist2 = gdx * gdx + gdz * gdz;
            if (gdist2 > guard_range * guard_range) {
                if (!navigator_.is_moving() ||
                    navigator_.goal().x != target->position().x ||
                    navigator_.goal().z != target->position().z) {
                    navigator_.set_goal(target->position(), ctx.pathfinder, position(), layer_);
                }
                navigator_.update(*this, effective_speed(), dt, ctx.terrain);
            } else {
                navigator_.abort_move();
            }

            // Assist: if target is building, contribute build power
            if (target_unit->is_building()) {
                u32 target_build_id = target_unit->build_target_id();

                if (build_target_id_ != target_build_id) {
                    // Switch to new assist target
                    if (is_building()) stop_assisting();

                    auto* build_target = registry.find(target_build_id);
                    if (build_target && !build_target->destroyed()) {
                        build_target_id_ = target_build_id;
                        build_time_ = target_unit->build_time();
                        build_cost_mass_ = target_unit->build_cost_mass();
                        build_cost_energy_ = target_unit->build_cost_energy();
                        work_progress_ = build_target->fraction_complete();

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
                }

                // Progress the build with our own build rate
                if (build_target_id_ != 0) {
                    if (!progress_build_assist(dt, registry, econ_eff)) {
                        stop_assisting();
                    }
                }
            } else if (target_unit->is_reclaiming()) {
                // Assist reclaim: contribute reclaim power
                u32 target_reclaim_id = target_unit->reclaim_target_id();

                if (reclaim_target_id_ != target_reclaim_id) {
                    // Switch to new reclaim target
                    if (is_reclaiming()) stop_reclaiming();
                    if (is_building()) stop_assisting();

                    auto* reclaim_target = registry.find(target_reclaim_id);
                    if (reclaim_target && !reclaim_target->destroyed()) {
                        reclaim_target_id_ = target_reclaim_id;

                        // Compute own reclaim rate based on own build_rate
                        // (assister contributes speed but NOT duplicate resources
                        //  — only the primary reclaimer sets production rates)
                        f64 max_mass = 0, max_energy = 0, time_mult = 1.0;
                        if (reclaim_target->lua_table_ref() >= 0) {
                            lua_rawgeti(L, LUA_REGISTRYINDEX,
                                        reclaim_target->lua_table_ref());
                            int rtbl = lua_gettop(L);

                            lua_pushstring(L, "MaxMassReclaim");
                            lua_rawget(L, rtbl);
                            if (lua_isnumber(L, -1))
                                max_mass = lua_tonumber(L, -1);
                            lua_pop(L, 1);

                            lua_pushstring(L, "MaxEnergyReclaim");
                            lua_rawget(L, rtbl);
                            if (lua_isnumber(L, -1))
                                max_energy = lua_tonumber(L, -1);
                            lua_pop(L, 1);

                            lua_pushstring(L, "TimeReclaim");
                            lua_rawget(L, rtbl);
                            if (lua_isnumber(L, -1))
                                time_mult = lua_tonumber(L, -1);
                            lua_pop(L, 1);

                            lua_pop(L, 1); // rtbl
                        }

                        f64 max_value = std::max(max_mass, max_energy);
                        if (max_value > 0 && build_rate_ > 0) {
                            f64 reclaim_time = time_mult * max_value
                                / static_cast<f64>(build_rate_) / 10.0;
                            if (reclaim_time <= 0) reclaim_time = 0.01;
                            reclaim_rate_ = static_cast<f32>(1.0 / reclaim_time);
                        } else {
                            reclaim_rate_ = 0;
                        }

                        spdlog::info("Guard reclaim assist: entity #{} "
                                     "assisting #{} reclaiming #{}",
                                     entity_id(), cmd.target_id,
                                     target_reclaim_id);
                    }
                }

                if (reclaim_target_id_ != 0) {
                    if (!progress_reclaim_assist(dt, registry)) {
                        stop_reclaiming();
                    }
                }
            } else {
                // Target not building/reclaiming — stop if we were
                if (is_building()) stop_assisting();
                if (is_reclaiming()) stop_reclaiming();

                // Auto-repair: if target is damaged and we have build_rate
                if (target_unit->health() < target_unit->max_health() &&
                    build_rate_ > 0) {
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
                } else {
                    if (is_repairing()) stop_repairing(L, registry);
                }
            }

            goto done_commands; // Guard is persistent
        }

        case CommandType::Dive: {
            // Toggle submarine layer: Water ↔ Sub
            if (layer_ == "Water") {
                set_layer_with_callback("Sub", L);
                spdlog::debug("Unit #{} diving: Water → Sub", entity_id());
            } else if (layer_ == "Sub" || layer_ == "Seabed") {
                std::string from = layer_;
                set_layer_with_callback("Water", L);
                spdlog::debug("Unit #{} surfacing: {} → Water", entity_id(), from);
            }
            command_queue_.pop_front();
            continue;
        }

        case CommandType::Enhance: {
            if (!enhancing_) {
                if (!start_enhance(cmd, L)) {
                    command_queue_.pop_front();
                    continue;
                }
            }
            if (!progress_enhance(dt, L, econ_eff)) {
                command_queue_.pop_front();
                continue;
            }
            goto done_commands;
        }

        case CommandType::TransportLoad: {
            // Cargo unit loads into transport (target_id = transport entity)
            if (cmd.target_id == 0 || transport_id_ != 0) {
                set_unit_state("TransportLoading", false);
                command_queue_.pop_front();
                continue;
            }
            auto* target = registry.find(cmd.target_id);
            if (!target || target->destroyed() || !target->is_unit()) {
                set_unit_state("TransportLoading", false);
                command_queue_.pop_front();
                continue;
            }
            auto* transport = static_cast<Unit*>(target);

            // Check capacity before moving
            if (transport->transport_capacity() > 0 &&
                static_cast<i32>(transport->cargo_ids().size()) >=
                    transport->transport_capacity()) {
                set_unit_state("TransportLoading", false);
                command_queue_.pop_front();
                continue;
            }

            // Move toward transport
            set_unit_state("TransportLoading", true);
            constexpr f32 load_range = 5.0f;
            f32 ldx = transport->position().x - position().x;
            f32 ldz = transport->position().z - position().z;
            f32 ldist2 = ldx * ldx + ldz * ldz;
            if (ldist2 > load_range * load_range) {
                if (!navigator_.is_moving() ||
                    navigator_.goal().x != transport->position().x ||
                    navigator_.goal().z != transport->position().z) {
                    navigator_.set_goal(transport->position(), ctx.pathfinder,
                                        position(), layer_);
                }
                navigator_.update(*this, effective_speed(), dt, ctx.terrain);
                goto done_commands;
            }
            navigator_.abort_move();

            // Attach to transport
            attach_to_transport(transport, registry, L);
            set_unit_state("TransportLoading", false);
            command_queue_.pop_front();
            continue;
        }

        case CommandType::TransportUnload: {
            // Transport drops all cargo at target position
            if (cargo_ids_.empty()) {
                set_unit_state("TransportUnloading", false);
                command_queue_.pop_front();
                continue;
            }

            // Move to drop position
            constexpr f32 unload_range = 5.0f;
            f32 udx = cmd.target_pos.x - position().x;
            f32 udz = cmd.target_pos.z - position().z;
            f32 udist2 = udx * udx + udz * udz;
            if (udist2 > unload_range * unload_range) {
                set_unit_state("TransportUnloading", true);
                if (!navigator_.is_moving() ||
                    navigator_.goal().x != cmd.target_pos.x ||
                    navigator_.goal().z != cmd.target_pos.z) {
                    navigator_.set_goal(cmd.target_pos, ctx.pathfinder,
                                        position(), layer_);
                }
                navigator_.update(*this, effective_speed(), dt, ctx.terrain);
                goto done_commands;
            }
            navigator_.abort_move();

            // Detach all cargo
            detach_all_cargo(registry, L);
            set_unit_state("TransportUnloading", false);
            command_queue_.pop_front();
            continue;
        }

        default:
            command_queue_.pop_front();
            continue;
        }
    }
done_commands:

    // Update weapons (target scanning + firing)
    for (auto& weapon : weapons_) {
        weapon->update(dt, *this, registry, L);
    }
}

bool Unit::start_build(const UnitCommand& cmd, EntityRegistry& registry,
                       lua_State* L) {
    // Call __osc_create_building_unit from Lua registry
    lua_pushstring(L, "__osc_create_building_unit");
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (!lua_isfunction(L, -1)) {
        spdlog::warn("start_build: __osc_create_building_unit not registered");
        lua_pop(L, 1);
        return false;
    }

    lua_pushstring(L, cmd.blueprint_id.c_str());
    lua_pushnumber(L, army() + 1); // 1-based for Lua
    bool build_at_self = (cmd.type == CommandType::BuildFactory ||
                          cmd.type == CommandType::Upgrade);
    f32 bx = build_at_self ? position().x : cmd.target_pos.x;
    f32 bz = build_at_self ? position().z : cmd.target_pos.z;
    lua_pushnumber(L, bx);
    lua_pushnumber(L, 0); // y = 0 (terrain height not queried yet)
    lua_pushnumber(L, bz);

    if (lua_pcall(L, 5, 2, 0) != 0) {
        spdlog::warn("start_build pcall failed: {}", lua_tostring(L, -1));
        lua_pop(L, 1);
        return false;
    }

    // Returns: entity_id, lua_table
    if (lua_isnil(L, -2)) {
        lua_pop(L, 2);
        return false;
    }

    build_target_id_ = static_cast<u32>(lua_tonumber(L, -2));
    int target_tbl = lua_gettop(L); // target Lua table

    // Read BuildTime, BuildCostMass, BuildCostEnergy from target's blueprint
    auto* target = registry.find(build_target_id_);
    if (!target) {
        lua_pop(L, 2);
        build_target_id_ = 0;
        return false;
    }

    // Read economy data from the target blueprint via the __blueprints global
    lua_pushstring(L, "__blueprints");
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (lua_istable(L, -1)) {
        lua_pushstring(L, cmd.blueprint_id.c_str());
        lua_gettable(L, -2);
        if (lua_istable(L, -1)) {
            lua_pushstring(L, "Economy");
            lua_gettable(L, -2);
            if (lua_istable(L, -1)) {
                lua_pushstring(L, "BuildTime");
                lua_gettable(L, -2);
                if (lua_isnumber(L, -1))
                    build_time_ = lua_tonumber(L, -1);
                lua_pop(L, 1);

                lua_pushstring(L, "BuildCostMass");
                lua_gettable(L, -2);
                if (lua_isnumber(L, -1))
                    build_cost_mass_ = lua_tonumber(L, -1);
                lua_pop(L, 1);

                lua_pushstring(L, "BuildCostEnergy");
                lua_gettable(L, -2);
                if (lua_isnumber(L, -1))
                    build_cost_energy_ = lua_tonumber(L, -1);
                lua_pop(L, 1);
            }
            lua_pop(L, 1); // Economy
        }
        lua_pop(L, 1); // blueprint entry
    }
    lua_pop(L, 1); // __blueprints

    spdlog::info("start_build: entity #{} building {} (target #{}), "
                 "BuildTime={:.0f} BuildRate={:.1f} CostMass={:.0f} CostEnergy={:.0f}",
                 entity_id(), cmd.blueprint_id, build_target_id_,
                 build_time_, build_rate_, build_cost_mass_, build_cost_energy_);

    // Set economy drain on builder
    if (build_time_ > 0 && build_rate_ > 0) {
        economy_.consumption_mass =
            build_cost_mass_ * static_cast<f64>(build_rate_) / build_time_;
        economy_.consumption_energy =
            build_cost_energy_ * static_cast<f64>(build_rate_) / build_time_;
        economy_.consumption_active = true;
    }

    // Set UnitBeingBuilt and UnitBuildOrder on builder Lua table
    // (FactoryUnit.BuildingState.Main reads these)
    if (lua_table_ref() >= 0) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, lua_table_ref());
        int btbl = lua_gettop(L);
        lua_pushstring(L, "UnitBeingBuilt");
        lua_pushvalue(L, target_tbl);
        lua_rawset(L, btbl);
        lua_pushstring(L, "UnitBuildOrder");
        const char* build_order = "MobileBuild";
        if (cmd.type == CommandType::BuildFactory) build_order = "UnitBuild";
        else if (cmd.type == CommandType::Upgrade) build_order = "Upgrade";
        lua_pushstring(L, build_order);
        lua_rawset(L, btbl);
        lua_pop(L, 1); // btbl
    }

    // Call builder:OnStartBuild(target, order_type)
    const char* order_str = "UnitBuild";
    if (cmd.type == CommandType::BuildMobile) order_str = "MobileBuild";
    else if (cmd.type == CommandType::Upgrade) order_str = "Upgrade";
    if (lua_table_ref() >= 0) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, lua_table_ref());
        int builder_tbl = lua_gettop(L);
        lua_pushstring(L, "OnStartBuild");
        lua_gettable(L, builder_tbl);
        if (lua_isfunction(L, -1)) {
            lua_pushvalue(L, builder_tbl); // self
            lua_pushvalue(L, target_tbl);  // target
            lua_pushstring(L, order_str);
            if (lua_pcall(L, 3, 0, 0) != 0) {
                spdlog::warn("OnStartBuild error: {}", lua_tostring(L, -1));
                lua_pop(L, 1);
            }
        } else {
            lua_pop(L, 1);
        }
        lua_pop(L, 1); // builder_tbl
    }

    // Call target:OnStartBeingBuilt(builder, layer)
    lua_pushstring(L, "OnStartBeingBuilt");
    lua_gettable(L, target_tbl);
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, target_tbl); // self
        if (lua_table_ref() >= 0) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, lua_table_ref());
        } else {
            lua_pushnil(L);
        }
        lua_pushstring(L, layer().c_str());
        if (lua_pcall(L, 3, 0, 0) != 0) {
            spdlog::warn("OnStartBeingBuilt error: {}", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    } else {
        lua_pop(L, 1);
    }

    lua_pop(L, 2); // pop entity_id + target_tbl from create_building_unit
    work_progress_ = 0.0f;
    return true;
}

bool Unit::progress_build(f64 dt, EntityRegistry& registry, lua_State* L,
                          map::PathfindingGrid* grid, f32 efficiency) {
    auto* target = registry.find(build_target_id_);
    if (!target || target->destroyed()) {
        finish_build(registry, L, false, grid);
        return false;
    }

    // Guard: if an assister already pushed fraction to 1.0 this tick
    if (target->fraction_complete() >= 1.0f) {
        finish_build(registry, L, true, grid);
        return false;
    }

    if (build_time_ <= 0 || build_rate_ <= 0) {
        finish_build(registry, L, false, grid);
        return false;
    }

    f32 progress_rate = build_rate_ / static_cast<f32>(build_time_);
    f32 new_frac = std::min(1.0f, target->fraction_complete() +
                            progress_rate * static_cast<f32>(dt) * efficiency);
    target->set_fraction_complete(new_frac);
    target->set_health(new_frac * target->max_health());
    work_progress_ = new_frac;

    if (new_frac >= 1.0f) {
        finish_build(registry, L, true, grid);
        return false; // command done
    }
    return true; // still building
}

void Unit::finish_build(EntityRegistry& registry, lua_State* L, bool success,
                        map::PathfindingGrid* grid) {
    if (success && build_target_id_ != 0) {
        auto* target = registry.find(build_target_id_);
        if (target && target->is_unit()) {
            auto* target_unit = static_cast<Unit*>(target);
            target_unit->set_is_being_built(false);
            target_unit->set_fraction_complete(1.0f);
            target_unit->set_health(target_unit->max_health());

            spdlog::info("finish_build: entity #{} completed building target #{}",
                         entity_id(), build_target_id_);

            // Call target:OnStopBeingBuilt(builder, layer)
            if (target->lua_table_ref() >= 0) {
                lua_rawgeti(L, LUA_REGISTRYINDEX, target->lua_table_ref());
                int target_tbl = lua_gettop(L);
                lua_pushstring(L, "OnStopBeingBuilt");
                lua_gettable(L, target_tbl);
                if (lua_isfunction(L, -1)) {
                    lua_pushvalue(L, target_tbl); // self
                    if (lua_table_ref() >= 0) {
                        lua_rawgeti(L, LUA_REGISTRYINDEX, lua_table_ref());
                    } else {
                        lua_pushnil(L);
                    }
                    lua_pushstring(L, target_unit->layer().c_str());
                    if (lua_pcall(L, 3, 0, 0) != 0) {
                        spdlog::warn("OnStopBeingBuilt error: {}",
                                     lua_tostring(L, -1));
                        lua_pop(L, 1);
                    }
                } else {
                    lua_pop(L, 1);
                }
                lua_pop(L, 1); // target_tbl
            }
        }

        // Re-validate target after OnStopBeingBuilt callback
        // (Lua callback may have destroyed the entity)
        target = registry.find(build_target_id_);

        // Mark completed structure as obstacle on pathfinding grid
        // (after re-validation — only if target survived OnStopBeingBuilt)
        if (grid && target && !target->destroyed() && target->is_unit()) {
            auto* tu = static_cast<Unit*>(target);
            if (tu->has_category("STRUCTURE") && tu->footprint_size_x() > 0) {
                grid->mark_obstacle(
                    target->position().x, target->position().z,
                    tu->footprint_size_x(), tu->footprint_size_z());
            }
        }

        // Call builder:OnStopBuild(target)
        if (lua_table_ref() >= 0 && target && !target->destroyed() &&
            target->lua_table_ref() >= 0) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, lua_table_ref());
            int builder_tbl = lua_gettop(L);
            lua_pushstring(L, "OnStopBuild");
            lua_gettable(L, builder_tbl);
            if (lua_isfunction(L, -1)) {
                lua_pushvalue(L, builder_tbl);
                lua_rawgeti(L, LUA_REGISTRYINDEX, target->lua_table_ref());
                if (lua_pcall(L, 2, 0, 0) != 0) {
                    spdlog::warn("OnStopBuild error: {}",
                                 lua_tostring(L, -1));
                    lua_pop(L, 1);
                }
            } else {
                lua_pop(L, 1);
            }
            lua_pop(L, 1); // builder_tbl
        }
    } else if (build_target_id_ != 0) {
        spdlog::debug("finish_build: entity #{} failed/cancelled build of target #{}",
                      entity_id(), build_target_id_);

        // Call builder:OnFailedToBuild()
        if (lua_table_ref() >= 0) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, lua_table_ref());
            int builder_tbl = lua_gettop(L);
            lua_pushstring(L, "OnFailedToBuild");
            lua_gettable(L, builder_tbl);
            if (lua_isfunction(L, -1)) {
                lua_pushvalue(L, builder_tbl);
                if (lua_pcall(L, 1, 0, 0) != 0) {
                    spdlog::warn("OnFailedToBuild error: {}",
                                 lua_tostring(L, -1));
                    lua_pop(L, 1);
                }
            } else {
                lua_pop(L, 1);
            }
            lua_pop(L, 1); // builder_tbl
        }
    }

    // Clear UnitBeingBuilt and UnitBuildOrder on builder Lua table
    if (lua_table_ref() >= 0) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, lua_table_ref());
        int btbl = lua_gettop(L);
        lua_pushstring(L, "UnitBeingBuilt");
        lua_pushnil(L);
        lua_rawset(L, btbl);
        lua_pushstring(L, "UnitBuildOrder");
        lua_pushnil(L);
        lua_rawset(L, btbl);
        lua_pop(L, 1);
    }

    // Clear builder's economy drain
    economy_.consumption_mass = 0;
    economy_.consumption_energy = 0;
    economy_.consumption_active = false;

    build_target_id_ = 0;
    build_time_ = 0;
    build_cost_mass_ = 0;
    build_cost_energy_ = 0;
    work_progress_ = 0.0f;
}

void Unit::stop_assisting() {
    economy_.consumption_mass = 0;
    economy_.consumption_energy = 0;
    economy_.consumption_active = false;
    build_target_id_ = 0;
    build_time_ = 0;
    build_cost_mass_ = 0;
    build_cost_energy_ = 0;
    work_progress_ = 0.0f;
}

bool Unit::progress_build_assist(f64 dt, EntityRegistry& registry,
                                  f32 efficiency) {
    auto* target = registry.find(build_target_id_);
    if (!target || target->destroyed())
        return false;

    if (build_time_ <= 0 || build_rate_ <= 0)
        return false;

    if (target->fraction_complete() >= 1.0f)
        return false;

    f32 progress_rate = build_rate_ / static_cast<f32>(build_time_);
    f32 new_frac = std::min(1.0f,
        target->fraction_complete() + progress_rate * static_cast<f32>(dt) * efficiency);
    target->set_fraction_complete(new_frac);
    target->set_health(new_frac * target->max_health());
    work_progress_ = new_frac;

    return new_frac < 1.0f;
}

void Unit::stop_reclaiming() {
    // Only clear production rates if we were the primary reclaimer
    // (assisters don't set production rates, so nothing to clear)
    if (reclaim_target_id_ != 0 && economy_.production_active &&
        reclaim_rate_ > 0) {
        economy_.production_mass = 0;
        economy_.production_energy = 0;
        economy_.production_active = false;
    }
    reclaim_target_id_ = 0;
    reclaim_rate_ = 0;
}

/// Helper: call OnReclaimed on target, then ensure it's marked destroyed.
void Unit::call_on_reclaimed(u32 target_id, EntityRegistry& registry,
                              lua_State* L) {
    auto* target = registry.find(target_id);
    if (!target || target->destroyed()) return;

    if (target->lua_table_ref() >= 0 && lua_table_ref() >= 0) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, target->lua_table_ref());
        int target_tbl = lua_gettop(L);
        lua_pushstring(L, "OnReclaimed");
        lua_gettable(L, target_tbl);
        if (lua_isfunction(L, -1)) {
            lua_pushvalue(L, target_tbl); // self
            lua_rawgeti(L, LUA_REGISTRYINDEX, lua_table_ref());
            if (lua_pcall(L, 2, 0, 0) != 0) {
                spdlog::warn("OnReclaimed error: {}", lua_tostring(L, -1));
                lua_pop(L, 1);
            }
        } else {
            lua_pop(L, 1);
        }
        lua_pop(L, 1); // target_tbl
    }

    // Re-validate after pcall (Lua callback may have destroyed the entity)
    target = registry.find(target_id);
    if (target && !target->destroyed()) {
        target->mark_destroyed();
    }
}

bool Unit::progress_reclaim(f64 dt, EntityRegistry& registry, lua_State* L) {
    auto* target = registry.find(reclaim_target_id_);
    if (!target || target->destroyed()) {
        stop_reclaiming();
        return false;
    }

    if (reclaim_rate_ <= 0) {
        stop_reclaiming();
        return false;
    }

    // Already fully reclaimed (e.g., an assister finished it)
    if (target->fraction_complete() <= 0.0f) {
        u32 tid = reclaim_target_id_;
        call_on_reclaimed(tid, registry, L);
        spdlog::info("Reclaim complete: entity #{} finished reclaiming #{}",
                     entity_id(), tid);
        stop_reclaiming();
        return false;
    }

    f32 new_frac = std::max(0.0f,
        target->fraction_complete() - reclaim_rate_ * static_cast<f32>(dt));
    target->set_fraction_complete(new_frac);

    if (new_frac <= 0.0f) {
        u32 tid = reclaim_target_id_;
        call_on_reclaimed(tid, registry, L);
        spdlog::info("Reclaim complete: entity #{} finished reclaiming #{}",
                     entity_id(), tid);
        stop_reclaiming();
        return false;
    }

    return true; // still reclaiming
}

bool Unit::progress_reclaim_assist(f64 dt, EntityRegistry& registry) {
    auto* target = registry.find(reclaim_target_id_);
    if (!target || target->destroyed())
        return false;

    if (reclaim_rate_ <= 0)
        return false;

    if (target->fraction_complete() <= 0.0f)
        return false;

    f32 new_frac = std::max(0.0f,
        target->fraction_complete() - reclaim_rate_ * static_cast<f32>(dt));
    target->set_fraction_complete(new_frac);

    return new_frac > 0.0f;
}

bool Unit::start_repair(const UnitCommand& cmd, EntityRegistry& registry,
                        lua_State* L) {
    auto* target = registry.find(cmd.target_id);
    if (!target || target->destroyed() || !target->is_unit()) return false;
    auto* target_unit = static_cast<Unit*>(target);

    // Don't repair units that are being built (use build-assist instead)
    if (target_unit->is_being_built()) return false;
    // Don't repair units already at full health
    if (target->health() >= target->max_health()) return false;
    if (build_rate_ <= 0) return false;

    // Read BuildTime/BuildCostMass/BuildCostEnergy from target's blueprint
    repair_build_time_ = 0;
    repair_cost_mass_ = 0;
    repair_cost_energy_ = 0;

    lua_pushstring(L, "__blueprints");
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (lua_istable(L, -1)) {
        lua_pushstring(L, target_unit->unit_id().c_str());
        lua_gettable(L, -2);
        if (lua_istable(L, -1)) {
            lua_pushstring(L, "Economy");
            lua_gettable(L, -2);
            if (lua_istable(L, -1)) {
                lua_pushstring(L, "BuildTime");
                lua_gettable(L, -2);
                if (lua_isnumber(L, -1))
                    repair_build_time_ = lua_tonumber(L, -1);
                lua_pop(L, 1);

                lua_pushstring(L, "BuildCostMass");
                lua_gettable(L, -2);
                if (lua_isnumber(L, -1))
                    repair_cost_mass_ = lua_tonumber(L, -1);
                lua_pop(L, 1);

                lua_pushstring(L, "BuildCostEnergy");
                lua_gettable(L, -2);
                if (lua_isnumber(L, -1))
                    repair_cost_energy_ = lua_tonumber(L, -1);
                lua_pop(L, 1);
            }
            lua_pop(L, 1); // Economy
        }
        lua_pop(L, 1); // blueprint entry
    }
    lua_pop(L, 1); // __blueprints

    if (repair_build_time_ <= 0) return false;

    repair_target_id_ = cmd.target_id;

    // Set economy consumption (same formula as build)
    economy_.consumption_mass =
        repair_cost_mass_ * static_cast<f64>(build_rate_) / repair_build_time_;
    economy_.consumption_energy =
        repair_cost_energy_ * static_cast<f64>(build_rate_) / repair_build_time_;
    economy_.consumption_active = true;

    spdlog::info("start_repair: entity #{} repairing #{} "
                 "(BuildTime={:.0f} BuildRate={:.1f})",
                 entity_id(), cmd.target_id, repair_build_time_, build_rate_);

    // Call builder:OnStartBuild(target, "Repair")
    // FA Lua detects order=="Repair" and routes to OnStartRepair internally
    if (lua_table_ref() >= 0 && target->lua_table_ref() >= 0) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, lua_table_ref());
        int builder_tbl = lua_gettop(L);
        lua_pushstring(L, "OnStartBuild");
        lua_gettable(L, builder_tbl);
        if (lua_isfunction(L, -1)) {
            lua_pushvalue(L, builder_tbl); // self
            lua_rawgeti(L, LUA_REGISTRYINDEX, target->lua_table_ref());
            lua_pushstring(L, "Repair");
            if (lua_pcall(L, 3, 0, 0) != 0) {
                spdlog::warn("OnStartBuild(Repair) error: {}",
                             lua_tostring(L, -1));
                lua_pop(L, 1);
            }
        } else {
            lua_pop(L, 1);
        }
        lua_pop(L, 1); // builder_tbl
    }

    // Re-validate target after lua_pcall
    target = registry.find(repair_target_id_);
    if (!target || target->destroyed()) {
        stop_repairing(L, registry);
        return false;
    }

    return true;
}

bool Unit::progress_repair(f64 dt, EntityRegistry& registry, lua_State* L,
                            f32 efficiency) {
    auto* target = registry.find(repair_target_id_);
    if (!target || target->destroyed()) {
        stop_repairing(L, registry);
        return false;
    }

    if (repair_build_time_ <= 0 || build_rate_ <= 0) {
        stop_repairing(L, registry);
        return false;
    }

    // heal_per_tick = (build_rate / build_time) * max_health * dt * efficiency
    f32 heal_rate = build_rate_ / static_cast<f32>(repair_build_time_);
    f32 heal_amount = heal_rate * target->max_health() * static_cast<f32>(dt) * efficiency;
    f32 new_health = std::min(target->max_health(),
                              target->health() + heal_amount);
    target->set_health(new_health);

    if (new_health >= target->max_health()) {
        spdlog::info("repair complete: entity #{} finished repairing #{}",
                     entity_id(), repair_target_id_);
        stop_repairing(L, registry);
        return false; // repair done
    }
    return true; // still repairing
}

void Unit::stop_repairing(lua_State* L, EntityRegistry& registry) {
    u32 target_id = repair_target_id_;

    // Zero repair state BEFORE lua_pcall to prevent re-entrant double-callback
    repair_target_id_ = 0;
    repair_build_time_ = 0;
    repair_cost_mass_ = 0;
    repair_cost_energy_ = 0;

    // Clear economy drain
    economy_.consumption_mass = 0;
    economy_.consumption_energy = 0;
    economy_.consumption_active = false;

    // Call builder:OnStopBuild(target) — FA handles OnStopRepair inside
    if (target_id != 0 && lua_table_ref() >= 0) {
        auto* target = registry.find(target_id);
        if (target && !target->destroyed() && target->lua_table_ref() >= 0) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, lua_table_ref());
            int builder_tbl = lua_gettop(L);
            lua_pushstring(L, "OnStopBuild");
            lua_gettable(L, builder_tbl);
            if (lua_isfunction(L, -1)) {
                lua_pushvalue(L, builder_tbl);
                lua_rawgeti(L, LUA_REGISTRYINDEX, target->lua_table_ref());
                if (lua_pcall(L, 2, 0, 0) != 0) {
                    spdlog::warn("OnStopBuild(repair) error: {}",
                                 lua_tostring(L, -1));
                    lua_pop(L, 1);
                }
            } else {
                lua_pop(L, 1);
            }
            lua_pop(L, 1); // builder_tbl
        }
    }
}

bool Unit::start_capture(const UnitCommand& cmd, EntityRegistry& registry,
                         lua_State* L) {
    auto* target = registry.find(cmd.target_id);
    if (!target || target->destroyed() || !target->is_unit()) return false;
    auto* target_unit = static_cast<Unit*>(target);

    // Don't capture own or allied units, units being built, or uncapturable
    if (target_unit->army() == army()) return false;
    if (target_unit->is_being_built()) return false;
    if (!target_unit->capturable()) return false;
    if (build_rate_ <= 0) return false;

    // Read BuildTime and BuildCostEnergy from target's blueprint
    f64 build_time = 0;
    f64 build_cost_energy = 0;

    lua_pushstring(L, "__blueprints");
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (lua_istable(L, -1)) {
        lua_pushstring(L, target_unit->unit_id().c_str());
        lua_gettable(L, -2);
        if (lua_istable(L, -1)) {
            lua_pushstring(L, "Economy");
            lua_gettable(L, -2);
            if (lua_istable(L, -1)) {
                lua_pushstring(L, "BuildTime");
                lua_gettable(L, -2);
                if (lua_isnumber(L, -1))
                    build_time = lua_tonumber(L, -1);
                lua_pop(L, 1);

                lua_pushstring(L, "BuildCostEnergy");
                lua_gettable(L, -2);
                if (lua_isnumber(L, -1))
                    build_cost_energy = lua_tonumber(L, -1);
                lua_pop(L, 1);
            }
            lua_pop(L, 1); // Economy
        }
        lua_pop(L, 1); // blueprint entry
    }
    lua_pop(L, 1); // __blueprints

    if (build_time <= 0) return false;

    // Capture time = (BuildTime / BuildRate) / 2  (FA formula: half build time)
    capture_time_ = (build_time / static_cast<f64>(build_rate_)) / 2.0;
    if (capture_time_ <= 0) capture_time_ = 0.01;
    capture_energy_cost_ = build_cost_energy;
    capture_target_id_ = cmd.target_id;
    work_progress_ = 0.0f;

    // Set economy: energy-only drain (zero mass to clear any stale value)
    economy_.consumption_mass = 0;
    economy_.consumption_energy = capture_energy_cost_ / capture_time_;
    economy_.consumption_active = true;

    spdlog::info("start_capture: entity #{} capturing #{} "
                 "(BuildTime={:.0f} BuildRate={:.1f} captureTime={:.1f}s energy={:.0f})",
                 entity_id(), cmd.target_id, build_time, build_rate_,
                 capture_time_, capture_energy_cost_);

    // Call self:OnStartCapture(target)
    if (lua_table_ref() >= 0 && target->lua_table_ref() >= 0) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, lua_table_ref());
        int self_tbl = lua_gettop(L);
        lua_pushstring(L, "OnStartCapture");
        lua_gettable(L, self_tbl);
        if (lua_isfunction(L, -1)) {
            lua_pushvalue(L, self_tbl);
            lua_rawgeti(L, LUA_REGISTRYINDEX, target->lua_table_ref());
            if (lua_pcall(L, 2, 0, 0) != 0) {
                spdlog::warn("OnStartCapture error: {}", lua_tostring(L, -1));
                lua_pop(L, 1);
            }
        } else {
            lua_pop(L, 1);
        }
        lua_pop(L, 1); // self_tbl
    }

    // Re-validate target after pcall
    target = registry.find(capture_target_id_);
    if (!target || target->destroyed()) {
        stop_capturing(L, registry, true);
        return false;
    }

    // Call target:OnStartBeingCaptured(self)
    if (target->lua_table_ref() >= 0 && lua_table_ref() >= 0) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, target->lua_table_ref());
        int target_tbl = lua_gettop(L);
        lua_pushstring(L, "OnStartBeingCaptured");
        lua_gettable(L, target_tbl);
        if (lua_isfunction(L, -1)) {
            lua_pushvalue(L, target_tbl);
            lua_rawgeti(L, LUA_REGISTRYINDEX, lua_table_ref());
            if (lua_pcall(L, 2, 0, 0) != 0) {
                spdlog::warn("OnStartBeingCaptured error: {}",
                             lua_tostring(L, -1));
                lua_pop(L, 1);
            }
        } else {
            lua_pop(L, 1);
        }
        lua_pop(L, 1); // target_tbl
    }

    // Re-validate target after pcall
    target = registry.find(capture_target_id_);
    if (!target || target->destroyed()) {
        stop_capturing(L, registry, true);
        return false;
    }

    return true;
}

bool Unit::progress_capture(f64 dt, EntityRegistry& registry, lua_State* L,
                             f32 efficiency) {
    auto* target = registry.find(capture_target_id_);
    if (!target || target->destroyed()) {
        stop_capturing(L, registry, true);
        return false;
    }

    if (capture_time_ <= 0) {
        stop_capturing(L, registry, true);
        return false;
    }

    f32 progress_per_tick = static_cast<f32>(dt / capture_time_) * efficiency;
    work_progress_ = std::min(1.0f, work_progress_ + progress_per_tick);

    if (work_progress_ >= 1.0f) {
        // Capture complete
        u32 target_id = capture_target_id_;
        i32 target_old_army = -1;
        if (target->is_unit())
            target_old_army = static_cast<Unit*>(target)->army();

        // Zero capture state BEFORE lua_pcall (re-entrancy protection)
        capture_target_id_ = 0;
        capture_time_ = 0;
        capture_energy_cost_ = 0;
        economy_.consumption_mass = 0;
        economy_.consumption_energy = 0;
        economy_.consumption_active = false;
        work_progress_ = 0.0f;

        spdlog::info("capture complete: entity #{} captured #{}",
                     entity_id(), target_id);

        // Call self:OnStopCapture(target)
        if (lua_table_ref() >= 0 && target->lua_table_ref() >= 0) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, lua_table_ref());
            int self_tbl = lua_gettop(L);
            lua_pushstring(L, "OnStopCapture");
            lua_gettable(L, self_tbl);
            if (lua_isfunction(L, -1)) {
                lua_pushvalue(L, self_tbl);
                lua_rawgeti(L, LUA_REGISTRYINDEX, target->lua_table_ref());
                if (lua_pcall(L, 2, 0, 0) != 0) {
                    spdlog::warn("OnStopCapture error: {}",
                                 lua_tostring(L, -1));
                    lua_pop(L, 1);
                }
            } else {
                lua_pop(L, 1);
            }
            lua_pop(L, 1); // self_tbl
        }

        // Re-validate target
        target = registry.find(target_id);
        if (!target || target->destroyed()) return false;

        // Call target:OnCaptured(self) — FA Lua handles ownership transfer
        if (target->lua_table_ref() >= 0 && lua_table_ref() >= 0) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, target->lua_table_ref());
            int target_tbl = lua_gettop(L);
            lua_pushstring(L, "OnCaptured");
            lua_gettable(L, target_tbl);
            if (lua_isfunction(L, -1)) {
                lua_pushvalue(L, target_tbl);
                lua_rawgeti(L, LUA_REGISTRYINDEX, lua_table_ref());
                if (lua_pcall(L, 2, 0, 0) != 0) {
                    spdlog::warn("OnCaptured error: {}",
                                 lua_tostring(L, -1));
                    lua_pop(L, 1);
                }
            } else {
                lua_pop(L, 1);
            }
            lua_pop(L, 1); // target_tbl
        }

        // Re-validate target
        target = registry.find(target_id);
        if (!target || target->destroyed()) return false;

        // C++ fallback: if OnCaptured didn't change army, do it directly
        if (target->is_unit()) {
            auto* tu = static_cast<Unit*>(target);
            if (tu->army() == target_old_army) {
                spdlog::info("capture C++ fallback: transferring #{} "
                             "from army {} to army {}",
                             target_id, target_old_army, army());
                tu->set_army(army());
                // Update Army field on Lua table
                if (target->lua_table_ref() >= 0) {
                    lua_rawgeti(L, LUA_REGISTRYINDEX, target->lua_table_ref());
                    lua_pushstring(L, "Army");
                    lua_pushnumber(L, army() + 1); // 1-based for Lua
                    lua_rawset(L, -3);
                    lua_pop(L, 1);
                }
            }
        }

        return false; // capture done
    }

    return true; // still capturing
}

void Unit::stop_capturing(lua_State* L, EntityRegistry& registry, bool failed) {
    u32 target_id = capture_target_id_;

    // Zero capture state BEFORE lua_pcall (re-entrancy protection)
    capture_target_id_ = 0;
    capture_time_ = 0;
    capture_energy_cost_ = 0;

    // Clear economy drain
    economy_.consumption_mass = 0;
    economy_.consumption_energy = 0;
    economy_.consumption_active = false;
    work_progress_ = 0.0f;

    if (target_id == 0) return;

    auto* target = registry.find(target_id);
    if (!target || target->destroyed()) return;

    if (failed) {
        // Call self:OnFailedCapture(target)
        if (lua_table_ref() >= 0 && target->lua_table_ref() >= 0) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, lua_table_ref());
            int self_tbl = lua_gettop(L);
            lua_pushstring(L, "OnFailedCapture");
            lua_gettable(L, self_tbl);
            if (lua_isfunction(L, -1)) {
                lua_pushvalue(L, self_tbl);
                lua_rawgeti(L, LUA_REGISTRYINDEX, target->lua_table_ref());
                if (lua_pcall(L, 2, 0, 0) != 0) {
                    spdlog::warn("OnFailedCapture error: {}",
                                 lua_tostring(L, -1));
                    lua_pop(L, 1);
                }
            } else {
                lua_pop(L, 1);
            }
            lua_pop(L, 1); // self_tbl
        }

        // Re-validate target
        target = registry.find(target_id);
        if (!target || target->destroyed()) return;

        // Call target:OnFailedBeingCaptured(self)
        if (target->lua_table_ref() >= 0 && lua_table_ref() >= 0) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, target->lua_table_ref());
            int target_tbl = lua_gettop(L);
            lua_pushstring(L, "OnFailedBeingCaptured");
            lua_gettable(L, target_tbl);
            if (lua_isfunction(L, -1)) {
                lua_pushvalue(L, target_tbl);
                lua_rawgeti(L, LUA_REGISTRYINDEX, lua_table_ref());
                if (lua_pcall(L, 2, 0, 0) != 0) {
                    spdlog::warn("OnFailedBeingCaptured error: {}",
                                 lua_tostring(L, -1));
                    lua_pop(L, 1);
                }
            } else {
                lua_pop(L, 1);
            }
            lua_pop(L, 1); // target_tbl
        }
    } else {
        // Normal stop/interrupt
        // Call self:OnStopCapture(target)
        if (lua_table_ref() >= 0 && target->lua_table_ref() >= 0) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, lua_table_ref());
            int self_tbl = lua_gettop(L);
            lua_pushstring(L, "OnStopCapture");
            lua_gettable(L, self_tbl);
            if (lua_isfunction(L, -1)) {
                lua_pushvalue(L, self_tbl);
                lua_rawgeti(L, LUA_REGISTRYINDEX, target->lua_table_ref());
                if (lua_pcall(L, 2, 0, 0) != 0) {
                    spdlog::warn("OnStopCapture error: {}",
                                 lua_tostring(L, -1));
                    lua_pop(L, 1);
                }
            } else {
                lua_pop(L, 1);
            }
            lua_pop(L, 1); // self_tbl
        }

        // Re-validate target
        target = registry.find(target_id);
        if (!target || target->destroyed()) return;

        // Call target:OnStopBeingCaptured(self)
        if (target->lua_table_ref() >= 0 && lua_table_ref() >= 0) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, target->lua_table_ref());
            int target_tbl = lua_gettop(L);
            lua_pushstring(L, "OnStopBeingCaptured");
            lua_gettable(L, target_tbl);
            if (lua_isfunction(L, -1)) {
                lua_pushvalue(L, target_tbl);
                lua_rawgeti(L, LUA_REGISTRYINDEX, lua_table_ref());
                if (lua_pcall(L, 2, 0, 0) != 0) {
                    spdlog::warn("OnStopBeingCaptured error: {}",
                                 lua_tostring(L, -1));
                    lua_pop(L, 1);
                }
            } else {
                lua_pop(L, 1);
            }
            lua_pop(L, 1); // target_tbl
        }
    }
}

// --- Enhancement system ---

bool Unit::has_enhancement(const std::string& enh) const {
    for (auto& [slot, name] : enhancements_) {
        if (name == enh) return true;
    }
    return false;
}

void Unit::add_enhancement(const std::string& slot, const std::string& enh) {
    enhancements_[slot] = enh;
}

void Unit::remove_enhancement(const std::string& enh) {
    for (auto it = enhancements_.begin(); it != enhancements_.end(); ++it) {
        if (it->second == enh) {
            enhancements_.erase(it);
            return;
        }
    }
}

bool Unit::start_enhance(const UnitCommand& cmd, lua_State* L) {
    enhance_name_ = cmd.blueprint_id;

    // Read enhancement BP from self.Blueprint.Enhancements[name]
    f64 enh_build_time = 0, enh_cost_mass = 0, enh_cost_energy = 0;

    if (lua_table_ref() >= 0) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, lua_table_ref());
        int self_tbl = lua_gettop(L);

        lua_pushstring(L, "Blueprint");
        lua_rawget(L, self_tbl);
        if (lua_istable(L, -1)) {
            lua_pushstring(L, "Enhancements");
            lua_gettable(L, -2);
            if (lua_istable(L, -1)) {
                lua_pushstring(L, enhance_name_.c_str());
                lua_gettable(L, -2);
                if (lua_istable(L, -1)) {
                    lua_pushstring(L, "BuildTime");
                    lua_gettable(L, -2);
                    if (lua_isnumber(L, -1)) enh_build_time = lua_tonumber(L, -1);
                    lua_pop(L, 1);

                    lua_pushstring(L, "BuildCostMass");
                    lua_gettable(L, -2);
                    if (lua_isnumber(L, -1)) enh_cost_mass = lua_tonumber(L, -1);
                    lua_pop(L, 1);

                    lua_pushstring(L, "BuildCostEnergy");
                    lua_gettable(L, -2);
                    if (lua_isnumber(L, -1)) enh_cost_energy = lua_tonumber(L, -1);
                    lua_pop(L, 1);
                }
                lua_pop(L, 1); // enh entry
            }
            lua_pop(L, 1); // Enhancements
        }
        lua_pop(L, 1); // Blueprint
        lua_pop(L, 1); // self_tbl
    }

    if (enh_build_time <= 0 || build_rate_ <= 0) {
        spdlog::warn("start_enhance: invalid BuildTime ({}) or BuildRate ({}) "
                     "for enhancement '{}' on entity #{}",
                     enh_build_time, build_rate_, enhance_name_, entity_id());
        enhance_name_.clear();
        return false;
    }

    enhance_build_time_ = enh_build_time;
    work_progress_ = 0.0f;

    // Set economy drain
    economy_.consumption_mass =
        enh_cost_mass * static_cast<f64>(build_rate_) / enhance_build_time_;
    economy_.consumption_energy =
        enh_cost_energy * static_cast<f64>(build_rate_) / enhance_build_time_;
    economy_.consumption_active = true;

    // Call self:OnWorkBegin(enhancement_name)
    if (lua_table_ref() >= 0) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, lua_table_ref());
        int self_tbl = lua_gettop(L);
        lua_pushstring(L, "OnWorkBegin");
        lua_gettable(L, self_tbl);
        if (lua_isfunction(L, -1)) {
            lua_pushvalue(L, self_tbl); // self
            lua_pushstring(L, enhance_name_.c_str());
            if (lua_pcall(L, 2, 0, 0) != 0) {
                spdlog::warn("OnWorkBegin error: {}", lua_tostring(L, -1));
                lua_pop(L, 1);
                lua_pop(L, 1); // self_tbl
                // Cancel on error
                economy_.consumption_mass = 0;
                economy_.consumption_energy = 0;
                economy_.consumption_active = false;
                enhance_build_time_ = 0;
                enhance_name_.clear();
                return false;
            }
        } else {
            lua_pop(L, 1); // non-function
        }
        lua_pop(L, 1); // self_tbl
    }

    enhancing_ = true;
    spdlog::info("start_enhance: entity #{} enhancing '{}' "
                 "(BuildTime={:.0f} BuildRate={:.1f} CostMass={:.0f} CostEnergy={:.0f})",
                 entity_id(), enhance_name_, enhance_build_time_, build_rate_,
                 enh_cost_mass, enh_cost_energy);
    return true;
}

bool Unit::progress_enhance(f64 dt, lua_State* L, f32 efficiency) {
    if (enhance_build_time_ <= 0 || build_rate_ <= 0) {
        cancel_enhance(L);
        return false;
    }

    work_progress_ = std::min(1.0f, work_progress_ + static_cast<f32>(
        static_cast<f64>(build_rate_) / enhance_build_time_ * dt) * efficiency);

    // Update WorkProgress on Lua table
    if (lua_table_ref() >= 0) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, lua_table_ref());
        lua_pushstring(L, "WorkProgress");
        lua_pushnumber(L, work_progress_);
        lua_rawset(L, -3);
        lua_pop(L, 1);
    }

    if (work_progress_ >= 1.0f) {
        finish_enhance(L);
        return false; // done
    }
    return true; // still in progress
}

void Unit::finish_enhance(lua_State* L) {
    work_progress_ = 1.0f;

    // Clear economy drain
    economy_.consumption_mass = 0;
    economy_.consumption_energy = 0;
    economy_.consumption_active = false;

    // Call self:OnWorkEnd(enhancement_name)
    if (lua_table_ref() >= 0) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, lua_table_ref());
        int self_tbl = lua_gettop(L);
        lua_pushstring(L, "OnWorkEnd");
        lua_gettable(L, self_tbl);
        if (lua_isfunction(L, -1)) {
            lua_pushvalue(L, self_tbl); // self
            lua_pushstring(L, enhance_name_.c_str());
            if (lua_pcall(L, 2, 0, 0) != 0) {
                spdlog::warn("OnWorkEnd error: {}", lua_tostring(L, -1));
                lua_pop(L, 1);
            }
        } else {
            lua_pop(L, 1);
        }
        lua_pop(L, 1); // self_tbl
    }

    // Also add to C++ enhancement map by reading Slot from blueprint
    if (lua_table_ref() >= 0) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, lua_table_ref());
        int self_tbl = lua_gettop(L);
        lua_pushstring(L, "Blueprint");
        lua_rawget(L, self_tbl);
        if (lua_istable(L, -1)) {
            lua_pushstring(L, "Enhancements");
            lua_gettable(L, -2);
            if (lua_istable(L, -1)) {
                lua_pushstring(L, enhance_name_.c_str());
                lua_gettable(L, -2);
                if (lua_istable(L, -1)) {
                    lua_pushstring(L, "Slot");
                    lua_gettable(L, -2);
                    if (lua_type(L, -1) == LUA_TSTRING) {
                        std::string slot = lua_tostring(L, -1);
                        enhancements_[slot] = enhance_name_;
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

    spdlog::info("finish_enhance: entity #{} completed enhancement '{}'",
                 entity_id(), enhance_name_);
    enhancing_ = false;
    enhance_build_time_ = 0;
    work_progress_ = 0.0f;
    enhance_name_.clear();
}

void Unit::cancel_enhance(lua_State* L) {
    // Call self:OnWorkFail(enhancement_name)
    if (lua_table_ref() >= 0 && !enhance_name_.empty()) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, lua_table_ref());
        int self_tbl = lua_gettop(L);
        lua_pushstring(L, "OnWorkFail");
        lua_gettable(L, self_tbl);
        if (lua_isfunction(L, -1)) {
            lua_pushvalue(L, self_tbl);
            lua_pushstring(L, enhance_name_.c_str());
            if (lua_pcall(L, 2, 0, 0) != 0) {
                spdlog::warn("OnWorkFail error: {}", lua_tostring(L, -1));
                lua_pop(L, 1);
            }
        } else {
            lua_pop(L, 1);
        }
        lua_pop(L, 1); // self_tbl
    }

    // Clear economy drain
    economy_.consumption_mass = 0;
    economy_.consumption_energy = 0;
    economy_.consumption_active = false;

    enhancing_ = false;
    enhance_build_time_ = 0;
    work_progress_ = 0.0f;
    enhance_name_.clear();
}

// ---------------------------------------------------------------------------
// Intel system
// ---------------------------------------------------------------------------

bool Unit::is_intel_enabled(const std::string& type) const {
    auto it = intel_states_.find(type);
    return it != intel_states_.end() && it->second.enabled;
}

f32 Unit::get_intel_radius(const std::string& type) const {
    auto it = intel_states_.find(type);
    return it != intel_states_.end() ? it->second.radius : 0.0f;
}

void Unit::init_intel(const std::string& type, f32 radius) {
    intel_states_[type] = IntelState{radius, true};
}

void Unit::enable_intel(const std::string& type) {
    intel_states_[type].enabled = true;
}

void Unit::disable_intel(const std::string& type) {
    auto it = intel_states_.find(type);
    if (it != intel_states_.end())
        it->second.enabled = false;
}

void Unit::set_intel_radius(const std::string& type, f32 radius) {
    intel_states_[type].radius = radius;
}

// ---------------------------------------------------------------------------
// Transport system
// ---------------------------------------------------------------------------

void Unit::remove_cargo(u32 id) {
    cargo_ids_.erase(std::remove(cargo_ids_.begin(), cargo_ids_.end(), id),
                     cargo_ids_.end());
}

void Unit::attach_to_transport(Unit* transport, EntityRegistry& registry,
                               lua_State* L) {
    // Capacity guard — reject if transport is full
    if (transport->transport_capacity() > 0 &&
        static_cast<i32>(transport->cargo_ids().size()) >=
            transport->transport_capacity()) {
        spdlog::warn("Transport #{} is full (capacity {}), cannot attach #{}",
                     transport->entity_id(), transport->transport_capacity(),
                     entity_id());
        return;
    }

    transport_id_ = transport->entity_id();
    transport->add_cargo(entity_id());
    set_unit_state("Attached", true);
    navigator_.abort_move();
    set_position(transport->position());

    spdlog::info("Transport: entity #{} loaded onto transport #{}",
                 entity_id(), transport->entity_id());

    // Lua callback: transport:OnTransportAttach(bone, cargo)
    // FA Lua chains to cargo:OnAttachedToTransport internally
    if (transport->lua_table_ref() >= 0 && lua_table_ref() >= 0) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, transport->lua_table_ref());
        int transport_tbl = lua_gettop(L);
        lua_pushstring(L, "OnTransportAttach");
        lua_gettable(L, transport_tbl);
        if (lua_isfunction(L, -1)) {
            lua_pushvalue(L, transport_tbl); // self (transport)
            lua_pushstring(L, "Attachpoint");  // bone placeholder
            lua_rawgeti(L, LUA_REGISTRYINDEX, lua_table_ref()); // cargo
            if (lua_pcall(L, 3, 0, 0) != 0) {
                spdlog::warn("OnTransportAttach error: {}",
                             lua_tostring(L, -1));
                lua_pop(L, 1);
            }
        } else {
            lua_pop(L, 1); // non-function
        }
        lua_pop(L, 1); // transport_tbl
    }
}

void Unit::detach_all_cargo(EntityRegistry& registry, lua_State* L) {
    // Snapshot IDs (safety against modification during Lua callbacks)
    std::vector<u32> snapshot = cargo_ids_;
    cargo_ids_.clear();

    for (u32 cargo_id : snapshot) {
        auto* cargo_entity = registry.find(cargo_id);
        if (!cargo_entity || cargo_entity->destroyed() ||
            !cargo_entity->is_unit())
            continue;
        auto* cargo = static_cast<Unit*>(cargo_entity);

        cargo->set_transport_id(0);
        cargo->set_unit_state("Attached", false);
        cargo->set_position(position()); // drop at transport position

        spdlog::info("Transport: entity #{} unloaded from transport #{}",
                     cargo_id, entity_id());

        // Lua callback: transport:OnTransportDetach(bone, cargo)
        // FA Lua chains to cargo:OnDetachedFromTransport internally
        if (lua_table_ref() >= 0 && cargo->lua_table_ref() >= 0) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, lua_table_ref());
            int transport_tbl = lua_gettop(L);
            lua_pushstring(L, "OnTransportDetach");
            lua_gettable(L, transport_tbl);
            if (lua_isfunction(L, -1)) {
                lua_pushvalue(L, transport_tbl); // self (transport)
                lua_pushstring(L, "Attachpoint");  // bone placeholder
                lua_rawgeti(L, LUA_REGISTRYINDEX, cargo->lua_table_ref());
                if (lua_pcall(L, 3, 0, 0) != 0) {
                    spdlog::warn("OnTransportDetach error: {}",
                                 lua_tostring(L, -1));
                    lua_pop(L, 1);
                }
            } else {
                lua_pop(L, 1); // non-function
            }
            lua_pop(L, 1); // transport_tbl
        }
    }
}

void Unit::set_layer_with_callback(const std::string& new_layer, lua_State* L) {
    std::string old_layer = layer_;
    set_layer(new_layer);

    // Update self.Layer on the Lua table
    if (lua_table_ref() >= 0) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, lua_table_ref());
        int tbl = lua_gettop(L);

        lua_pushstring(L, "Layer");
        lua_pushstring(L, new_layer.c_str());
        lua_rawset(L, tbl);

        // Call self:OnLayerChange(new, old)
        lua_pushstring(L, "OnLayerChange");
        lua_gettable(L, tbl);
        if (lua_isfunction(L, -1)) {
            lua_pushvalue(L, tbl); // self
            lua_pushstring(L, new_layer.c_str());
            lua_pushstring(L, old_layer.c_str());
            if (lua_pcall(L, 3, 0, 0) != 0) {
                spdlog::warn("OnLayerChange error: {}", lua_tostring(L, -1));
                lua_pop(L, 1);
            }
        } else {
            lua_pop(L, 1); // non-function
        }
        lua_pop(L, 1); // tbl
    }
}

} // namespace osc::sim
