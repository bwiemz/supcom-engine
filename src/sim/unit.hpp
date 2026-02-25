#pragma once

#include "sim/entity.hpp"
#include "sim/navigator.hpp"
#include "sim/unit_command.hpp"
#include "sim/weapon.hpp"

#include <deque>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

struct lua_State;

namespace osc::sim {

class EntityRegistry;

struct UnitEconomy {
    f64 production_mass = 0.0;
    f64 production_energy = 0.0;
    f64 consumption_mass = 0.0;
    f64 consumption_energy = 0.0;
    bool production_active = false;
    bool consumption_active = false;
    bool maintenance_active = false;
    f64 energy_maintenance_override = -1.0; // negative = not set
    f64 storage_mass = 0.0;
    f64 storage_energy = 0.0;
};

class Unit : public Entity {
public:
    bool is_unit() const override { return true; }

    const std::string& unit_id() const { return unit_id_; }
    void set_unit_id(const std::string& id) { unit_id_ = id; }

    i32 weapon_count() const {
        return static_cast<i32>(weapons_.size());
    }
    f32 build_rate() const { return build_rate_; }
    void set_build_rate(f32 r) { build_rate_ = r; }

    const std::string& layer() const { return layer_; }
    void set_layer(const std::string& l) { layer_ = l; }

    bool is_being_built() const { return is_being_built_; }
    void set_is_being_built(bool b) { is_being_built_ = b; }

    f32 max_speed() const { return max_speed_; }
    void set_max_speed(f32 s) { max_speed_ = s; }

    // Navigation
    Navigator& navigator() { return navigator_; }
    const Navigator& navigator() const { return navigator_; }
    bool is_moving() const { return navigator_.is_moving(); }

    // Economy
    UnitEconomy& economy() { return economy_; }
    const UnitEconomy& economy() const { return economy_; }

    // Weapons
    void add_weapon(std::unique_ptr<Weapon> w);
    Weapon* get_weapon(i32 index);
    const std::vector<std::unique_ptr<Weapon>>& weapons() const {
        return weapons_;
    }

    // Categories (cached from blueprint CategoriesHash at creation time)
    const std::unordered_set<std::string>& categories() const {
        return categories_;
    }
    bool has_category(const std::string& cat) const {
        return categories_.count(cat) > 0;
    }
    void add_category(std::string cat) {
        categories_.insert(std::move(cat));
    }

    // Rally point (factories send produced units here)
    bool has_rally_point() const { return has_rally_point_; }
    const Vector3& rally_point() const { return rally_point_; }
    void set_rally_point(const Vector3& p) { rally_point_ = p; has_rally_point_ = true; }
    void clear_rally_point() { rally_point_ = {}; has_rally_point_ = false; }

    // Build state (builder side) — tracks what this unit is constructing
    u32 build_target_id() const { return build_target_id_; }
    void set_build_target_id(u32 id) { build_target_id_ = id; }
    bool is_building() const { return build_target_id_ != 0; }

    f64 build_time() const { return build_time_; }
    void set_build_time(f64 t) { build_time_ = t; }
    f64 build_cost_mass() const { return build_cost_mass_; }
    void set_build_cost_mass(f64 c) { build_cost_mass_ = c; }
    f64 build_cost_energy() const { return build_cost_energy_; }
    void set_build_cost_energy(f64 c) { build_cost_energy_ = c; }

    // Work progress (generic — used by build, upgrade, capture, etc.)
    f32 work_progress() const { return work_progress_; }
    void set_work_progress(f32 p) { work_progress_ = p; }

    // State flags
    bool busy() const { return busy_; }
    void set_busy(bool b) { busy_ = b; }

    bool block_command_queue() const { return block_command_queue_; }
    void set_block_command_queue(bool b) { block_command_queue_ = b; }

    i32 fire_state() const { return fire_state_; }
    void set_fire_state(i32 s) { fire_state_ = s; }

    // Command queue
    const std::deque<UnitCommand>& command_queue() const {
        return command_queue_;
    }
    void push_command(const UnitCommand& cmd, bool clear_existing);
    void clear_commands();

    /// Per-tick update: process command queue + movement + weapons.
    void update(f64 dt, EntityRegistry& registry, lua_State* L);

    /// Build helpers called from update()
    bool start_build(const UnitCommand& cmd, EntityRegistry& registry,
                     lua_State* L);
    bool progress_build(f64 dt, EntityRegistry& registry, lua_State* L);
    void finish_build(EntityRegistry& registry, lua_State* L, bool success);

    /// Assist helpers (Guard command)
    void stop_assisting();
    bool progress_build_assist(f64 dt, EntityRegistry& registry);

    /// Reclaim helpers
    u32 reclaim_target_id() const { return reclaim_target_id_; }
    bool is_reclaiming() const { return reclaim_target_id_ != 0; }
    void stop_reclaiming();
    bool progress_reclaim(f64 dt, EntityRegistry& registry, lua_State* L);
    bool progress_reclaim_assist(f64 dt, EntityRegistry& registry);

private:
    void call_on_reclaimed(u32 target_id, EntityRegistry& registry, lua_State* L);

    std::string unit_id_;
    f32 build_rate_ = 1.0f;
    std::string layer_ = "Land";
    bool is_being_built_ = false;
    f32 max_speed_ = 0;
    Navigator navigator_;
    UnitEconomy economy_;
    std::unordered_set<std::string> categories_;
    std::deque<UnitCommand> command_queue_;
    std::vector<std::unique_ptr<Weapon>> weapons_;
    Vector3 rally_point_;
    bool has_rally_point_ = false;
    u32 build_target_id_ = 0;     // entity ID of unit being built
    f64 build_time_ = 0;          // target's Economy.BuildTime
    f64 build_cost_mass_ = 0;     // target's Economy.BuildCostMass
    f64 build_cost_energy_ = 0;   // target's Economy.BuildCostEnergy
    f32 work_progress_ = 0.0f;
    u32 reclaim_target_id_ = 0;   // entity ID being reclaimed
    f32 reclaim_rate_ = 0;        // fraction_complete decrease per second
    bool busy_ = false;
    bool block_command_queue_ = false;
    i32 fire_state_ = 0;         // 0=ReturnFire, 1=HoldFire, 2=HoldGround
};

} // namespace osc::sim
