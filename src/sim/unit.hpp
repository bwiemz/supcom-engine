#pragma once

#include "sim/entity.hpp"
#include "sim/navigator.hpp"
#include "sim/unit_command.hpp"
#include "sim/weapon.hpp"

#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct lua_State;

namespace osc::map {
class PathfindingGrid;
}

namespace osc::sim {

struct SimContext;
class EntityRegistry;

struct IntelState {
    f32 radius = 0;
    bool enabled = false;
};

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

    // Script bits (9 toggles, bits 0-8)
    bool get_script_bit(i32 bit) const {
        return (bit >= 0 && bit <= 8) ? ((script_bits_ >> bit) & 1) != 0 : false;
    }
    void set_script_bit(i32 bit, bool value) {
        if (bit < 0 || bit > 8) return;
        if (value) script_bits_ |= static_cast<u16>(1u << bit);
        else       script_bits_ &= static_cast<u16>(~(1u << bit));
    }
    void toggle_script_bit(i32 bit) {
        if (bit >= 0 && bit <= 8)
            script_bits_ ^= static_cast<u16>(1u << bit);
    }

    // Toggle caps (which RULEUTC_* toggles this unit supports)
    bool has_toggle_cap(const std::string& cap) const {
        return toggle_caps_.count(cap) > 0;
    }
    void add_toggle_cap(const std::string& cap) { toggle_caps_.insert(cap); }
    void remove_toggle_cap(const std::string& cap) { toggle_caps_.erase(cap); }

    // Layer change with Lua OnLayerChange(new, old) callback
    void set_layer_with_callback(const std::string& new_layer, lua_State* L);

    // Threat levels (cached from blueprint Defense at creation time)
    f32 surface_threat() const { return surface_threat_; }
    f32 air_threat() const { return air_threat_; }
    f32 sub_threat() const { return sub_threat_; }
    f32 economy_threat() const { return economy_threat_; }
    void set_surface_threat(f32 t) { surface_threat_ = t; }
    void set_air_threat(f32 t) { air_threat_ = t; }
    void set_sub_threat(f32 t) { sub_threat_ = t; }
    void set_economy_threat(f32 t) { economy_threat_ = t; }

    // Command queue
    const std::deque<UnitCommand>& command_queue() const {
        return command_queue_;
    }
    void push_command(const UnitCommand& cmd, bool clear_existing);
    void clear_commands();

    // Footprint (from blueprint, for pathfinding obstacle marking)
    f32 footprint_size_x() const { return footprint_size_x_; }
    f32 footprint_size_z() const { return footprint_size_z_; }
    void set_footprint_size(f32 sx, f32 sz) { footprint_size_x_ = sx; footprint_size_z_ = sz; }

    /// Per-tick update: process command queue + movement + weapons.
    void update(f64 dt, SimContext& ctx);

    /// Build helpers called from update()
    bool start_build(const UnitCommand& cmd, EntityRegistry& registry,
                     lua_State* L);
    bool progress_build(f64 dt, EntityRegistry& registry, lua_State* L,
                         map::PathfindingGrid* grid = nullptr);
    void finish_build(EntityRegistry& registry, lua_State* L, bool success,
                      map::PathfindingGrid* grid = nullptr);

    /// Assist helpers (Guard command)
    void stop_assisting();
    bool progress_build_assist(f64 dt, EntityRegistry& registry);

    /// Reclaim helpers
    u32 reclaim_target_id() const { return reclaim_target_id_; }
    bool is_reclaiming() const { return reclaim_target_id_ != 0; }
    void stop_reclaiming();
    bool progress_reclaim(f64 dt, EntityRegistry& registry, lua_State* L);
    bool progress_reclaim_assist(f64 dt, EntityRegistry& registry);

    /// Repair helpers
    u32 repair_target_id() const { return repair_target_id_; }
    bool is_repairing() const { return repair_target_id_ != 0; }
    bool start_repair(const UnitCommand& cmd, EntityRegistry& registry, lua_State* L);
    bool progress_repair(f64 dt, EntityRegistry& registry, lua_State* L);
    void stop_repairing(lua_State* L, EntityRegistry& registry);

    /// Capture helpers
    u32 capture_target_id() const { return capture_target_id_; }
    bool is_capturing() const { return capture_target_id_ != 0; }
    bool capturable() const { return capturable_; }
    void set_capturable(bool c) { capturable_ = c; }
    bool start_capture(const UnitCommand& cmd, EntityRegistry& registry, lua_State* L);
    bool progress_capture(f64 dt, EntityRegistry& registry, lua_State* L);
    void stop_capturing(lua_State* L, EntityRegistry& registry, bool failed);

    /// Enhancement helpers
    bool has_enhancement(const std::string& enh) const;
    void add_enhancement(const std::string& slot, const std::string& enh);
    void remove_enhancement(const std::string& enh);
    const std::unordered_map<std::string, std::string>& enhancements() const { return enhancements_; }
    bool is_enhancing() const { return enhancing_; }
    const std::string& enhance_name() const { return enhance_name_; }
    bool start_enhance(const UnitCommand& cmd, lua_State* L);
    bool progress_enhance(f64 dt, lua_State* L);
    void finish_enhance(lua_State* L);
    void cancel_enhance(lua_State* L);

    // Immobile flag (set during enhancement)
    bool immobile() const { return immobile_; }
    void set_immobile(bool b) { immobile_ = b; }

    // Unit states (generic string-based state tracking)
    bool has_unit_state(const std::string& state) const { return unit_states_.count(state) > 0; }
    void set_unit_state(const std::string& state, bool v) {
        if (v) unit_states_.insert(state);
        else   unit_states_.erase(state);
    }

    // Intel system (per-type enabled/disabled + radius)
    bool is_intel_enabled(const std::string& type) const;
    f32 get_intel_radius(const std::string& type) const;
    void init_intel(const std::string& type, f32 radius);
    void enable_intel(const std::string& type);
    void disable_intel(const std::string& type);
    void set_intel_radius(const std::string& type, f32 radius);

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
    u32 repair_target_id_ = 0;    // entity ID of unit being repaired
    f64 repair_build_time_ = 0;   // target's Economy.BuildTime
    f64 repair_cost_mass_ = 0;    // target's Economy.BuildCostMass
    f64 repair_cost_energy_ = 0;  // target's Economy.BuildCostEnergy
    u32 capture_target_id_ = 0;   // entity ID of unit being captured
    f64 capture_time_ = 0;        // total seconds to capture
    f64 capture_energy_cost_ = 0; // total energy drain
    bool capturable_ = true;      // can this unit be captured?
    f32 footprint_size_x_ = 0;    // from blueprint Footprint.SizeX
    f32 footprint_size_z_ = 0;    // from blueprint Footprint.SizeZ
    bool busy_ = false;
    bool block_command_queue_ = false;
    i32 fire_state_ = 0;         // 0=ReturnFire, 1=HoldFire, 2=HoldGround
    u16 script_bits_ = 0;        // 9 toggle bits (0-8)
    std::unordered_set<std::string> toggle_caps_; // RULEUTC_* toggle capabilities
    f32 surface_threat_ = 0;
    f32 air_threat_ = 0;
    f32 sub_threat_ = 0;
    f32 economy_threat_ = 0;
    // Enhancement system
    std::unordered_map<std::string, std::string> enhancements_; // slot → enh name
    bool enhancing_ = false;
    f64 enhance_build_time_ = 0;
    std::string enhance_name_;
    bool immobile_ = false;
    std::unordered_set<std::string> unit_states_; // generic string-based states
    // Intel system
    std::unordered_map<std::string, IntelState> intel_states_;
};

} // namespace osc::sim
