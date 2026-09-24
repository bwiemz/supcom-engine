#pragma once

#include "core/types.hpp"
#include "sim/entity.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace osc::sim {

class EntityRegistry;

class Platoon {
public:
    u32 platoon_id() const { return platoon_id_; }
    void set_platoon_id(u32 id) { platoon_id_ = id; }

    i32 army_index() const { return army_index_; }
    void set_army_index(i32 a) { army_index_ = a; }

    const std::string& name() const { return name_; }
    void set_name(const std::string& n) { name_ = n; }

    int lua_table_ref() const { return lua_table_ref_; }
    void set_lua_table_ref(int ref) { lua_table_ref_ = ref; }

    bool destroyed() const { return destroyed_; }
    void mark_destroyed() { destroyed_ = true; }

    // Unit membership
    void add_unit(u32 entity_id);
    void remove_unit(u32 entity_id);
    bool has_unit(u32 entity_id) const;
    const std::vector<u32>& unit_ids() const { return unit_ids_; }
    /// It has held a unit: once they are all gone, Moho destroys it.
    bool had_units() const { return had_units_; }

    // Compute centroid position of all living units
    Vector3 get_position(const EntityRegistry& registry) const;

    // Plan name
    const std::string& plan_name() const { return plan_name_; }
    void set_plan_name(const std::string& p) { plan_name_ = p; }

    // Squad tracking (simplified: just store squad name per unit)
    void set_unit_squad(u32 entity_id, const std::string& squad);
    const std::string& get_unit_squad(u32 entity_id) const;

    // The formation each unit was assigned with (AssignUnitsToPlatoon, a
    // template's fifth field); the override, if set, stands for all.
    void set_unit_formation(u32 entity_id, const std::string& formation);
    /// The formation `entity_id` moves in: the override, else its own ("" for
    /// none).
    std::string unit_formation(u32 entity_id) const;

    // Formation override
    const std::string& formation_override() const { return formation_override_; }
    void set_formation_override(const std::string& f) { formation_override_ = f; }

    // Priority targets (Lua table ref)
    int priority_targets_ref() const { return priority_targets_ref_; }
    void set_priority_targets_ref(int ref) { priority_targets_ref_ = ref; }

private:
    u32 platoon_id_ = 0;
    i32 army_index_ = -1;
    std::string name_;
    int lua_table_ref_ = -2; // LUA_NOREF
    bool destroyed_ = false;
    bool had_units_ = false;
    std::string plan_name_;
    std::vector<u32> unit_ids_;
    std::unordered_map<u32, std::string> squad_map_;
    std::unordered_map<u32, std::string> formation_map_; // lookup only
    std::string formation_override_;
    int priority_targets_ref_ = -2; // LUA_NOREF
};

} // namespace osc::sim
