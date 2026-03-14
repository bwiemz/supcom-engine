#pragma once

#include "sim/entity.hpp"
#include "sim/navigator.hpp"
#include "sim/unit_command.hpp"
#include "sim/weapon.hpp"

#include <array>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace osc::sim { class Manipulator; }

struct lua_State;

namespace osc::map {
class PathfindingGrid;
class Terrain;
}

namespace osc::sim {

struct SimContext;
class EntityRegistry;

struct IntelState {
    f32 radius = 0;
    bool enabled = false;
};

struct BuildQueueEntry {
    std::string blueprint_id;
    int count = 1;
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

    const std::string& armor_type() const { return armor_type_; }
    void set_armor_type(const std::string& t) { armor_type_ = t; }

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

    // Pause state
    bool is_paused() const { return paused_; }
    void set_paused(bool p) { paused_ = p; }

    // Shield back-reference (entity ID, set by _c_CreateShield)
    u32 shield_entity_id() const { return shield_entity_id_; }
    void set_shield_entity_id(u32 id) { shield_entity_id_ = id; }

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

    // Build queue (factory production queue)
    std::vector<BuildQueueEntry>& build_queue() { return build_queue_; }
    const std::vector<BuildQueueEntry>& build_queue() const { return build_queue_; }

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

    /// Lua callback helpers: call self:method() or self:method(entity)
    void call_lua_method(lua_State* L, const char* method_name);
    void call_lua_method_with_entity(lua_State* L, const char* method_name,
                                      Entity* arg_entity);

    /// Build helpers called from update()
    bool start_build(const UnitCommand& cmd, EntityRegistry& registry,
                     lua_State* L);
    bool progress_build(f64 dt, EntityRegistry& registry, lua_State* L,
                         map::PathfindingGrid* grid = nullptr,
                         f32 efficiency = 1.0f);
    void finish_build(EntityRegistry& registry, lua_State* L, bool success,
                      map::PathfindingGrid* grid = nullptr);

    /// Assist helpers (Guard command)
    void stop_assisting();
    bool progress_build_assist(f64 dt, EntityRegistry& registry,
                                f32 efficiency = 1.0f);

    /// Reclaim helpers
    u32 reclaim_target_id() const { return reclaim_target_id_; }
    void set_reclaim_target_id(u32 id) { reclaim_target_id_ = id; }
    bool is_reclaiming() const { return reclaim_target_id_ != 0; }
    void stop_reclaiming();
    bool progress_reclaim(f64 dt, EntityRegistry& registry, lua_State* L);
    bool progress_reclaim_assist(f64 dt, EntityRegistry& registry);

    /// Repair helpers
    u32 repair_target_id() const { return repair_target_id_; }
    void set_repair_target_id(u32 id) { repair_target_id_ = id; }
    bool is_repairing() const { return repair_target_id_ != 0; }
    bool start_repair(const UnitCommand& cmd, EntityRegistry& registry, lua_State* L);
    bool progress_repair(f64 dt, EntityRegistry& registry, lua_State* L,
                          f32 efficiency = 1.0f);
    void stop_repairing(lua_State* L, EntityRegistry& registry);

    /// Capture helpers
    u32 capture_target_id() const { return capture_target_id_; }
    void set_capture_target_id(u32 id) { capture_target_id_ = id; }
    bool is_capturing() const { return capture_target_id_ != 0; }
    bool is_being_captured() const { return being_captured_; }
    void set_being_captured(bool v) { being_captured_ = v; }
    bool capturable() const { return capturable_; }
    void set_capturable(bool c) { capturable_ = c; }
    bool start_capture(const UnitCommand& cmd, EntityRegistry& registry, lua_State* L);
    bool progress_capture(f64 dt, EntityRegistry& registry, lua_State* L,
                           f32 efficiency = 1.0f);
    void stop_capturing(lua_State* L, EntityRegistry& registry, bool failed);

    /// Enhancement helpers
    bool has_enhancement(const std::string& enh) const;
    void add_enhancement(const std::string& slot, const std::string& enh);
    void remove_enhancement(const std::string& enh);
    const std::unordered_map<std::string, std::string>& enhancements() const { return enhancements_; }
    bool is_enhancing() const { return enhancing_; }
    const std::string& enhance_name() const { return enhance_name_; }
    bool start_enhance(const UnitCommand& cmd, lua_State* L);
    bool progress_enhance(f64 dt, lua_State* L, f32 efficiency = 1.0f);
    void finish_enhance(lua_State* L);
    void cancel_enhance(lua_State* L);

    // Veterancy level (0-5, set from Lua VeterancyComponent)
    u8 vet_level() const { return vet_level_; }
    void set_vet_level(u8 level) { vet_level_ = level; }

    // Stats/telemetry system
    void set_stat(const std::string& key, f64 value);
    f64 get_stat(const std::string& key, f64 default_val = 0) const;
    bool has_stat(const std::string& key) const;

    // Silo ammo system (nuke + tactical missile counters)
    i32 nuke_silo_ammo() const { return nuke_silo_ammo_; }
    i32 tactical_silo_ammo() const { return tactical_silo_ammo_; }
    void give_nuke_silo_ammo(i32 amount) { nuke_silo_ammo_ += amount; if (nuke_silo_ammo_ < 0) nuke_silo_ammo_ = 0; }
    void give_tactical_silo_ammo(i32 amount) { tactical_silo_ammo_ += amount; if (tactical_silo_ammo_ < 0) tactical_silo_ammo_ = 0; }
    void remove_nuke_silo_ammo(i32 amount) { nuke_silo_ammo_ -= amount; if (nuke_silo_ammo_ < 0) nuke_silo_ammo_ = 0; }
    void remove_tactical_silo_ammo(i32 amount) { tactical_silo_ammo_ -= amount; if (tactical_silo_ammo_ < 0) tactical_silo_ammo_ = 0; }

    // Immobile flag (set during enhancement)
    bool immobile() const { return immobile_; }
    void set_immobile(bool b) { immobile_ = b; }

    // Unit states (generic string-based state tracking)
    bool has_unit_state(const std::string& state) const { return unit_states_.count(state) > 0; }
    void set_unit_state(const std::string& state, bool v) {
        if (v) unit_states_.insert(state);
        else   unit_states_.erase(state);
    }

    // Shield ratio (health bar, set by shield's UpdateShieldRatio)
    f32 shield_ratio() const { return shield_ratio_; }
    void set_shield_ratio(f32 r) { shield_ratio_ = r; }

    // Transport system
    const std::vector<u32>& cargo_ids() const { return cargo_ids_; }
    void add_cargo(u32 id) { cargo_ids_.push_back(id); }
    void remove_cargo(u32 id);
    void clear_cargo() { cargo_ids_.clear(); }

    u32 transport_id() const { return transport_id_; }
    void set_transport_id(u32 id) { transport_id_ = id; }
    bool is_loaded() const { return transport_id_ != 0; }

    f32 speed_mult() const { return speed_mult_; }
    void set_speed_mult(f32 m) { speed_mult_ = m; }
    f32 effective_speed() const { return max_speed_ * speed_mult_; }

    // Movement multipliers
    f32 accel_mult() const { return accel_mult_; }
    void set_accel_mult(f32 m) { accel_mult_ = m; }
    f32 turn_mult() const { return turn_mult_; }
    void set_turn_mult(f32 m) { turn_mult_ = m; }
    f32 break_off_distance_mult() const { return break_off_distance_mult_; }
    void set_break_off_distance_mult(f32 m) { break_off_distance_mult_ = m; }
    f32 break_off_trigger_mult() const { return break_off_trigger_mult_; }
    void set_break_off_trigger_mult(f32 m) { break_off_trigger_mult_ = m; }
    void reset_speed_and_accel() { speed_mult_ = 1.0f; accel_mult_ = 1.0f; turn_mult_ = 1.0f; }

    // Fuel system
    f32 fuel_ratio() const { return fuel_ratio_; }
    void set_fuel_ratio(f32 r) { fuel_ratio_ = r; }
    f32 fuel_use_time() const { return fuel_use_time_; }
    void set_fuel_use_time(f32 t) { fuel_use_time_ = t; }

    // Air movement (populated from blueprint Air subtable)
    f32 heading() const { return heading_; }
    void set_heading(f32 h) { heading_ = h; }
    f32 pitch_angle() const { return pitch_; }
    void set_pitch_angle(f32 p) { pitch_ = p; }
    f32 bank_angle() const { return bank_angle_; }
    void set_bank_angle(f32 b) { bank_angle_ = b; }
    f32 current_airspeed() const { return current_airspeed_; }
    void set_current_airspeed(f32 s) { current_airspeed_ = s; }
    f32 current_altitude() const { return current_altitude_; }
    void set_current_altitude(f32 a) { current_altitude_ = a; }
    f32 max_airspeed() const { return max_airspeed_; }
    void set_max_airspeed(f32 s) { max_airspeed_ = s; }
    f32 turn_rate_rad() const { return turn_rate_rad_; }
    void set_turn_rate_rad(f32 r) { turn_rate_rad_ = r; }
    f32 accel_rate() const { return accel_rate_; }
    void set_accel_rate(f32 r) { accel_rate_ = r; }
    f32 climb_rate() const { return climb_rate_; }
    void set_climb_rate(f32 r) { climb_rate_ = r; }
    f32 elevation_target() const { return elevation_target_; }
    void set_elevation_target(f32 e) { elevation_target_ = e; }
    bool is_air_unit() const { return layer_ == "Air"; }

    // Air crash state (M159)
    bool is_crashing() const { return crashing_; }
    bool crash_impacted() const { return crash_impacted_; }
    f32 crash_velocity_y() const { return crash_velocity_y_; }
    f32 crash_spin_rate() const { return crash_spin_rate_; }
    f32 crash_damage() const { return crash_damage_; }
    void set_crash_damage(f32 d) { crash_damage_ = d; }

    /// Start air crash sequence (overrides normal death for air units).
    void begin_air_crash(f32 crash_dmg);

    // Misc flags
    u32 creator_id() const { return creator_id_; }
    void set_creator_id(u32 id) { creator_id_ = id; }
    bool auto_overcharge() const { return auto_overcharge_; }
    void set_auto_overcharge(bool b) { auto_overcharge_ = b; }
    bool overcharge_paused() const { return overcharge_paused_; }
    void set_overcharge_paused(bool b) { overcharge_paused_ = b; }
    u32 focus_entity_id() const { return focus_entity_id_; }
    void set_focus_entity_id(u32 id) { focus_entity_id_ = id; }

    // Death animation state
    bool is_dying() const { return dying_; }
    f32 death_timer() const { return death_timer_; }
    void begin_dying(f32 duration);
    void tick_dying(f32 dt);

    // Damage/kill flags
    bool can_take_damage() const { return can_take_damage_; }
    void set_can_take_damage(bool b) { can_take_damage_ = b; }
    bool can_be_killed() const { return can_be_killed_; }
    void set_can_be_killed(bool b) { can_be_killed_ = b; }
    u32 last_attacker_id() const { return last_attacker_id_; }
    void set_last_attacker_id(u32 id) { last_attacker_id_ = id; }

    // Command caps (RULEUCC_* command capabilities)
    void add_command_cap(const std::string& cap) { command_caps_.insert(cap); }
    void remove_command_cap(const std::string& cap) { command_caps_.erase(cap); }
    void restore_command_caps() { command_caps_ = original_command_caps_; }
    void snapshot_command_caps() { original_command_caps_ = command_caps_; }
    bool has_command_cap(const std::string& cap) const { return command_caps_.count(cap) > 0; }

    // Build restrictions
    void add_build_restriction(const std::string& id) { build_restrictions_.insert(id); }
    void remove_build_restriction(const std::string& id) { build_restrictions_.erase(id); }
    void restore_build_restrictions() { build_restrictions_.clear(); }
    bool is_build_restricted(const std::string& id) const { return build_restrictions_.count(id) > 0; }

    // Elevation override
    f32 elevation_override() const { return elevation_override_; }
    void set_elevation_override(f32 e) { elevation_override_ = e; }
    bool has_elevation_override() const { return elevation_override_ >= 0; }
    void clear_elevation_override() { elevation_override_ = -1.0f; }

    i32 transport_class() const { return transport_class_; }
    void set_transport_class(i32 c) { transport_class_ = c; }
    i32 transport_capacity() const { return transport_capacity_; }
    void set_transport_capacity(i32 c) { transport_capacity_ = c; }

    void attach_to_transport(Unit* transport, EntityRegistry& registry, lua_State* L);
    void detach_all_cargo(EntityRegistry& registry, lua_State* L);

    // Bone visibility (per-unit, ShowBone/HideBone)
    bool is_bone_hidden(i32 idx) const { return hidden_bones_.count(idx) > 0; }
    void show_bone(i32 idx) { hidden_bones_.erase(idx); }
    void hide_bone(i32 idx) { hidden_bones_.insert(idx); }

    // Animated bone matrices (for GPU skinning)
    const std::vector<std::array<f32, 16>>& animated_bone_matrices() const {
        return animated_bone_matrices_;
    }
    std::vector<std::array<f32, 16>>& animated_bone_matrices() {
        return animated_bone_matrices_;
    }
    u32 animated_bone_count() const {
        return static_cast<u32>(animated_bone_matrices_.size());
    }
    void init_animated_bones();

    // Manipulator system
    Manipulator* add_manipulator(std::unique_ptr<Manipulator> m);
    void remove_manipulator(Manipulator* m);
    void tick_manipulators(f32 dt, lua_State* L);
    void destroy_all_manipulators();

    // Intel system (per-type enabled/disabled + radius)
    bool is_intel_enabled(const std::string& type) const;
    f32 get_intel_radius(const std::string& type) const;
    void init_intel(const std::string& type, f32 radius);
    void enable_intel(const std::string& type);
    void disable_intel(const std::string& type);
    void set_intel_radius(const std::string& type, f32 radius);
    const std::unordered_map<std::string, IntelState>& intel_states() const { return intel_states_; }

    // Adjacency system
    const std::unordered_set<u32>& adjacent_unit_ids() const { return adjacent_unit_ids_; }
    void add_adjacent(u32 id) { adjacent_unit_ids_.insert(id); }
    void remove_adjacent(u32 id) { adjacent_unit_ids_.erase(id); }
    void clear_adjacents() { adjacent_unit_ids_.clear(); }

    f32 skirt_size_x() const { return skirt_size_x_; }
    f32 skirt_size_z() const { return skirt_size_z_; }
    f32 skirt_offset_x() const { return skirt_offset_x_; }
    f32 skirt_offset_z() const { return skirt_offset_z_; }
    void set_skirt(f32 sx, f32 sz, f32 ox, f32 oz) {
        skirt_size_x_ = sx; skirt_size_z_ = sz;
        skirt_offset_x_ = ox; skirt_offset_z_ = oz;
    }

    void fire_adjacency_callbacks(EntityRegistry& registry, lua_State* L);

    // OnGiven callback system
    void add_on_given_callback(int ref) { on_given_callbacks_.push_back(ref); }
    const std::vector<int>& on_given_callbacks() const { return on_given_callbacks_; }
    void clear_on_given_callbacks(lua_State* L);

private:
    void call_on_reclaimed(u32 target_id, EntityRegistry& registry, lua_State* L);
    bool nav_update(f64 dt, const map::Terrain* terrain);

    std::string unit_id_;
    std::string armor_type_ = "Default";
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
    bool being_captured_ = false;  // is this unit currently being captured?
    f32 footprint_size_x_ = 0;    // from blueprint Footprint.SizeX
    f32 footprint_size_z_ = 0;    // from blueprint Footprint.SizeZ
    bool paused_ = false;
    u32 shield_entity_id_ = 0;       // entity ID of shield (set by _c_CreateShield)
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
    f32 shield_ratio_ = 1.0f;    // shield health ratio (0-1)
    // Bone visibility
    std::unordered_set<i32> hidden_bones_;
    // Animated bone matrices (identity = no deformation)
    std::vector<std::array<f32, 16>> animated_bone_matrices_;
    // Intel system
    std::unordered_map<std::string, IntelState> intel_states_;
    // Manipulator system
    std::vector<std::unique_ptr<Manipulator>> manipulators_;
    // Transport system
    std::vector<u32> cargo_ids_;      // entity IDs of units loaded on this transport
    u32 transport_id_ = 0;           // entity ID of transport this unit is on (0 = not loaded)
    f32 speed_mult_ = 1.0f;          // speed multiplier (reduced when carrying cargo)
    i32 transport_class_ = 0;        // cargo TransportClass (1=small, 2=medium, 3=large)
    i32 transport_capacity_ = 0;     // transport Class1Capacity (max small slots)
    // Veterancy
    u8 vet_level_ = 0;
    // Stats/telemetry
    std::unordered_map<std::string, f64> stats_;
    // Silo ammo counters
    i32 nuke_silo_ammo_ = 0;
    i32 tactical_silo_ammo_ = 0;
    // Adjacency system
    std::unordered_set<u32> adjacent_unit_ids_;
    f32 skirt_size_x_ = 0;
    f32 skirt_size_z_ = 0;
    f32 skirt_offset_x_ = 0;
    f32 skirt_offset_z_ = 0;
    // Movement multipliers
    f32 accel_mult_ = 1.0f;
    f32 turn_mult_ = 1.0f;
    f32 break_off_distance_mult_ = 1.0f;
    f32 break_off_trigger_mult_ = 1.0f;
    // Fuel system
    f32 fuel_ratio_ = -1.0f;     // -1 = no fuel system (sentinel)
    f32 fuel_use_time_ = 0.0f;   // seconds of flight time
    // Air movement state
    f32 heading_ = 0;            // yaw in radians
    f32 pitch_ = 0;              // pitch in radians (visual only for dive/climb)
    f32 bank_angle_ = 0;         // roll in radians (visual banking on turns)
    f32 current_airspeed_ = 0;   // current speed (ramps toward max_airspeed_)
    f32 current_altitude_ = 0;   // actual Y offset above terrain
    f32 max_airspeed_ = 0;       // from blueprint Air.MaxAirspeed (fallback: max_speed_)
    f32 turn_rate_rad_ = 0;      // yaw rate rad/s, from Air.TurnSpeed (deg→rad)
    f32 accel_rate_ = 0;         // from Air.AccelerateRate (fallback: max_airspeed * 0.5)
    f32 climb_rate_ = 5.0f;      // vertical speed limit (units/sec)
    f32 elevation_target_ = 18.0f; // target altitude above terrain, from Physics.Elevation
    // Air crash state
    bool crashing_ = false;
    bool crash_impacted_ = false; // set once on terrain impact, consumed by SimState
    f32 crash_velocity_y_ = 0;
    f32 crash_spin_rate_ = 0;
    f32 crash_damage_ = 100.0f;  // from blueprint General.CrashDamage
    // Misc flags
    u32 creator_id_ = 0;
    bool auto_overcharge_ = false;
    bool overcharge_paused_ = false;
    u32 focus_entity_id_ = 0;
    // Damage/kill flags
    bool can_take_damage_ = true;
    bool can_be_killed_ = true;
    u32 last_attacker_id_ = 0;
    // Command caps
    std::unordered_set<std::string> command_caps_;
    std::unordered_set<std::string> original_command_caps_;
    // Build restrictions
    std::unordered_set<std::string> build_restrictions_;
    // Elevation override
    f32 elevation_override_ = -1.0f; // -1 = no override (sentinel)
    // Death animation
    bool dying_ = false;
    f32 death_timer_ = 0.0f;
    f32 death_duration_ = 0.0f;
    // OnGiven callbacks (Lua registry refs)
    std::vector<int> on_given_callbacks_;
    // Build queue (factory production queue)
    std::vector<BuildQueueEntry> build_queue_;
};

} // namespace osc::sim
