#include "sim/unit.hpp"
#include "core/dmath.hpp"
#include "core/test_status.hpp"
#include "blueprints/blueprint_store.hpp"
#include "sim/bone_data.hpp"
#include "sim/entity_registry.hpp"
#include "sim/manipulator.hpp"
#include "sim/sim_state.hpp"
#include "sim/thread_manager.hpp"
#include "map/pathfinding_grid.hpp"
#include "map/terrain.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <utility>
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

void Unit::clear_commands(const char*) {
    command_queue_.clear();
    navigator_.abort_move();
}

std::vector<BuildQueueEntry> Unit::factory_queue() const {
    std::vector<BuildQueueEntry> groups;
    for (const auto& c : command_queue_) {
        if (c.type != CommandType::BuildFactory) continue;
        if (!groups.empty() && groups.back().blueprint_id == c.blueprint_id) ++groups.back().count;
        else groups.push_back({c.blueprint_id, 1});
    }
    return groups;
}

void Unit::decrease_build_count(int index, int count, EntityRegistry& registry, lua_State* L) {
    if (index < 1 || count < 1) return;
    // Where the group's orders sit in the command queue, as factory_queue()
    // groups them.
    std::vector<size_t> group;
    int g = 0;
    const std::string* run = nullptr;
    for (size_t i = 0; i < command_queue_.size(); ++i) {
        const auto& c = command_queue_[i];
        if (c.type != CommandType::BuildFactory) continue;
        if (!run || *run != c.blueprint_id) {
            if (++g > index) break;
            run = &c.blueprint_id;
        }
        if (g == index) group.push_back(i);
    }
    // Newest first, so earlier positions stay valid.
    const bool in_progress = building_factory_order();
    bool cancel = false;
    for (auto it = group.rbegin(); it != group.rend() && count > 0; ++it, --count) {
        if (*it == 0 && in_progress) cancel = true;
        command_queue_.erase(command_queue_.begin() + static_cast<std::ptrdiff_t>(*it));
    }
    if (cancel) cancel_factory_build(registry, L);
}

void Unit::increase_build_count(int index, int count) {
    if (index < 1 || count < 1) return;
    // The group's last order, as factory_queue() groups them.
    std::optional<size_t> last;
    int g = 0;
    const std::string* run = nullptr;
    for (size_t i = 0; i < command_queue_.size(); ++i) {
        const auto& c = command_queue_[i];
        if (c.type != CommandType::BuildFactory) continue;
        if (!run || *run != c.blueprint_id) {
            if (++g > index) break;
            run = &c.blueprint_id;
        }
        if (g == index) last = i;
    }
    if (!last) return;
    // More of the same order, as Moho counts one factory command up: the
    // blueprint and the command it was issued as, none of the runtime state
    // of the group's last order (which may be the one under way).
    UnitCommand more;
    more.type = CommandType::BuildFactory;
    more.blueprint_id = command_queue_[*last].blueprint_id;
    more.command_id = command_queue_[*last].command_id;
    command_queue_.insert(command_queue_.begin() + static_cast<std::ptrdiff_t>(*last + 1),
                          static_cast<size_t>(count), more);
}

void Unit::cancel_factory_build(EntityRegistry& registry, lua_State* L) {
    const u32 target_id = build_target_id_;
    if (target_id == 0) return;
    finish_build(registry, L, false); // OnFailedToBuild; the factory's work ends
    // The unit under construction goes with it, through its own Destroy
    // (OnDestroy and the rest of its script lifecycle).
    auto* target = registry.find(target_id);
    if (!target || target->destroyed()) return;
    if (target->is_unit()) static_cast<Unit*>(target)->call_lua_method(L, "Destroy");
    target = registry.find(target_id);
    if (target && !target->destroyed()) { // no script object (or no Destroy)
        target->mark_destroyed();
        registry.unregister_entity(target_id);
    }
}

// --- Adjacency helpers ---

namespace {

struct SkirtRect { f32 x0, z0, x1, z1; };

SkirtRect get_skirt_rect(const Unit& u) {
    f32 sx0 = u.position().x - u.footprint_size_x() * 0.5f + u.skirt_offset_x();
    f32 sz0 = u.position().z - u.footprint_size_z() * 0.5f + u.skirt_offset_z();
    return { sx0, sz0, sx0 + u.skirt_size_x(), sz0 + u.skirt_size_z() };
}

bool skirts_adjacent(const SkirtRect& a, const SkirtRect& b) {
    // FA structures are grid-aligned but skirt edges can be at .0 or .5
    // offsets, so use a tolerance slightly larger than half a grid cell.
    constexpr f32 eps = 0.6f;
    bool x_overlap = (a.x0 < b.x1 - eps) && (b.x0 < a.x1 - eps);
    bool z_overlap = (a.z0 < b.z1 - eps) && (b.z0 < a.z1 - eps);
    bool x_touching = std::abs(a.x1 - b.x0) < eps || std::abs(b.x1 - a.x0) < eps;
    bool z_touching = std::abs(a.z1 - b.z0) < eps || std::abs(b.z1 - a.z0) < eps;
    return (x_overlap && z_touching) || (z_overlap && x_touching);
}

/// Call OnAdjacentTo(self_tbl, other_tbl, trigger_tbl) on self_tbl.
/// Returns false if self entity was destroyed by the callback.
bool call_on_adjacent_to(lua_State* L, int self_ref, int other_ref, int trigger_ref,
                         u32 self_id, EntityRegistry& registry) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, self_ref);
    int tbl = lua_gettop(L);
    lua_pushstring(L, "OnAdjacentTo");
    lua_gettable(L, tbl);
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, tbl);
        lua_rawgeti(L, LUA_REGISTRYINDEX, other_ref);
        lua_rawgeti(L, LUA_REGISTRYINDEX, trigger_ref);
        if (lua_pcall(L, 3, 0, 0) != 0) {
            spdlog::warn("OnAdjacentTo error: {}", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    } else {
        lua_pop(L, 1);
    }
    lua_pop(L, 1); // tbl
    auto* e = registry.find(self_id);
    return e && !e->destroyed();
}

} // anonymous namespace

void Unit::fire_adjacency_callbacks(EntityRegistry& registry, lua_State* L) {
    if (!has_category("STRUCTURE")) return;
    if (skirt_size_x_ <= 0 || skirt_size_z_ <= 0) return;
    if (lua_table_ref() < 0) return;

    auto my_skirt = get_skirt_rect(*this);
    // Expand generously: a neighbor's center can be far from its skirt edge
    // (e.g., factory center at 200 has skirt edge at 204, 4 units away).
    // Use 12.0 to cover largest FA structures (experimental: ~8 footprint + ~8 skirt offset).
    constexpr f32 expand = 12.0f;
    auto candidates = registry.collect_in_rect(
        my_skirt.x0 - expand, my_skirt.z0 - expand,
        my_skirt.x1 + expand, my_skirt.z1 + expand);

    u32 my_id = entity_id();

    for (u32 cid : candidates) {
        if (cid == my_id) continue;
        auto* ce = registry.find(cid);
        if (!ce || ce->destroyed() || !ce->is_unit()) continue;
        auto* cu = static_cast<Unit*>(ce);
        if (cu->army() != army()) continue;
        if (!cu->has_category("STRUCTURE")) continue;
        if (cu->is_being_built()) continue;
        if (cu->skirt_size_x() <= 0 || cu->skirt_size_z() <= 0) continue;
        if (cu->lua_table_ref() < 0) continue;

        auto cand_skirt = get_skirt_rect(*cu);
        if (!skirts_adjacent(my_skirt, cand_skirt)) continue;
        if (adjacent_unit_ids_.count(cid)) continue; // already adjacent

        // Track adjacency on both sides
        add_adjacent(cid);
        cu->add_adjacent(my_id);

        // Fire OnAdjacentTo(self, neighbor, self) on self
        if (!call_on_adjacent_to(L, lua_table_ref(), cu->lua_table_ref(),
                                  lua_table_ref(), my_id, registry)) {
            return; // self destroyed
        }

        // Re-validate neighbor
        ce = registry.find(cid);
        if (!ce || ce->destroyed()) continue;
        cu = static_cast<Unit*>(ce);

        // Fire OnAdjacentTo(neighbor, self, self) on neighbor
        // Re-validate self first
        auto* me = registry.find(my_id);
        if (!me || me->destroyed()) return;

        call_on_adjacent_to(L, cu->lua_table_ref(), lua_table_ref(),
                            lua_table_ref(), cid, registry);

        // Re-validate self after neighbor callback
        me = registry.find(my_id);
        if (!me || me->destroyed()) return;

        spdlog::debug("Adjacency: #{} and #{} are now adjacent", my_id, cid);
    }
}

void Unit::clear_on_given_callbacks(lua_State* L) {
    for (int ref : on_given_callbacks_) {
        luaL_unref(L, LUA_REGISTRYINDEX, ref);
    }
    on_given_callbacks_.clear();
}

void Unit::begin_dying() {
    if (dying_) return;
    dying_ = true;
    clear_commands();
    set_do_not_target(true);
    economy_.consumption_mass = 0;
    economy_.consumption_energy = 0;
    economy_.consumption_active = false;
    economy_.production_mass = 0;
    economy_.production_energy = 0;
    economy_.production_active = false;
    // Its missile under way stops asking for resources.
    abandon_silo_build();
    // Killed in flight: it falls, tumbling, until it lands.
    if (is_air_unit()) {
        crashing_ = true;
        crash_velocity_y_ = 0;
        crash_spin_rate_ = (static_cast<f32>(entity_id() % 100) / 100.0f - 0.5f) * 4.0f;
    }
}

bool Unit::take_crash_impact() {
    const bool landed = crash_impacted_;
    crash_impacted_ = false;
    return landed;
}

void Unit::tick_dying(f32 dt, const map::Terrain* terrain) {
    if (!dying_) return;

    if (crashing_) {
        // Gravity acceleration
        crash_velocity_y_ += -9.8f * dt;

        // Spin and tumble
        heading_ += crash_spin_rate_ * dt;
        bank_angle_ += crash_spin_rate_ * 0.7f * dt;
        pitch_ = std::clamp(pitch_ - 0.3f * dt, -1.0f, 0.3f);
        set_orientation(euler_to_quat(heading_, pitch_, bank_angle_));

        // Move forward (decaying) and down
        auto p = position();
        f32 fwd = current_airspeed_ * 0.5f;
        p.x += osc::dmath::sin(heading_) * fwd * dt;
        p.z += osc::dmath::cos(heading_) * fwd * dt;
        p.y += crash_velocity_y_ * dt;

        // It lands on the ground, or on the water over it.
        f32 ground = terrain ? terrain->get_surface_height(p.x, p.z) : 0.0f;
        if (p.y <= ground) {
            p.y = ground;
            set_position(p);
            crash_impacted_ = true;
            crashing_ = false;
            return;
        }

        set_position(p);
        current_airspeed_ *= (1.0f - 0.5f * dt); // decelerate forward speed
    }
}

bool Unit::nav_update(f64 dt, const map::Terrain* terrain, f32 speed_cap) {
    if (is_air_unit())
        return navigator_.update_air(*this, dt, terrain);
    const f32 speed = speed_cap > 0 ? std::min(effective_speed(), speed_cap) : effective_speed();
    bool result = navigator_.update(*this, speed, dt, terrain);

    // A sub under the surface, or on its way, keeps its depth as it moves
    // (the ground navigator put it on the surface; see tick_dive).
    if (terrain && (sub_elevation_ != 0.0f || vert_motion_ != VertMotion::None)) {
        auto p = position();
        p.y = terrain->water_elevation() + sub_elevation_;
        set_position(p);
    }

    return result;
}

void Unit::update(f64 dt, SimContext& ctx) {
    if (!tick_lifecycle(dt, ctx)) return;

    // Compute economy efficiency for this unit's army
    f32 econ_eff = 1.0f;
    if (army() >= 0 && static_cast<u32>(army()) < SimContext::MAX_EFFICIENCY_ARMIES) {
        const auto& ae = ctx.army_efficiency[static_cast<u32>(army())];
        econ_eff = static_cast<f32>(std::min(ae.mass, ae.energy));
    }

    // Assisting a silo is renewed each tick (see Guard); one that stops
    // stops paying below.
    const bool was_assisting_silo = std::exchange(assisting_silo_, false);

    // A teleport or an OverCharge whose order was taken away unfinished.
    if (teleporting_ || overcharge_armed_) {
        settle_interrupted_orders(ctx.L);
        if (destroyed() || !in_registry()) return;
    }
    // A ferry, or a unit waiting for one, whose order is gone from the head
    // of its queue stops being one.
    {
        const UnitCommand* head = command_queue_.empty() ? nullptr : &command_queue_.front();
        if (has_unit_state("Ferrying") && !(head && head->type == CommandType::Ferry)) {
            set_unit_state("Ferrying", false);
            ferry_phase_ = FerryPhase::Load;
            ferry_index_ = 0;
            ferry_leg_set_ = false;
            ferry_for_ = 0;
        }
        if (has_unit_state("WaitForFerry") && !(head && head->type == CommandType::WaitForFerry))
            set_unit_state("WaitForFerry", false);
        // A factory whose guard order went while it built for the guarded
        // factory drops that unit (M206h).
        if (factory_assist_build_ && !(head && head->type == CommandType::Guard))
            end_guard_build(ctx.registry, ctx.L);
    }

    // Paused units skip their orders, and what follows them, but still
    // update weapons.
    if (!paused_) {
        if (!tick_orders(dt, ctx, econ_eff)) return;
        if (!tick_after_orders(dt, ctx)) return;
    }
    tick_upkeep(dt, ctx, econ_eff, was_assisting_silo);
}

bool Unit::tick_lifecycle(f64 dt, SimContext& ctx) {
    if (destroyed()) return false;

    // Dying units only tick manipulators (for death animation) — skip everything else
    if (dying_) {
        tick_dying(static_cast<f32>(dt), ctx.terrain);
        tick_manipulators(static_cast<f32>(dt), ctx.L);
        return false;
    }
    auto& registry = ctx.registry;

    // A pickup whose load order is no longer the transport's head is over,
    // aborted; a unit whose load order went mid-beam comes back down (M206m).
    const bool loading_head =
        !command_queue_.empty() && command_queue_.front().type == CommandType::TransportLoad;
    if (pickup_phase_ != PickupPhase::None &&
        !(loading_head && command_queue_.front().target_id == entity_id())) {
        finish_pickup(false, ctx.L);
        if (destroyed() || !in_registry()) return false;
    }
    if (beam_up_ticks_ > 0 && !(loading_head && command_queue_.front().target_id != entity_id())) {
        abandon_beam_up(ctx.terrain, ctx.L);
        if (destroyed() || !in_registry()) return false;
    }

    // A transport gives up the slots of units no longer aboard (M206l).
    if (transport_slots_ && !transport_slots_->slots().empty()) release_stale_slots(registry);

    // Cargo position following: if loaded on a transport, skip all processing
    if (transport_id_ != 0) {
        auto* transport_entity = registry.find(transport_id_);
        if (transport_entity && !transport_entity->destroyed() && transport_entity->is_unit()) {
            hang_from(*static_cast<const Unit*>(transport_entity));
        } else {
            // Transport gone — auto-detach and clean up stale cargo entry
            if (transport_entity && transport_entity->is_unit()) {
                static_cast<Unit*>(transport_entity)->remove_cargo(entity_id());
            }
            transport_id_ = 0;
            set_unit_state("Attached", false);
        }
        ground_speed_ = 0; // carried, not driving
        return false;      // Skip commands and weapons while loaded
    }
    drove_ = false;
    return true;
}

bool Unit::tick_after_orders(f64 dt, SimContext& ctx) {
    if (destroyed() || !in_registry()) return false;
    auto& registry = ctx.registry;
    auto* L = ctx.L;

    if (!drove_ && !is_air_unit()) coast(dt, ctx.terrain);

    // A sub dives or surfaces, moving or not (M206o).
    tick_dive(ctx.terrain, L);
    if (destroyed() || !in_registry()) return false;

    // An idle transport hovers low with cargo aboard, and climbs back to its
    // flying height without (Moho's ShouldHoverInsteadOfLand; M206n).
    if (transport_hover_height_ > 0 && is_air_unit() && !dying_ && command_queue_.empty() &&
        !navigator_.is_moving())
        hold_altitude(dt, ctx.terrain,
                      cargo_ids_.empty() ? elevation_target_ : transport_hover_height_);

    // Amphibious layer transition: auto-switch Land↔Water based on terrain
    if (is_amphibious() && !dying_ && ctx.terrain) {
        f32 terrain_h = ctx.terrain->get_terrain_height(position().x, position().z);
        f32 water_elev = ctx.terrain->water_elevation();
        if (terrain_h < water_elev && layer_ == "Land") {
            set_layer_with_callback("Water", L);
        } else if (terrain_h >= water_elev && layer_ == "Water") {
            set_layer_with_callback("Land", L);
        }
    }

    // Air unit separation: lightweight boids repulsion to prevent stacking
    if (is_air_unit() && !dying_ && navigator_.is_moving()) {
        constexpr f32 SEPARATION_RADIUS = 8.0f;
        constexpr f32 SEPARATION_FORCE = 3.0f;
        const auto nearby = registry.units_in_radius(position().x, position().z, SEPARATION_RADIUS);
        f32 repulse_x = 0, repulse_z = 0;
        for (auto* ne : nearby) {
            if (ne == this) continue;
            auto* nu = static_cast<Unit*>(ne);
            if (!nu->is_air_unit() || nu->army() != army()) continue;
            f32 ndx = position().x - ne->position().x;
            f32 ndz = position().z - ne->position().z;
            f32 nd2 = ndx * ndx + ndz * ndz;
            if (nd2 > 0.01f && nd2 < SEPARATION_RADIUS * SEPARATION_RADIUS) {
                f32 inv = 1.0f / std::sqrt(nd2);
                repulse_x += ndx * inv;
                repulse_z += ndz * inv;
            }
        }
        if (repulse_x != 0 || repulse_z != 0) {
            auto p = position();
            f32 fdt = static_cast<f32>(dt);
            p.x += repulse_x * SEPARATION_FORCE * fdt;
            p.z += repulse_z * SEPARATION_FORCE * fdt;
            set_position(p);
        }
    }

    // Carried units hang where the transport now is (M206l), however their
    // own ticks fall around ours: Moho moves attached entities with their
    // parent, so none trails it by a tick.
    for (const u32 id : cargo_ids_) {
        auto* e = registry.find(id);
        if (!e || e->destroyed() || !e->is_unit()) continue;
        auto* cargo = static_cast<Unit*>(e);
        if (cargo->transport_id() == entity_id()) cargo->hang_from(*this);
    }

    // Fuel: flying burns it. Running dry doesn't bring an aircraft down: its
    // script slows it (OnRunOutOfFuel) until refuelling restores it (OnGotFuel).
    if (fuel_ratio_ >= 0 && fuel_use_time_ > 0) {
        if (is_air_unit())
            fuel_ratio_ = std::max(0.0f, fuel_ratio_ - static_cast<f32>(dt) / fuel_use_time_);
        const bool dry = fuel_ratio_ <= 0;
        if (dry != out_of_fuel_) {
            out_of_fuel_ = dry;
            if (L) call_lua_method(L, dry ? "OnRunOutOfFuel" : "OnGotFuel");
            if (destroyed() || dying_) return false;
        }
    }

    return true;
}

void Unit::tick_upkeep(f64 dt, SimContext& ctx, f32 econ_eff, bool was_assisting_silo) {
    auto& registry = ctx.registry;
    auto* L = ctx.L;

    if (was_assisting_silo && !assisting_silo_ && !is_building() && !is_reclaiming() &&
        !is_repairing() && !is_capturing() && !enhancing_) {
        economy_.consumption_energy = 0;
        economy_.consumption_mass = 0;
        economy_.consumption_active = false;
    }

    // Per-tick health regeneration (base rate + veterancy buffs via SetRegenRate)
    if (regen_rate() > 0 && health() > 0 && health() < max_health()) {
        f32 new_hp = std::min(max_health(), health() + regen_rate() * static_cast<f32>(dt));
        set_health(new_hp);
    }

    // Weapons hear about the motion change (through the unit script) before
    // they fire this tick. A unit under construction neither moves nor
    // fights. A weapon's script may kill its own unit (KamikazeWeapon).
    if (!is_being_built()) {
        // Its silo builds beside everything else (paused, it waits).
        if (!dying_ && (silo_building() || !silo_orders_.empty() || auto_mode_)) {
            update_silo(dt, econ_eff, L);
            if (destroyed() || dying_) return;
        }
        update_motion_horz(L);
        update_motion_turn(L);
        for (auto& weapon : weapons_) {
            if (destroyed() || dying_) break;
            weapon->update(*this, registry, L, ctx.visibility_grid, ctx.sim);
        }
    }

    // Update manipulators (rotators, animators, sliders, aim controllers)
    tick_manipulators(static_cast<f32>(dt), L);
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

    // UnitBeingBuilt / UnitBuildOrder on the builder are the scripts' own
    // fields (StructureUnit/ConstructionUnit.OnStartBuild set them), not the
    // engine's: clearing them when the build ended broke factory rolloff,
    // which still reads UnitBeingBuilt afterwards.

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
                          map::PathfindingGrid* grid, f32 efficiency, bool* built) {
    if (built) *built = false;
    auto* target = registry.find(build_target_id_);
    if (!target || target->destroyed()) {
        finish_build(registry, L, false, grid);
        return false;
    }

    // Guard: if an assister already pushed fraction to 1.0 this tick
    if (target->fraction_complete() >= 1.0f) {
        if (built) *built = true;
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
        if (built) *built = true;
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

            // A unit built by its army (Moho's Units_History)
            lua_pushstring(L, "osc_sim_state");
            lua_rawget(L, LUA_REGISTRYINDEX);
            auto* sim_for_stat = static_cast<sim::SimState*>(lua_touserdata(L, -1));
            lua_pop(L, 1);
            if (sim_for_stat) {
                if (auto* brain = sim_for_stat->get_army(target_unit->army())) {
                    brain->record_unit_built(target_unit->blueprint_id(),
                                             target_unit->build_cost_mass(),
                                             target_unit->build_cost_energy());
                }
            }

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

        // The completed structure now blocks paths until it is removed
        // (after re-validation — only if target survived OnStopBeingBuilt)
        if (target && !target->destroyed() && target->is_unit()) {
            lua_pushstring(L, "osc_sim_state");
            lua_rawget(L, LUA_REGISTRYINDEX);
            auto* sim = static_cast<SimState*>(lua_touserdata(L, -1));
            lua_pop(L, 1);
            if (sim) sim->occupy_footprint(*static_cast<Unit*>(target));
        }

        // Fire adjacency callbacks for newly completed structure
        target = registry.find(build_target_id_);
        if (target && !target->destroyed() && target->is_unit()) {
            static_cast<Unit*>(target)->fire_adjacency_callbacks(registry, L);
        }

        // Auto-add completed unit to its army's ArmyPool platoon.
        // Original GPG engine auto-assigns every completed unit to ArmyPool;
        // AI managers (PlatoonFormManager, FactoryBuilderManager) rely on this.
        target = registry.find(build_target_id_);
        if (target && !target->destroyed() && target->is_unit()) {
            auto* completed = static_cast<Unit*>(target);
            lua_pushstring(L, "osc_sim_state");
            lua_rawget(L, LUA_REGISTRYINDEX);
            auto* sim = static_cast<SimState*>(lua_touserdata(L, -1));
            lua_pop(L, 1);
            if (sim) {
                auto* brain = sim->get_army(completed->army());
                if (brain) {
                    auto* pool = brain->find_platoon_by_name("ArmyPool");
                    if (pool && !pool->has_unit(build_target_id_)) {
                        pool->add_unit(build_target_id_);
                    }
                }
            }
        }

        // Call builder:OnStopBuild(target)
        target = registry.find(build_target_id_);
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
        registry.unregister_entity(target_id);
    }
}

bool Unit::progress_reclaim(f64 dt, EntityRegistry& registry, lua_State* L) {
    auto* target = registry.find(reclaim_target_id_);
    if (!target || target->destroyed() || !target->reclaimable()) {
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
    target_unit->set_being_captured(true);
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

        // Clear being_captured on target
        if (target->is_unit())
            static_cast<Unit*>(target)->set_being_captured(false);

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

    // Clear being_captured on target
    if (target->is_unit())
        static_cast<Unit*>(target)->set_being_captured(false);

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

// --- Stats/telemetry system ---

void Unit::set_stat(const std::string& key, f64 value) {
    stats_[key] = value;
}

f64 Unit::get_stat(const std::string& key, f64 default_val) const {
    auto it = stats_.find(key);
    return (it != stats_.end()) ? it->second : default_val;
}

bool Unit::has_stat(const std::string& key) const {
    return stats_.count(key) > 0;
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

bool Unit::start_enhance(const UnitCommand& cmd, lua_State* L,
                         const blueprints::BlueprintStore* store) {
    enhance_name_ = cmd.blueprint_id;
    enhance_slot_.clear();

    // Read the enhancement from Blueprint.Enhancements[name]
    f64 enh_build_time = 0, enh_cost_mass = 0, enh_cost_energy = 0;

    const blueprints::BlueprintEntry* entry =
        store ? store->find(blueprint_id()) : nullptr;
    if (entry) {
        store->push_lua_table(*entry, L);
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

                    lua_pushstring(L, "Slot");
                    lua_gettable(L, -2);
                    if (lua_type(L, -1) == LUA_TSTRING)
                        enhance_slot_ = lua_tostring(L, -1);
                    lua_pop(L, 1);
                }
                lua_pop(L, 1); // enh entry
            }
            lua_pop(L, 1); // Enhancements
        }
        lua_pop(L, 1); // blueprint
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

    // Also add to the C++ enhancement map (slot read in start_enhance)
    if (!enhance_slot_.empty()) enhancements_[enhance_slot_] = enhance_name_;

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
    if (type == "Cloak") {
        set_cloaked(true);
    } else if (type == "RadarStealth") {
        set_radar_stealth(true);
    } else if (type == "SonarStealth") {
        set_sonar_stealth(true);
    }
}

void Unit::add_intel(const std::string& type, f32 radius) {
    intel_states_.try_emplace(type, IntelState{radius, false});
}

void Unit::enable_intel(const std::string& type) {
    auto it = intel_states_.find(type);
    if (it == intel_states_.end()) return;
    it->second.enabled = true;
    if (type == "Cloak") {
        set_cloaked(true);
    } else if (type == "RadarStealth") {
        set_radar_stealth(true);
    } else if (type == "SonarStealth") {
        set_sonar_stealth(true);
    }
}

void Unit::disable_intel(const std::string& type) {
    auto it = intel_states_.find(type);
    if (it != intel_states_.end())
        it->second.enabled = false;
    if (type == "Cloak") {
        set_cloaked(false);
    } else if (type == "RadarStealth") {
        set_radar_stealth(false);
    } else if (type == "SonarStealth") {
        set_sonar_stealth(false);
    }
}

void Unit::set_intel_radius(const std::string& type, f32 radius) {
    intel_states_[type].radius = radius;
}

// ---------------------------------------------------------------------------
// Transport system
// ---------------------------------------------------------------------------

TransportSlots* Unit::transport_slots() {
    if (!transport_slots_ && bone_data())
        transport_slots_ = std::make_unique<TransportSlots>(*bone_data(), transport_layout_);
    return transport_slots_.get();
}

bool Unit::transport_has_space_for(const Unit& cargo) {
    if (const TransportSlots* slots = transport_slots(); slots && slots->has_points())
        return slots->has_space_for(cargo.transport_class());
    return transport_capacity_ <= 0 || static_cast<i32>(cargo_ids_.size()) < transport_capacity_;
}

i32 Unit::transport_attach_bone() const {
    // Moho's GetBestAttachPoint.
    const i32 bone = bone_data() ? bone_data()->find_bone("AttachPoint") : -1;
    if (bone < 0 && motion_type_ == "RULEUMT_Air") return 0;
    return bone;
}

void Unit::hang_from(const Unit& transport) {
    const TransportSlots* slots = transport.built_transport_slots();
    const TransportSlots::Slot* slot = slots ? slots->slot_of(entity_id()) : nullptr;
    if (!slot) {
        set_position(transport.position());
        return;
    }
    const Vector3 at = transport.bone_world_position(slot->bone);
    const Quaternion facing =
        quat_multiply(transport.orientation(), transport.bone_pose(slot->bone).rotation);
    // Our AttachPoint bone, or our centre (Moho's bone -1: half our height up).
    Vector3 anchor{0.0f, size_y_ * 0.5f, 0.0f};
    if (bone_data() && bone_data()->is_valid(slot->unit_bone)) {
        const Vector3 model = bone_pose(slot->unit_bone).position;
        const f32 scale = bone_data()->model_scale;
        anchor = {model.x * scale, model.y * scale, model.z * scale};
    }
    const Vector3 offset = quat_rotate(facing, anchor);
    set_position({at.x - offset.x, at.y - offset.y, at.z - offset.z});
    set_orientation(facing);
}

void Unit::release_stale_slots(const EntityRegistry& registry) {
    std::vector<u32> stale;
    for (const TransportSlots::Slot& slot : transport_slots_->slots()) {
        const Entity* e = registry.find(slot.unit_id);
        const auto* u =
            e && !e->destroyed() && e->is_unit() ? static_cast<const Unit*>(e) : nullptr;
        const bool aboard =
            u && u->transport_id() == entity_id() &&
            std::find(cargo_ids_.begin(), cargo_ids_.end(), slot.unit_id) != cargo_ids_.end();
        // A unit given a slot for the pickup keeps it while it still comes (M206m).
        const bool coming =
            u && !u->is_dying() && u->calls_transport(entity_id()) &&
            std::find(pickup_ids_.begin(), pickup_ids_.end(), slot.unit_id) != pickup_ids_.end();
        if (!aboard && !coming) stale.push_back(slot.unit_id);
    }
    for (const u32 id : stale) transport_slots_->release(id);
}

void Unit::remove_cargo(u32 id) {
    cargo_ids_.erase(std::remove(cargo_ids_.begin(), cargo_ids_.end(), id),
                     cargo_ids_.end());
}

void Unit::attach_to_transport(Unit* transport, EntityRegistry& registry,
                               lua_State* L) {
    // A free slot of the unit's class (M206l); a transport without attach
    // points counts its Class1Capacity instead. Scripts hear the bone.
    std::string bone_name = "Attachpoint";
    if (TransportSlots* slots = transport->transport_slots(); slots && slots->has_points()) {
        const std::optional<i32> bone =
            slots->assign(entity_id(), transport_class_, transport_attach_bone());
        if (!bone) {
            spdlog::warn("Transport #{} has no free slot for #{} (class {})",
                         transport->entity_id(), entity_id(), transport_class_);
            return;
        }
        bone_name = transport->bone_data()->bones[static_cast<size_t>(*bone)].name;
    } else if (!transport->transport_has_space_for(*this)) {
        spdlog::warn("Transport #{} is full (capacity {}), cannot attach #{}",
                     transport->entity_id(), transport->transport_capacity(),
                     entity_id());
        return;
    }

    transport_id_ = transport->entity_id();
    transport->add_cargo(entity_id());
    set_unit_state("Attached", true);
    navigator_.abort_move();
    ground_speed_ = 0; // aboard, it no longer drives
    // Boarding pops the unit onto its bone; the renderer jumps it.
    hang_from(*transport);
    note_snap();

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
            lua_pushstring(L, bone_name.c_str());
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

void Unit::detach_all_cargo(EntityRegistry& registry, lua_State* L, const map::Terrain* terrain) {
    detach_cargo(cargo_ids_, registry, L, terrain);
}

bool Unit::footprint_fits(const map::PathfindingGrid& grid) const {
    const f32 half_x = std::max(footprint_size_x(), 1.0f) * 0.5f;
    const f32 half_z = std::max(footprint_size_z(), 1.0f) * 0.5f;
    constexpr f32 kInside = 0.01f; // the footprint's own edge cells only
    u32 x0 = 0, z0 = 0, x1 = 0, z1 = 0;
    grid.world_to_grid(position().x - half_x + kInside, position().z - half_z + kInside, x0, z0);
    grid.world_to_grid(position().x + half_x - kInside, position().z + half_z - kInside, x1, z1);
    const bool amphibious = is_amphibious() || is_hover();
    for (u32 gz = z0; gz <= z1; ++gz)
        for (u32 gx = x0; gx <= x1; ++gx)
            if (!grid.is_passable_for(gx, gz, "Land", naval_draft_, amphibious)) return false;
    return true;
}

void Unit::detach_cargo(std::vector<u32> ids, EntityRegistry& registry, lua_State* L,
                        const map::Terrain* terrain) {
    // Taken off the cargo list first (safety against modification during
    // Lua callbacks), in cargo order.
    std::vector<u32> snapshot;
    std::erase_if(cargo_ids_, [&](u32 id) {
        if (std::find(ids.begin(), ids.end(), id) == ids.end()) return false;
        snapshot.push_back(id);
        return true;
    });

    for (u32 cargo_id : snapshot) {
        auto* cargo_entity = registry.find(cargo_id);
        if (!cargo_entity || cargo_entity->destroyed() ||
            !cargo_entity->is_unit())
            continue;
        auto* cargo = static_cast<Unit*>(cargo_entity);

        cargo->set_transport_id(0);
        cargo->set_unit_state("Attached", false);
        // Set down where it hangs, level, its slot given up (M206l); a
        // transport without slots drops it at its origin.
        std::string bone_name = "Attachpoint";
        const TransportSlots::Slot* slot =
            transport_slots_ ? transport_slots_->slot_of(cargo_id) : nullptr;
        if (slot) {
            bone_name = bone_data()->bones[static_cast<size_t>(slot->bone)].name;
            transport_slots_->release(cargo_id);
        } else {
            cargo->set_position(position());
        }
        cargo->set_orientation(euler_to_quat(quat_yaw(cargo->orientation()), 0.0f, 0.0f));
        if (terrain) {
            Vector3 at = cargo->position();
            at.y = terrain->get_surface_height(at.x, at.z);
            cargo->set_position(at);
        }
        cargo->note_snap();

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
                lua_pushstring(L, bone_name.c_str());
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

namespace {

const char* motion_horz_name(Unit::MotionHorz motion) {
    switch (motion) {
    case Unit::MotionHorz::Stopped: return "Stopped";
    case Unit::MotionHorz::Cruise: return "Cruise";
    case Unit::MotionHorz::TopSpeed: return "TopSpeed";
    case Unit::MotionHorz::Stopping: return "Stopping";
    }
    return "Stopped";
}

} // namespace

/// Moho reports a unit's horizontal motion as it starts, reaches top speed,
/// slows and stops. Retail's Unit script plays move sounds and effects on it
/// and passes it to every weapon (FiringRandomnessWhileMoving, packing to
/// move). Ground units here reach full speed at once, so a start is Cruise
/// then TopSpeed a tick later, and a stop is Stopping then Stopped.
void Unit::update_motion_horz(lua_State* L) {
    MotionHorz next = motion_horz_;
    if (is_air_unit()) {
        const bool moving = current_airspeed_ > 0.01f;
        const bool at_top = current_airspeed_ >= 0.99f * max_airspeed_;
        switch (motion_horz_) {
        case MotionHorz::Stopped:
            if (moving) next = MotionHorz::Cruise;
            break;
        case MotionHorz::Cruise:
        case MotionHorz::TopSpeed:
            if (!moving) next = MotionHorz::Stopping;
            else next = at_top ? MotionHorz::TopSpeed : MotionHorz::Cruise;
            break;
        case MotionHorz::Stopping: next = moving ? MotionHorz::Cruise : MotionHorz::Stopped; break;
        }
    } else {
        // From its speed: at rest, braking to a halt, at full speed, or on
        // the way up to it.
        const f32 speed = std::abs(ground_speed_);
        if (speed <= 1e-3f) next = MotionHorz::Stopped;
        else if (target_speed_ <= 1e-3f) next = MotionHorz::Stopping;
        else if (speed >= 0.99f * top_speed_) next = MotionHorz::TopSpeed;
        else next = MotionHorz::Cruise;
    }
    if (next == motion_horz_) return;
    const MotionHorz old = motion_horz_;
    motion_horz_ = next;

    if (!L || lua_table_ref() < 0) return;
    const int top = lua_gettop(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, lua_table_ref());
    const int self = lua_gettop(L);
    lua_pushstring(L, "OnMotionHorzEventChange");
    lua_gettable(L, self);
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, self);
        lua_pushstring(L, motion_horz_name(next));
        lua_pushstring(L, motion_horz_name(old));
        if (lua_pcall(L, 3, 0, 0) != 0) {
            const char* err = lua_tostring(L, -1);
            const std::string message =
                "OnMotionHorzEventChange error: " + std::string(err ? err : "(unknown)");
            spdlog::warn("{}", message);
            if (test_status::count_lua_failures()) test_status::record_failure(message);
        }
    }
    lua_settop(L, top);
}

namespace {

const char* motion_turn_name(Unit::MotionTurn turn) {
    switch (turn) {
    case Unit::MotionTurn::Straight: return "Straight";
    case Unit::MotionTurn::Turn: return "Turn";
    case Unit::MotionTurn::SharpTurn: return "SharpTurn";
    }
    return "Straight";
}

} // namespace

void Unit::update_motion_turn(lua_State* L) {
    const MotionTurn next = drove_ ? motion_turn_next_ : MotionTurn::Straight;
    if (next == motion_turn_) return;
    const MotionTurn old = motion_turn_;
    motion_turn_ = next;
    if (!L || lua_table_ref() < 0) return;
    const int top = lua_gettop(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, lua_table_ref());
    const int self = lua_gettop(L);
    lua_pushstring(L, "OnMotionTurnEventChange");
    lua_gettable(L, self);
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, self);
        lua_pushstring(L, motion_turn_name(next));
        lua_pushstring(L, motion_turn_name(old));
        if (lua_pcall(L, 3, 0, 0) != 0) {
            const char* err = lua_tostring(L, -1);
            const std::string message =
                "OnMotionTurnEventChange error: " + std::string(err ? err : "(unknown)");
            spdlog::warn("{}", message);
            if (test_status::count_lua_failures()) test_status::record_failure(message);
        }
    }
    lua_settop(L, top);
}

f32 Unit::separation_radius() const {
    const CollisionShape& s = collision_shape();
    switch (s.type) {
    case CollisionShapeType::BOX: return std::max(s.sx, s.sz);
    case CollisionShapeType::SPHERE: return s.sx;
    case CollisionShapeType::NONE: break;
    }
    return 0;
}

void Unit::coast(f64 dt, const map::Terrain* terrain) {
    target_speed_ = 0;
    if (ground_speed_ == 0) return;
    if (immobile_ || parent_entity_id() != 0) {
        ground_speed_ = 0;
        return;
    }
    const f32 step = static_cast<f32>(dt);
    // A unit made without its blueprint's physics stops at once.
    const f32 brake =
        drive_.max_brake > 0 ? drive_.max_brake * accel_mult_ * step : std::abs(ground_speed_);
    ground_speed_ = ground_speed_ > 0 ? std::max(0.0f, ground_speed_ - brake)
                                      : std::min(0.0f, ground_speed_ + brake);
    const f32 heading = quat_yaw(orientation());
    Vector3 p = position();
    p.x += osc::dmath::sin(heading) * ground_speed_ * step;
    p.z += osc::dmath::cos(heading) * ground_speed_ * step;
    if (terrain) p.y = terrain->get_surface_height(p.x, p.z);
    set_position(p);
}

void Unit::set_vert_event(const char* event, lua_State* L) {
    if (vert_event_ == event) return;
    const std::string old = vert_event_;
    vert_event_ = event;
    if (!L || lua_table_ref() < 0) return;
    lua_rawgeti(L, LUA_REGISTRYINDEX, lua_table_ref());
    const int tbl = lua_gettop(L);
    lua_pushstring(L, "OnMotionVertEventChange");
    lua_gettable(L, tbl);
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, tbl);
        lua_pushstring(L, event);
        lua_pushstring(L, old.c_str());
        if (lua_pcall(L, 3, 0, 0) != 0) {
            spdlog::warn("OnMotionVertEventChange error: {}", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    } else {
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
}

void Unit::start_dive(lua_State* L) {
    vert_motion_ = VertMotion::Down;
    set_unit_state("MovingUp", false);
    set_unit_state("MovingDown", true);
    set_vert_event("Down", L);
}

void Unit::start_surfacing(lua_State* L) {
    vert_motion_ = VertMotion::Up;
    set_unit_state("MovingDown", false);
    set_unit_state("MovingUp", true);
    set_vert_event("Up", L);
}

void Unit::tick_dive(const map::Terrain* terrain, lua_State* L) {
    if (!terrain) return;
    const f32 water = terrain->water_elevation();
    if (vert_motion_ != VertMotion::None) {
        // Down to its Physics.Elevation, but no deeper than a quarter over
        // the seabed; at DiveSurfaceSpeed/10 a tick, eased along a sine
        // (never under a tenth of that).
        f32 limit = elevation_target_;
        if (limit >= 0.0f) {
            vert_motion_ = VertMotion::None;
            set_unit_state("MovingDown", false);
            set_unit_state("MovingUp", false);
        } else {
            const f32 floor = std::min(
                0.0f, terrain->get_terrain_height(position().x, position().z) + 0.25f - water);
            limit = std::max(limit, floor);
            f32 phase = std::abs(sub_elevation_ / limit);
            if (phase > 0.5f) phase = 1.0f - phase;
            const f32 base = dive_surface_speed_ * 0.1f;
            const f32 speed = std::max(base * 0.1f, osc::dmath::sin(phase * 3.1415927f) * base);
            if (vert_motion_ == VertMotion::Up) {
                sub_elevation_ = std::min(0.0f, sub_elevation_ + speed);
                if (sub_elevation_ == 0.0f) {
                    vert_motion_ = VertMotion::None;
                    set_unit_state("MovingUp", false);
                    set_layer_with_callback("Water", L);
                    if (destroyed() || !in_registry()) return;
                    set_vert_event("Top", L);
                    if (destroyed() || !in_registry()) return;
                }
            } else if (sub_elevation_ - speed > limit) {
                sub_elevation_ -= speed;
            } else {
                sub_elevation_ = limit;
                vert_motion_ = VertMotion::None;
                set_unit_state("MovingDown", false);
                set_layer_with_callback("Sub", L);
                if (destroyed() || !in_registry()) return;
                set_vert_event("Bottom", L);
                if (destroyed() || !in_registry()) return;
            }
        }
    }
    // Under the surface, or on its way: at its depth.
    if (sub_elevation_ != 0.0f || vert_motion_ != VertMotion::None) {
        Vector3 at = position();
        at.y = water + sub_elevation_;
        set_position(at);
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

// ---------------------------------------------------------------------------
// Lua callback helpers
// ---------------------------------------------------------------------------

bool Unit::call_on_teleport_unit(lua_State* L, const Vector3& location) {
    if (lua_table_ref() < 0) return false;
    const int top = lua_gettop(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, lua_table_ref());
    const int tbl = lua_gettop(L);
    lua_pushstring(L, "OnTeleportUnit");
    lua_gettable(L, tbl);
    if (!lua_isfunction(L, -1)) {
        lua_settop(L, top);
        return false;
    }
    lua_pushvalue(L, tbl); // self
    lua_pushvalue(L, tbl); // teleporter: the unit teleports itself
    lua_newtable(L);       // location
    lua_pushnumber(L, location.x);
    lua_rawseti(L, -2, 1);
    lua_pushnumber(L, location.y);
    lua_rawseti(L, -2, 2);
    lua_pushnumber(L, location.z);
    lua_rawseti(L, -2, 3);
    lua_newtable(L);       // orientation: keep the current one
    const auto& q = orientation();
    lua_pushnumber(L, q.x);
    lua_rawseti(L, -2, 1);
    lua_pushnumber(L, q.y);
    lua_rawseti(L, -2, 2);
    lua_pushnumber(L, q.z);
    lua_rawseti(L, -2, 3);
    lua_pushnumber(L, q.w);
    lua_rawseti(L, -2, 4);
    if (lua_pcall(L, 4, 0, 0) != 0) {
        spdlog::warn("OnTeleportUnit error: {}", lua_tostring(L, -1));
    }
    lua_settop(L, top);
    return true;
}

void Unit::call_lua_method(lua_State* L, const char* method_name) {
    if (lua_table_ref() < 0) return;
    lua_rawgeti(L, LUA_REGISTRYINDEX, lua_table_ref());
    int tbl = lua_gettop(L);
    lua_pushstring(L, method_name);
    lua_gettable(L, tbl);
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, tbl); // self
        if (lua_pcall(L, 1, 0, 0) != 0) {
            spdlog::warn("{} error: {}", method_name, lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    } else {
        lua_pop(L, 1); // non-function
    }
    lua_pop(L, 1); // tbl
}

void Unit::call_lua_method_with_entity(lua_State* L, const char* method_name,
                                        Entity* arg_entity) {
    if (lua_table_ref() < 0) return;
    lua_rawgeti(L, LUA_REGISTRYINDEX, lua_table_ref());
    int tbl = lua_gettop(L);
    lua_pushstring(L, method_name);
    lua_gettable(L, tbl);
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, tbl); // self
        if (arg_entity && arg_entity->lua_table_ref() >= 0) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, arg_entity->lua_table_ref());
        } else {
            lua_pushnil(L);
        }
        if (lua_pcall(L, 2, 0, 0) != 0) {
            spdlog::warn("{} error: {}", method_name, lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    } else {
        lua_pop(L, 1); // non-function
    }
    lua_pop(L, 1); // tbl
}

// ---------------------------------------------------------------------------
// Manipulator system
// ---------------------------------------------------------------------------

Manipulator* Unit::add_manipulator(std::unique_ptr<Manipulator> m) {
    m->set_owner(this);
    auto* raw = m.get();
    manipulators_.push_back(std::move(m));
    return raw;
}

namespace {

/// Null the Lua table's pointer to `m` and drop the ref, before `m` is freed.
void detach_manipulator_table(lua_State* L, Manipulator& m) {
    if (!L || m.lua_table_ref() < 0) return;
    lua_rawgeti(L, LUA_REGISTRYINDEX, m.lua_table_ref());
    if (lua_istable(L, -1)) {
        lua_pushstring(L, "_c_object");
        lua_pushlightuserdata(L, nullptr);
        lua_rawset(L, -3);
    }
    lua_pop(L, 1);
    luaL_unref(L, LUA_REGISTRYINDEX, m.lua_table_ref());
    m.set_lua_table_ref(LUA_NOREF);
}

} // namespace

void Unit::release_manipulators(lua_State* L) {
    for (auto& m : manipulators_) detach_manipulator_table(L, *m);
    manipulators_.clear();
}

void Unit::release_weapon_scripts(lua_State* L) {
    if (!L) return;
    auto unref = [L](int& ref) {
        if (ref >= 0) luaL_unref(L, LUA_REGISTRYINDEX, ref);
        ref = LUA_NOREF;
    };
    for (i32 i = 0; i < weapon_count(); ++i) {
        auto* w = get_weapon(i);
        if (!w) continue;
        if (w->lua_table_ref >= 0) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, w->lua_table_ref);
            if (lua_istable(L, -1)) {
                // The weapon goes with its unit: its script OnDestroy runs
                // first (DefaultProjectileWeapon switches to DeadState,
                // ending its state thread).
                lua_pushstring(L, "OnDestroy");
                lua_gettable(L, -2);
                if (lua_isfunction(L, -1)) {
                    lua_pushvalue(L, -2);
                    if (lua_pcall(L, 1, 0, 0) != 0) {
                        spdlog::warn("Weapon OnDestroy error: {}", lua_tostring(L, -1));
                        lua_pop(L, 1);
                    }
                } else {
                    lua_pop(L, 1);
                }
                for (const char* key : {"_c_object", "_c_unit"}) {
                    lua_pushstring(L, key);
                    lua_pushlightuserdata(L, nullptr);
                    lua_rawset(L, -3);
                }
            }
            lua_pop(L, 1);
        }
        unref(w->lua_table_ref);
        unref(w->blueprint_ref);
        unref(w->weapon_priorities_ref);
    }
    clear_on_given_callbacks(L);
}

void Unit::remove_manipulator(Manipulator* m) {
    for (auto it = manipulators_.begin(); it != manipulators_.end(); ++it) {
        if (it->get() == m) {
            manipulators_.erase(it);
            return;
        }
    }
}

void Unit::tick_manipulators(f32 dt, lua_State* L) {
    // Reset bone matrices to identity before manipulators write their bones.
    // Each animator/rotator/slider writes only the bones it owns; unowned bones
    // stay at identity rather than carrying stale data from a previous tick.
    static constexpr std::array<f32, 16> IDENTITY = {
        1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    for (auto& m : animated_bone_matrices_) {
        m = IDENTITY;
    }

    // By index: a tick may wake script threads, and nothing here may assume
    // the vector is left untouched.
    for (size_t i = 0; i < manipulators_.size(); ++i) {
        Manipulator* m = manipulators_[i].get();
        if (m->is_destroyed() || !m->enabled()) continue;
        bool was_at_goal = m->is_at_goal();
        m->tick(dt);
        // If just reached goal and someone is waiting, wake the thread
        if (!was_at_goal && m->is_at_goal() && m->has_waiting_thread()) {
            // Look up ThreadManager from Lua registry
            lua_pushstring(L, "osc_thread_mgr");
            lua_rawget(L, LUA_REGISTRYINDEX);
            auto* tmgr = static_cast<ThreadManager*>(lua_touserdata(L, -1));
            lua_pop(L, 1);
            if (tmgr) {
                // Look up current tick from SimState in registry
                lua_pushstring(L, "osc_sim_state");
                lua_rawget(L, LUA_REGISTRYINDEX);
                auto* sim = static_cast<SimState*>(lua_touserdata(L, -1));
                lua_pop(L, 1);
                if (sim) tmgr->wake(*m, sim->tick_count());
            }
            m->clear_waiting_thread();
        }
    }
    // Free destroyed manipulators, detaching their Lua tables first.
    for (auto& m : manipulators_) {
        if (m->is_destroyed()) detach_manipulator_table(L, *m);
    }
    manipulators_.erase(
        std::remove_if(manipulators_.begin(), manipulators_.end(),
                        [](const auto& m) { return m->is_destroyed(); }),
        manipulators_.end());
    update_pose();
}

void Unit::update_pose() {
    const BoneData* bd = bone_data();
    if (!bd || manipulators_.empty()) {
        pose_.clear();
        return;
    }
    const size_t count = bd->bones.size();
    // Lower precedence first; each manipulator applies on top of the ones
    // before it (an animator sets bones, aim and rotators turn them).
    std::vector<Manipulator*>& order = pose_order_;
    order.clear();
    for (const auto& m : manipulators_) {
        if (!m->is_destroyed() && m->enabled()) order.push_back(m.get());
    }
    std::stable_sort(order.begin(), order.end(), [](const Manipulator* a, const Manipulator* b) {
        return a->precedence() < b->precedence();
    });
    PoseLocals& locals = pose_locals_;
    locals.changed = false;
    locals.local.resize(count);
    for (size_t i = 0; i < count; ++i)
        locals.local[i] = {bd->bones[i].local_position, bd->bones[i].local_rotation};
    for (Manipulator* m : order) m->apply_pose(locals);
    if (!locals.changed) {
        pose_.clear(); // the bind pose; the render matrices stay identity
        return;
    }
    // Bones are stored parents first (see the SCM parser).
    pose_.resize(count);
    for (size_t i = 0; i < count; ++i) {
        const i32 parent = bd->bones[i].parent_index;
        pose_[i] = parent < 0 || static_cast<size_t>(parent) >= i
                       ? locals.local[i]
                       : pose_compose(pose_[static_cast<size_t>(parent)], locals.local[i]);
    }
    // What the renderer skins with: each bone's posed transform times its
    // inverse bind pose (identity where a bone is at rest).
    if (animated_bone_matrices_.size() == count) {
        for (size_t i = 0; i < count; ++i) {
            f32 posed[16];
            pose_to_mat4(posed, pose_[i]);
            mat4_multiply(animated_bone_matrices_[i].data(), posed,
                          bd->bones[i].inverse_bind_pose.data());
        }
    }
}

BonePose Unit::bone_pose(i32 bone) const {
    const BoneData* bd = bone_data();
    if (!bd || !bd->is_valid(bone)) return {};
    if (static_cast<size_t>(bone) < pose_.size()) return pose_[static_cast<size_t>(bone)];
    const BoneInfo& info = bd->bones[static_cast<size_t>(bone)];
    return {info.world_position, info.world_rotation};
}

Vector3 Unit::bone_world_position(i32 bone) const {
    const BoneData* bd = bone_data();
    if (!bd || !bd->is_valid(bone)) return position();
    const Vector3 model = bone_pose(bone).position;
    const f32 s = bd->model_scale;
    const Vector3 offset =
        quat_rotate(orientation(), Vector3{model.x * s, model.y * s, model.z * s});
    return {position().x + offset.x, position().y + offset.y, position().z + offset.z};
}

Vector3 Unit::bone_world_forward(i32 bone) const {
    constexpr Vector3 kAhead{0.0f, 0.0f, 1.0f};
    const BoneData* bd = bone_data();
    if (!bd || !bd->is_valid(bone)) return quat_rotate(orientation(), kAhead);
    return quat_rotate(orientation(), quat_rotate(bone_pose(bone).rotation, kAhead));
}

// ---------------------------------------------------------------------------
// Silo (M206)
// ---------------------------------------------------------------------------

namespace {

/// self:method(weapon) on the unit's script, recording a failure in tests.
void call_with_weapon(lua_State* L, const Unit& unit, const char* method, const Weapon& weapon) {
    if (!L || unit.lua_table_ref() < 0) return;
    const int top = lua_gettop(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, unit.lua_table_ref());
    const int self = lua_gettop(L);
    lua_pushstring(L, method);
    lua_gettable(L, self);
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, self);
        if (weapon.lua_table_ref >= 0) lua_rawgeti(L, LUA_REGISTRYINDEX, weapon.lua_table_ref);
        else lua_pushnil(L);
        if (lua_pcall(L, 2, 0, 0) != 0) {
            const char* err = lua_tostring(L, -1);
            const std::string message =
                std::string(method) + " error: " + (err ? err : "(unknown)");
            spdlog::warn("{}", message);
            if (test_status::count_lua_failures()) test_status::record_failure(message);
        }
    }
    lua_settop(L, top);
}

/// self:method() on the unit's script as a number: `fallback` when it has no
/// such method or it fails.
f64 script_number(lua_State* L, const Unit& unit, const char* method, f64 fallback) {
    if (!L || unit.lua_table_ref() < 0) return fallback;
    const int top = lua_gettop(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, unit.lua_table_ref());
    const int self = lua_gettop(L);
    lua_pushstring(L, method);
    lua_gettable(L, self);
    f64 value = fallback;
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, self);
        if (lua_pcall(L, 1, 1, 0) == 0 && lua_type(L, -1) == LUA_TNUMBER)
            value = lua_tonumber(L, -1);
    }
    lua_settop(L, top);
    return value;
}

/// A projectile blueprint's Economy: its build time and costs (zero when
/// it has none).
struct MissileCost {
    f64 build_time = 0;
    f64 energy = 0;
    f64 mass = 0;
};
MissileCost missile_cost(lua_State* L, const std::string& projectile_bp_id) {
    MissileCost cost;
    if (!L || projectile_bp_id.empty()) return cost;
    const int top = lua_gettop(L);
    lua_pushstring(L, "__blueprints");
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (lua_istable(L, -1)) {
        lua_pushstring(L, projectile_bp_id.c_str());
        lua_rawget(L, -2);
    }
    if (lua_istable(L, -1)) {
        lua_pushstring(L, "Economy");
        lua_rawget(L, -2);
    }
    if (lua_istable(L, -1)) {
        const int econ = lua_gettop(L);
        for (auto [key, out] :
             {std::pair{"BuildTime", &cost.build_time}, std::pair{"BuildCostEnergy", &cost.energy},
              std::pair{"BuildCostMass", &cost.mass}}) {
            lua_pushstring(L, key);
            lua_rawget(L, econ);
            if (lua_type(L, -1) == LUA_TNUMBER) *out = lua_tonumber(L, -1);
            lua_pop(L, 1);
        }
    }
    lua_settop(L, top);
    return cost;
}

} // namespace

Weapon* Unit::silo_weapon(bool nuke) const {
    for (const auto& w : weapons_) {
        if (w->enabled && w->counted_projectile && w->nuke_weapon == nuke) return w.get();
    }
    return nullptr;
}

i32 Unit::silo_max_storage(bool nuke) const {
    const Weapon* w = silo_weapon(nuke);
    return w ? w->max_projectile_storage : 0;
}

i32 Unit::silo_build_count(bool nuke) const {
    return static_cast<i32>(std::count(silo_orders_.begin(), silo_orders_.end(), nuke));
}

void Unit::update_silo(f64 dt, f32 efficiency, lua_State* L) {
    if (!silo_building()) {
        // A paused unit starts nothing.
        if (paused_) return;
        // Ordered builds come first, oldest first (one for a kind the unit
        // can't build is dropped); in auto mode, then, a kind with room.
        while (!silo_orders_.empty() && !silo_weapon(silo_orders_.front()))
            silo_orders_.pop_front();
        std::optional<bool> kind;
        if (!silo_orders_.empty()) {
            kind = silo_orders_.front();
        } else if (auto_mode_) {
            for (const bool nuke : {true, false}) {
                if (silo_weapon(nuke) && silo_ammo(nuke) < silo_max_storage(nuke)) {
                    kind = nuke;
                    break;
                }
            }
        }
        // A full storage waits for a missile to be fired.
        if (!kind || silo_ammo(*kind) >= silo_max_storage(*kind)) return;
        Weapon* weapon = silo_weapon(*kind);
        const MissileCost cost = missile_cost(L, weapon->projectile_bp_id);
        if (cost.build_time <= 0) {
            // Nothing to build: its projectile has no Economy.
            if (!silo_orders_.empty()) silo_orders_.pop_front();
            return;
        }
        silo_build_ = {weapon->weapon_index, *kind, 0.0, cost.build_time, cost.energy, cost.mass};
        set_unit_state("SiloBuildingAmmo", true);
        call_with_weapon(L, *this, "OnSiloBuildStart", *weapon);
        if (destroyed() || !silo_building()) return;
        // Its first block goes down next tick, once the economy has its
        // request.
    } else {
        // Its weapon switched off: the missile is abandoned (the builds
        // ordered wait for a weapon; retail's scripts StopSiloBuild when an
        // enhancement removes one).
        const auto index = static_cast<size_t>(silo_build_.weapon);
        if (index >= weapons_.size() || !weapons_[index]->enabled) {
            abandon_silo_build();
            return;
        }
        if (paused_ || build_rate_ <= 0) {
            economy_.silo_energy = 0;
            economy_.silo_mass = 0;
            return;
        }
        silo_build_.progress =
            std::min(1.0, silo_build_.progress + static_cast<f64>(build_rate_) * dt /
                                                     silo_build_.build_time * efficiency);
        if (!is_building() && !enhancing_) work_progress_ = static_cast<f32>(silo_build_.progress);
        // Done, allowing for the rounding of its blocks' sum.
        if (silo_build_.progress >= 1.0 - 1e-9) {
            // Done: into storage, and the order it was for is met.
            const bool nuke = silo_build_.nuke;
            give_silo_ammo(nuke, 1);
            spdlog::info("Silo: {} #{} built a {} missile ({} stored)", unit_id_, entity_id(),
                         nuke ? "nuclear" : "tactical", silo_ammo(nuke));
            if (!silo_orders_.empty() && silo_orders_.front() == nuke) silo_orders_.pop_front();
            end_silo_build(L);
            return;
        }
    }
    if (paused_ || build_rate_ <= 0) return;
    // What it asks of the economy: the missile's cost over its build time at
    // the unit's build rate, as adjacency adjusts it (the engine asks the
    // unit's script for its EnergyBuildAdjMod and MassBuildAdjMod).
    const f64 per_second = static_cast<f64>(build_rate_) / silo_build_.build_time;
    economy_.silo_energy =
        silo_build_.energy * script_number(L, *this, "GetEnergyBuildAdjMod", 1.0) * per_second;
    economy_.silo_mass =
        silo_build_.mass * script_number(L, *this, "GetMassBuildAdjMod", 1.0) * per_second;
}

void Unit::abandon_silo_build() {
    if (!silo_building()) return;
    silo_build_ = {};
    economy_.silo_energy = 0;
    economy_.silo_mass = 0;
    set_unit_state("SiloBuildingAmmo", false);
    if (!is_building() && !enhancing_) work_progress_ = 0;
}

void Unit::end_silo_build(lua_State* L) {
    if (!silo_building()) return;
    const auto index = static_cast<size_t>(silo_build_.weapon);
    abandon_silo_build();
    if (index < weapons_.size()) call_with_weapon(L, *this, "OnSiloBuildEnd", *weapons_[index]);
}

void Unit::stop_silo_build() {
    silo_orders_.clear();
    abandon_silo_build();
}

void Unit::assist_silo_build(f32 rate, f64 dt, f32 efficiency) {
    if (!silo_building() || paused_) return;
    silo_build_.progress =
        std::min(1.0, silo_build_.progress +
                          static_cast<f64>(rate) * dt / silo_build_.build_time * efficiency);
}

Weapon* Unit::overcharge_weapon() const {
    for (const auto& w : weapons_) {
        if (w->overcharge) return w.get();
    }
    return nullptr;
}

void Unit::settle_interrupted_orders(lua_State* L) {
    const UnitCommand* head = command_queue_.empty() ? nullptr : &command_queue_.front();
    if (teleporting_ && !(head && head->type == CommandType::Teleport && head->started)) {
        teleporting_ = false;
        call_lua_method(L, "OnFailedTeleport");
        if (destroyed() || !in_registry()) return;
        head = command_queue_.empty() ? nullptr : &command_queue_.front();
    }
    if (overcharge_armed_ &&
        !(head && head->type == CommandType::Overcharge && head->started && !head->launched)) {
        overcharge_armed_ = false;
        // Its script switches it off once it has fired; one left on goes off.
        if (Weapon* oc = overcharge_weapon(); oc && oc->enabled && L)
            oc->call_script(L, "OnDisableWeapon");
    }
}

Weapon* Unit::launch_weapon(bool nuke) const {
    for (const auto& w : weapons_) {
        if (w->enabled && w->manual_fire && w->counted_projectile && !w->overcharge &&
            w->nuke_weapon == nuke)
            return w.get();
    }
    return nullptr;
}

const UnitCommand* Unit::launch_order_for(const Weapon& w) const {
    if (command_queue_.empty()) return nullptr;
    const UnitCommand& head = command_queue_.front();
    if (head.launched) return nullptr;
    if (head.type == CommandType::Overcharge)
        return head.started && overcharge_weapon() == &w ? &head : nullptr;
    if (head.type != CommandType::Nuke && head.type != CommandType::Tactical) return nullptr;
    return head.in_band && launch_weapon(head.type == CommandType::Nuke) == &w ? &head : nullptr;
}

UnitCommand* Unit::launch_order_for(const Weapon& w) {
    return const_cast<UnitCommand*>(std::as_const(*this).launch_order_for(w));
}

void Unit::destroy_all_manipulators() {
    manipulators_.clear();
}

void Unit::init_animated_bones() {
    if (!bone_data()) return;
    i32 count = bone_data()->bone_count();
    if (count <= 0) return;
    animated_bone_matrices_.resize(static_cast<size_t>(count));
    // Fill with identity matrices (column-major)
    static constexpr std::array<f32, 16> IDENTITY = {
        1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    for (auto& m : animated_bone_matrices_) {
        m = IDENTITY;
    }
}

void Unit::record_damage(u32 attacker_id, f32 amount) {
    for (auto& [id, dmg] : damage_contributions_) {
        if (id == attacker_id) { dmg += amount; return; }
    }
    damage_contributions_.emplace_back(attacker_id, amount);
}

void Unit::add_xp(f32 amount, lua_State* L, EntityRegistry& registry) {
    (void)registry;
    if (amount <= 0 || vet_level_ >= 5) return;
    vet_xp_ += amount;

    // Check for level-up (can gain multiple levels at once)
    while (vet_level_ < 5 &&
           vet_thresholds_[vet_level_] > 0 &&
           vet_xp_ >= vet_thresholds_[vet_level_]) {
        vet_level_++;

        // Apply per-level buffs from blueprint
        if (L) {
            apply_vet_buffs(L);
            fire_on_veteran(L);
        }

        spdlog::debug("Unit #{} leveled up to vet level {}", entity_id(), vet_level_);
    }
}

void Unit::apply_vet_buffs(lua_State* L) {
    // Read buff values from blueprint: Buffs.Regen/MaxHealth/Damage.Level{N}
    lua_pushstring(L, "__blueprints");
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return; }

    lua_pushstring(L, blueprint_id().c_str());
    lua_rawget(L, -2);
    if (!lua_istable(L, -1)) { lua_pop(L, 2); return; }

    lua_pushstring(L, "Buffs");
    lua_gettable(L, -2);
    if (!lua_istable(L, -1)) { lua_pop(L, 3); return; }

    char level_key[24]; // "Level" + any int + NUL
    snprintf(level_key, sizeof(level_key), "Level%d", vet_level_);

    // Regen buff: flat increase
    lua_pushstring(L, "Regen");
    lua_gettable(L, -2);
    if (lua_istable(L, -1)) {
        lua_pushstring(L, level_key);
        lua_gettable(L, -2);
        if (lua_isnumber(L, -1)) {
            set_regen_rate(regen_rate() + static_cast<f32>(lua_tonumber(L, -1)));
        }
        lua_pop(L, 1); // level value
    }
    lua_pop(L, 1); // Regen

    // MaxHealth buff: multiplier (e.g., 1.1 = +10%)
    lua_pushstring(L, "MaxHealth");
    lua_gettable(L, -2);
    if (lua_istable(L, -1)) {
        lua_pushstring(L, level_key);
        lua_gettable(L, -2);
        if (lua_isnumber(L, -1)) {
            f32 factor = static_cast<f32>(lua_tonumber(L, -1));
            if (factor > 0) {
                f32 old_max = max_health();
                f32 new_max = old_max * factor;
                set_max_health(new_max);
                // Heal the difference so unit gains the new HP
                set_health(health() + (new_max - old_max));
            }
        }
        lua_pop(L, 1); // level value
    }
    lua_pop(L, 1); // MaxHealth

    // Damage buff: multiplier (e.g., 1.1 = +10%)
    lua_pushstring(L, "Damage");
    lua_gettable(L, -2);
    if (lua_istable(L, -1)) {
        lua_pushstring(L, level_key);
        lua_gettable(L, -2);
        if (lua_isnumber(L, -1)) {
            f32 factor = static_cast<f32>(lua_tonumber(L, -1));
            if (factor > 0) {
                damage_multiplier_ *= factor;
            }
        }
        lua_pop(L, 1); // level value
    }
    lua_pop(L, 1); // Damage

    lua_pop(L, 3); // Buffs + bp + __blueprints
}

void Unit::fire_on_veteran(lua_State* L) {
    if (lua_table_ref() < 0) return;
    lua_rawgeti(L, LUA_REGISTRYINDEX, lua_table_ref());
    int tbl = lua_gettop(L);
    lua_pushstring(L, "OnVeteran");
    lua_gettable(L, tbl);
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, tbl); // self
        if (lua_pcall(L, 1, 0, 0) != 0) {
            spdlog::warn("OnVeteran error: {}", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    } else {
        lua_pop(L, 1); // not a function
    }
    lua_pop(L, 1); // unit table
}

} // namespace osc::sim
