#include "sim/unit.hpp"
#include "sim/entity_registry.hpp"

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

void Unit::update(f64 dt, EntityRegistry& registry, lua_State* L) {
    if (destroyed()) return;

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
                navigator_.set_goal(cmd.target_pos);
            }
            if (!navigator_.update(*this, max_speed_, dt)) {
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
                    navigator_.set_goal(target->position());
                }
                navigator_.update(*this, max_speed_, dt);
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
                    navigator_.set_goal(cmd.target_pos);
                    navigator_.update(*this, max_speed_, dt);
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
            if (!progress_build(dt, registry, L)) {
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
            if (!progress_build(dt, registry, L)) {
                command_queue_.pop_front();
                continue;
            }
            goto done_commands;
        }

        case CommandType::Patrol: {
            if (!navigator_.is_moving() ||
                navigator_.goal().x != cmd.target_pos.x ||
                navigator_.goal().z != cmd.target_pos.z) {
                navigator_.set_goal(cmd.target_pos);
            }
            if (!navigator_.update(*this, max_speed_, dt)) {
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
                navigator_.set_goal(target->position());
                navigator_.update(*this, max_speed_, dt);
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
                navigator_.set_goal(target->position());
                navigator_.update(*this, max_speed_, dt);
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
                    if (!progress_build_assist(dt, registry)) {
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
            }

            goto done_commands; // Guard is persistent
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
    f32 bx = (cmd.type == CommandType::BuildFactory) ? position().x : cmd.target_pos.x;
    f32 bz = (cmd.type == CommandType::BuildFactory) ? position().z : cmd.target_pos.z;
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
        lua_pushstring(L, (cmd.type == CommandType::BuildFactory)
                              ? "UnitBuild"
                              : "MobileBuild");
        lua_rawset(L, btbl);
        lua_pop(L, 1); // btbl
    }

    // Call builder:OnStartBuild(target, 'UnitBuild')
    if (lua_table_ref() >= 0) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, lua_table_ref());
        int builder_tbl = lua_gettop(L);
        lua_pushstring(L, "OnStartBuild");
        lua_gettable(L, builder_tbl);
        if (lua_isfunction(L, -1)) {
            lua_pushvalue(L, builder_tbl); // self
            lua_pushvalue(L, target_tbl);  // target
            lua_pushstring(L, "UnitBuild");
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

bool Unit::progress_build(f64 dt, EntityRegistry& registry, lua_State* L) {
    auto* target = registry.find(build_target_id_);
    if (!target || target->destroyed()) {
        finish_build(registry, L, false);
        return false;
    }

    // Guard: if an assister already pushed fraction to 1.0 this tick
    if (target->fraction_complete() >= 1.0f) {
        finish_build(registry, L, true);
        return false;
    }

    if (build_time_ <= 0 || build_rate_ <= 0) {
        finish_build(registry, L, false);
        return false;
    }

    f32 progress_rate = build_rate_ / static_cast<f32>(build_time_);
    f32 new_frac = std::min(1.0f, target->fraction_complete() + progress_rate * static_cast<f32>(dt));
    target->set_fraction_complete(new_frac);
    target->set_health(new_frac * target->max_health());
    work_progress_ = new_frac;

    if (new_frac >= 1.0f) {
        finish_build(registry, L, true);
        return false; // command done
    }
    return true; // still building
}

void Unit::finish_build(EntityRegistry& registry, lua_State* L, bool success) {
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

bool Unit::progress_build_assist(f64 dt, EntityRegistry& registry) {
    auto* target = registry.find(build_target_id_);
    if (!target || target->destroyed())
        return false;

    if (build_time_ <= 0 || build_rate_ <= 0)
        return false;

    if (target->fraction_complete() >= 1.0f)
        return false;

    f32 progress_rate = build_rate_ / static_cast<f32>(build_time_);
    f32 new_frac = std::min(1.0f,
        target->fraction_complete() + progress_rate * static_cast<f32>(dt));
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

} // namespace osc::sim
