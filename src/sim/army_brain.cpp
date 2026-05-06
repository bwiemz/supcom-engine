#include "sim/army_brain.hpp"
#include "sim/entity_registry.hpp"
#include "sim/shield.hpp"
#include "sim/unit.hpp"

#include <algorithm>
#include <sstream>

namespace osc::sim {

namespace {

constexpr const char* MAINTENANCE_INTEL_TYPES[] = {
    "Cloak",
    "CloakField",
    "Radar",
    "Sonar",
    "Omni",
    "Jammer",
    "RadarStealth",
    "SonarStealth",
    "RadarStealthField",
    "SonarStealthField",
    "StealthField",
    "WaterVision",
};

bool disable_maintenance_intel(Unit& unit) {
    bool disabled = false;
    for (const char* type : MAINTENANCE_INTEL_TYPES) {
        if (!unit.is_intel_enabled(type)) continue;
        unit.disable_intel(type);
        disabled = true;
    }
    return disabled;
}

bool turn_off_maintained_shield(Unit& unit, const EntityRegistry& registry) {
    if (unit.shield_entity_id() == 0) return false;

    auto* entity = registry.find(unit.shield_entity_id());
    if (!entity || entity->destroyed() || !entity->is_shield()) return false;

    auto* shield = static_cast<Shield*>(entity);
    if (!shield->is_on) return false;
    shield->is_on = false;
    return true;
}

} // namespace

bool ArmyBrain::is_defeated() const {
    return state_ == BrainState::Defeat || state_ == BrainState::Recalled;
}

void ArmyBrain::set_stored_resources(f64 mass, f64 energy) {
    economy_.mass.stored = mass;
    economy_.energy.stored = energy;
}

f64 ArmyBrain::get_economy_income(const std::string& resource_type) const {
    if (resource_type == "MASS") return economy_.mass.income;
    if (resource_type == "ENERGY") return economy_.energy.income;
    return 0.0;
}

f64 ArmyBrain::get_economy_requested(const std::string& resource_type) const {
    if (resource_type == "MASS") return economy_.mass.requested;
    if (resource_type == "ENERGY") return economy_.energy.requested;
    return 0.0;
}

f64 ArmyBrain::get_economy_stored(const std::string& resource_type) const {
    if (resource_type == "MASS") return economy_.mass.stored;
    if (resource_type == "ENERGY") return economy_.energy.stored;
    return 0.0;
}

f64 ArmyBrain::get_economy_stored_ratio(const std::string& resource_type) const {
    if (resource_type == "MASS") {
        return economy_.mass.max_storage > 0
            ? economy_.mass.stored / economy_.mass.max_storage : 0.0;
    }
    if (resource_type == "ENERGY") {
        return economy_.energy.max_storage > 0
            ? economy_.energy.stored / economy_.energy.max_storage : 0.0;
    }
    return 0.0;
}

f64 ArmyBrain::get_economy_usage(const std::string& resource_type) const {
    if (resource_type == "MASS") return economy_.mass.requested * mass_efficiency_;
    if (resource_type == "ENERGY") return economy_.energy.requested * energy_efficiency_;
    return 0.0;
}

f64 ArmyBrain::get_economy_trend(const std::string& resource_type) const {
    if (resource_type == "MASS")
        return economy_.mass.income - economy_.mass.requested;
    if (resource_type == "ENERGY")
        return economy_.energy.income - economy_.energy.requested;
    return 0.0;
}

i32 ArmyBrain::get_unit_cost_total(const EntityRegistry& registry) const {
    i32 count = 0;
    registry.for_each([&](const Entity& e) {
        if (e.army() == index_ && !e.destroyed() && e.is_unit())
            count++;
    });
    return count;
}

bool ArmyBrain::is_build_restricted(
    const std::unordered_set<std::string>& blueprint_categories) const {
    for (const auto& restriction : build_restrictions_) {
        if (restriction.empty()) continue;
        if (blueprint_categories.count(restriction) > 0) return true;

        std::istringstream tokens(restriction);
        std::string token;
        bool has_token = false;
        bool all_tokens_match = true;
        while (tokens >> token) {
            has_token = true;
            if (blueprint_categories.count(token) == 0) {
                all_tokens_match = false;
                break;
            }
        }
        if (has_token && all_tokens_match) return true;
    }
    return false;
}

std::vector<Entity*> ArmyBrain::get_units(EntityRegistry& registry) const {
    std::vector<Entity*> result;
    registry.for_each([&](Entity& e) {
        if (e.army() == index_ && !e.destroyed() && e.is_unit())
            result.push_back(&e);
    });
    return result;
}

void ArmyBrain::set_alliance(i32 other_army, Alliance alliance) {
    alliances_[other_army] = alliance;
}

Alliance ArmyBrain::get_alliance(i32 other_army) const {
    auto it = alliances_.find(other_army);
    if (it != alliances_.end()) return it->second;
    if (other_army == index_) return Alliance::Ally;
    return Alliance::Enemy;
}

bool ArmyBrain::is_ally(i32 other_army) const {
    return get_alliance(other_army) == Alliance::Ally;
}

bool ArmyBrain::is_enemy(i32 other_army) const {
    return get_alliance(other_army) == Alliance::Enemy;
}

bool ArmyBrain::is_neutral(i32 other_army) const {
    return get_alliance(other_army) == Alliance::Neutral;
}

void ArmyBrain::update_economy(const EntityRegistry& registry, f64 dt) {
    f64 mass_income = 0.0;
    f64 energy_income = 0.0;
    f64 mass_consumption = 0.0;
    f64 energy_consumption = 0.0;
    f64 total_storage_mass = 200.0;    // base storage
    f64 total_storage_energy = 200.0;

    registry.for_each([&](const Entity& e) {
        if (e.army() != index_ || e.destroyed() || !e.is_unit()) return;
        const auto& unit = static_cast<const Unit&>(e);
        const auto& econ = unit.economy();

        if (econ.production_active) {
            mass_income += econ.production_mass;
            energy_income += econ.production_energy;
        }

        if (econ.consumption_active) {
            mass_consumption += econ.consumption_mass;
            energy_consumption += econ.consumption_energy;
        }

        if (econ.maintenance_active && econ.energy_maintenance_override >= 0.0) {
            energy_consumption += econ.energy_maintenance_override;
        }

        // Storage contribution always counted
        total_storage_mass += econ.storage_mass;
        total_storage_energy += econ.storage_energy;
    });

    economy_.mass.income = mass_income;
    economy_.energy.income = energy_income;
    economy_.mass.requested = mass_consumption;
    economy_.energy.requested = energy_consumption;
    economy_.mass.max_storage = total_storage_mass;
    economy_.energy.max_storage = total_storage_energy;

    // Storage-aware efficiency: storage acts as buffer that smoothly drains
    // before stalling kicks in. available = income*dt + stored
    {
        f64 mass_avail = mass_income * dt + economy_.mass.stored;
        f64 mass_needed = mass_consumption * dt;
        f64 mass_consumed = (mass_needed > 0) ? std::min(mass_avail, mass_needed) : 0.0;
        economy_.mass.stored = std::clamp(mass_avail - mass_consumed,
                                           0.0, economy_.mass.max_storage);
        mass_efficiency_ = (mass_needed > 0) ? mass_consumed / mass_needed : 1.0;
    }
    {
        f64 energy_avail = energy_income * dt + economy_.energy.stored;
        f64 energy_needed = energy_consumption * dt;
        f64 energy_consumed = (energy_needed > 0) ? std::min(energy_avail, energy_needed) : 0.0;
        economy_.energy.stored = std::clamp(energy_avail - energy_consumed,
                                             0.0, economy_.energy.max_storage);
        energy_efficiency_ = (energy_needed > 0) ? energy_consumed / energy_needed : 1.0;
    }

    if (energy_efficiency_ < 1.0) {
        registry.for_each([&](Entity& e) {
            if (e.army() != index_ || e.destroyed() || !e.is_unit()) return;
            auto& unit = static_cast<Unit&>(e);
            auto& econ = unit.economy();
            if (!econ.maintenance_active ||
                econ.energy_maintenance_override <= 0.0) return;

            bool stopped = disable_maintenance_intel(unit);
            stopped = turn_off_maintained_shield(unit, registry) || stopped;
            if (stopped) econ.maintenance_active = false;
        });
    }

    // Accumulate total resources collected for score tracking
    if (economy_.mass.income > 0) {
        stats_["Economy_TotalProduced_Mass"] += economy_.mass.income * dt;
    }
    if (economy_.energy.income > 0) {
        stats_["Economy_TotalProduced_Energy"] += economy_.energy.income * dt;
    }
}

Platoon* ArmyBrain::create_platoon(const std::string& name) {
    auto p = std::make_unique<Platoon>();
    p->set_platoon_id(next_platoon_id_++);
    p->set_army_index(index_);
    p->set_name(name);
    auto* raw = p.get();
    platoons_.push_back(std::move(p));
    return raw;
}

Platoon* ArmyBrain::find_platoon_by_name(const std::string& name) {
    for (auto& p : platoons_) {
        if (!p->destroyed() && p->name() == name)
            return p.get();
    }
    return nullptr;
}

void ArmyBrain::destroy_platoon(Platoon* p) {
    if (p) p->mark_destroyed();
    // Don't erase from vector — pointer stability for lightuserdata
}

} // namespace osc::sim
