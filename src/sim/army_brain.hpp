#pragma once

#include "core/types.hpp"
#include "sim/entity.hpp"
#include "sim/platoon.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace osc::sim {

class EntityRegistry;

enum class Alliance : i32 {
    Enemy = 0,
    Neutral = 1,
    Ally = 2,
};

enum class BrainState : i32 {
    InProgress = 0,
    Victory = 1,
    Defeat = 2,
    Draw = 3,
    Recalled = 4,
};

struct ResourceState {
    f64 income = 0.0;
    f64 requested = 0.0;
    f64 stored = 0.0;
    f64 max_storage = 200.0;
};

struct EconomyState {
    ResourceState mass;
    ResourceState energy;
};

/// The C++ backing object for moho.aibrain_methods.
/// Each army in the game has one ArmyBrain.
class ArmyBrain {
public:
    // --- Identity ---
    i32 index() const { return index_; }
    void set_index(i32 i) { index_ = i; }

    const std::string& name() const { return name_; }
    void set_name(const std::string& n) { name_ = n; }

    const std::string& nickname() const { return nickname_; }
    void set_nickname(const std::string& n) { nickname_ = n; }

    i32 faction() const { return faction_; }
    void set_faction(i32 f) { faction_ = f; }

    bool is_human() const { return is_human_; }
    void set_human(bool h) { is_human_ = h; }

    // --- Brain state ---
    BrainState state() const { return state_; }
    void set_state(BrainState s) { state_ = s; }
    bool is_defeated() const;

    // --- Lua table reference ---
    int lua_table_ref() const { return lua_table_ref_; }
    void set_lua_table_ref(int ref) { lua_table_ref_ = ref; }

    // --- Economy ---
    EconomyState& economy() { return economy_; }
    const EconomyState& economy() const { return economy_; }

    void set_stored_resources(f64 mass, f64 energy);
    f64 get_economy_income(const std::string& resource_type) const;
    f64 get_economy_requested(const std::string& resource_type) const;
    f64 get_economy_stored(const std::string& resource_type) const;
    f64 get_economy_stored_ratio(const std::string& resource_type) const;
    f64 get_economy_trend(const std::string& resource_type) const;

    /// Per-tick economy update: sum unit production/consumption, update stored.
    void update_economy(const EntityRegistry& registry, f64 dt);
    f64 mass_efficiency() const { return mass_efficiency_; }
    f64 energy_efficiency() const { return energy_efficiency_; }

    // --- Unit tracking ---
    i32 unit_cap() const { return unit_cap_; }
    void set_unit_cap(i32 cap) { unit_cap_ = cap; }

    i32 get_unit_cost_total(const EntityRegistry& registry) const;
    std::vector<Entity*> get_units(EntityRegistry& registry) const;

    // --- Alliance ---
    void set_alliance(i32 other_army, Alliance alliance);
    Alliance get_alliance(i32 other_army) const;
    bool is_ally(i32 other_army) const;
    bool is_enemy(i32 other_army) const;
    bool is_neutral(i32 other_army) const;

    // --- Start position ---
    const Vector3& start_position() const { return start_position_; }
    void set_start_position(const Vector3& pos) { start_position_ = pos; }

    // --- Build placement counter (for grid offset in FindPlaceToBuild) ---
    i32 next_build_place_index() { return build_place_counter_++; }

    // --- Current enemy ---
    i32 current_enemy_index() const { return current_enemy_index_; }
    void set_current_enemy_index(i32 idx) { current_enemy_index_ = idx; }

    // --- Platoons ---
    Platoon* create_platoon(const std::string& name);
    Platoon* find_platoon_by_name(const std::string& name);
    void destroy_platoon(Platoon* p);
    size_t platoon_count() const { return platoons_.size(); }
    Platoon* platoon_at(size_t i) const {
        return i < platoons_.size() ? platoons_[i].get() : nullptr;
    }

    // --- Color ---
    void set_color(u8 r, u8 g, u8 b) {
        color_r_ = r; color_g_ = g; color_b_ = b;
    }

private:
    i32 index_ = 0;
    std::string name_;
    std::string nickname_;
    i32 faction_ = 1;
    bool is_human_ = true;

    BrainState state_ = BrainState::InProgress;
    int lua_table_ref_ = -2; // LUA_NOREF

    EconomyState economy_;
    f64 mass_efficiency_ = 1.0;
    f64 energy_efficiency_ = 1.0;
    i32 unit_cap_ = 1000;

    std::unordered_map<i32, Alliance> alliances_;
    Vector3 start_position_;
    i32 build_place_counter_ = 0;
    i32 current_enemy_index_ = -1; // -1 = no current enemy
    u8 color_r_ = 255, color_g_ = 255, color_b_ = 255;

    std::vector<std::unique_ptr<Platoon>> platoons_;
    u32 next_platoon_id_ = 1;
};

} // namespace osc::sim
