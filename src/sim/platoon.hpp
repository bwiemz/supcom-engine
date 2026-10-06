#pragma once

#include "core/types.hpp"
#include "sim/entity.hpp"

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace osc::sim {

class EntityRegistry;

class Platoon {
    friend struct StateIO; // snapshots (state_io.hpp)
public:
    u32 platoon_id() const { return platoon_id_; }
    void set_platoon_id(u32 id) { platoon_id_ = id; }

    i32 army_index() const { return army_index_; }
    void set_army_index(i32 a) { army_index_ = a; }

    /// MakePlatoon's name (Moho's mName): a label alone.
    const std::string& name() const { return name_; }
    void set_name(const std::string& n) { name_ = n; }
    /// UniquelyNamePlatoon's name (Moho's mUniqueName), which
    /// GetPlatoonUniquelyNamed finds: the army pool's is "ArmyPool". A
    /// platoon without one is destroyed once it holds no unit.
    const std::string& unique_name() const { return unique_name_; }
    void set_unique_name(const std::string& n) { unique_name_ = n; }
    bool is_army_pool() const;

    /// DisbandOnIdle: destroyed once its squads are idle, its units back
    /// in the pool (Moho's CleanUpPlatoons).
    bool disband_on_idle() const { return disband_on_idle_; }
    void set_disband_on_idle() { disband_on_idle_ = true; }

    int lua_table_ref() const { return lua_table_ref_; }
    void set_lua_table_ref(int ref) { lua_table_ref_ = ref; }

    bool destroyed() const { return destroyed_; }
    void mark_destroyed() { destroyed_ = true; }

    // Unit membership
    void add_unit(u32 entity_id);
    void remove_unit(u32 entity_id);
    bool has_unit(u32 entity_id) const;
    const std::vector<u32>& unit_ids() const { return unit_ids_; }

    // Compute centroid position of all living units
    Vector3 get_position(const EntityRegistry& registry) const;

    // Plan name
    const std::string& plan_name() const { return plan_name_; }
    void set_plan_name(const std::string& p) { plan_name_ = p; }

    // Squad tracking (simplified: just store squad name per unit)
    void set_unit_squad(u32 entity_id, const std::string& squad);
    const std::string& get_unit_squad(u32 entity_id) const;
    /// Moho's squad class of a squad name: 0 Unassigned (also no name), 1
    /// Attack, 2 Artillery, 3 Guard, 4 Support, 5 Scout, or -1 for another
    /// name. Moho reads the names in any case (retail asks for 'scout').
    static int squad_class(std::string_view squad);
    /// Whether `entity_id` is in squad `squad`: same class, in any case, or
    /// the same name when it is no class.
    bool in_squad(u32 entity_id, std::string_view squad) const;

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
    std::string unique_name_;
    int lua_table_ref_ = -2; // LUA_NOREF
    bool destroyed_ = false;
    bool disband_on_idle_ = false;
    std::string plan_name_;
    std::vector<u32> unit_ids_;
    std::unordered_map<u32, std::string> squad_map_;
    std::unordered_map<u32, std::string> formation_map_; // lookup only
    std::string formation_override_;
    int priority_targets_ref_ = -2; // LUA_NOREF
};

} // namespace osc::sim
