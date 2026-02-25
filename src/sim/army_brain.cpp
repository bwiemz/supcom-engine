#include "sim/army_brain.hpp"
#include "sim/entity_registry.hpp"
#include "sim/unit.hpp"

#include <algorithm>

namespace osc::sim {

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

        // TODO: separate maintenance consumption (shields, radar) gated by
        // maintenance_active + energy_maintenance_override. Deferred until
        // shields/radar are implemented — currently no units have maintenance.

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

    mass_efficiency_ = (mass_consumption > 0)
        ? std::min(1.0, mass_income / mass_consumption)
        : 1.0;
    energy_efficiency_ = (energy_consumption > 0)
        ? std::min(1.0, energy_income / energy_consumption)
        : 1.0;

    economy_.mass.stored += (mass_income - mass_consumption) * dt;
    economy_.energy.stored += (energy_income - energy_consumption) * dt;

    economy_.mass.stored = std::clamp(economy_.mass.stored,
                                       0.0, economy_.mass.max_storage);
    economy_.energy.stored = std::clamp(economy_.energy.stored,
                                         0.0, economy_.energy.max_storage);
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
