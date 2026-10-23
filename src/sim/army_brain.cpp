#include "sim/army_brain.hpp"
#include "sim/entity_registry.hpp"
#include "sim/shield.hpp"
#include "sim/unit.hpp"

#include <algorithm>
#include <cctype>
#include <utility>
#include <vector>

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

// The share loop of Moho's ProcessArmyEconomy (sim/CEconomy.cpp)
std::pair<f64, f64> share_overflow(const std::vector<ArmyBrain*>& allies, f64 dt, f64 mass,
                                   f64 energy) {
    if (mass <= 0.0 && energy <= 0.0) {
        return {0.0, 0.0};
    }
    std::vector<ArmyBrain*> takers;
    for (ArmyBrain* ally : allies) {
        const EconomyState& e = ally->economy();
        if (e.mass.stored < e.mass.max_storage || e.energy.stored < e.energy.max_storage) {
            takers.push_back(ally);
        }
    }
    f64 shared_mass = 0.0;
    f64 shared_energy = 0.0;
    for (size_t i = 0; i < takers.size(); ++i) {
        const f64 fraction = 1.0 / static_cast<f64>(takers.size() - i);
        const EconomyState& e = takers[i]->economy();
        const f64 m = std::min(mass * fraction, std::max(0.0, e.mass.max_storage - e.mass.stored));
        const f64 en =
            std::min(energy * fraction, std::max(0.0, e.energy.max_storage - e.energy.stored));
        takers[i]->bank_pending(m / dt, en / dt);
        mass -= m;
        energy -= en;
        shared_mass += m;
        shared_energy += en;
    }
    return {shared_mass, shared_energy};
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

bool ArmyBrain::add_stat_trigger(StatTrigger t) {
    if (!stats_.contains(t.stat)) {
        return false;
    }
    stat_triggers_.push_back(std::move(t));
    return true;
}

void ArmyBrain::remove_stat_trigger(const std::string& stat, const std::string& name) {
    std::erase_if(stat_triggers_,
                  [&](const StatTrigger& t) { return t.stat == stat && t.name == name; });
}

std::vector<std::string> ArmyBrain::take_fired_triggers() {
    const auto holds = [&](const StatTrigger& t) {
        const f64 v = t.blueprints ? blueprint_stat(t.stat, *t.blueprints) : get_stat(t.stat);
        if (t.compare == "LessThan") {
            return v < t.value;
        }
        if (t.compare == "LessThanOrEqual") {
            return v <= t.value;
        }
        if (t.compare == "GreaterThan") {
            return v > t.value;
        }
        if (t.compare == "GreaterThanOrEqual") {
            return v >= t.value;
        }
        return t.compare == "Equal" && v == t.value;
    };
    std::vector<std::string> fired;
    for (const auto& t : stat_triggers_) {
        if (std::find(fired.begin(), fired.end(), t.name) != fired.end()) {
            continue;
        }
        const bool all =
            std::all_of(stat_triggers_.begin(), stat_triggers_.end(),
                        [&](const StatTrigger& o) { return o.name != t.name || holds(o); });
        if (all) {
            fired.push_back(t.name);
        }
    }
    std::erase_if(stat_triggers_, [&](const StatTrigger& t) {
        return std::find(fired.begin(), fired.end(), t.name) != fired.end();
    });
    return fired;
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
    if (resource_type != "MASS" && resource_type != "ENERGY") return 0.0;
    return get_economy_income(resource_type) - get_economy_usage(resource_type);
}

f32 ArmyBrain::get_unit_cost_total(const EntityRegistry& registry) const {
    f32 total = 0.0f;
    registry.for_each_unit([&](const Entity& e) {
        if (e.army() != index_ || e.destroyed() || !e.is_unit()) return;
        const auto& unit = static_cast<const Unit&>(e);
        if (!unit.is_dying()) total += unit.cap_cost();
    });
    return total;
}

bool ArmyBrain::is_build_restricted(std::string blueprint_id) const {
    std::transform(blueprint_id.begin(), blueprint_id.end(), blueprint_id.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return restricted_blueprints_.count(blueprint_id) > 0;
}

std::vector<Entity*> ArmyBrain::get_units(EntityRegistry& registry) const {
    std::vector<Entity*> result;
    registry.for_each_unit([&](Entity& e) {
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

void ArmyBrain::update_unit_cap_stats(const EntityRegistry& registry) {
    stats_["UnitCap_Current"] = static_cast<f64>(get_unit_cost_total(registry));
    stats_["UnitCap_MaxCap"] = static_cast<f64>(unit_cap_);
}

void ArmyBrain::update_economy(EntityRegistry& registry, f64 dt,
                               const std::vector<ArmyBrain*>& allies) {
    f64 mass_income = std::exchange(bank_mass_, 0.0);
    f64 energy_income = std::exchange(bank_energy_, 0.0);
    const f64 pending_mass = std::exchange(pending_mass_, 0.0);
    const f64 pending_energy = std::exchange(pending_energy_, 0.0);
    economy_.mass.reclaimed += std::exchange(reclaim_mass_, 0.0) * dt;
    economy_.energy.reclaimed += std::exchange(reclaim_energy_, 0.0) * dt;
    // The units' storage and GiveStorage's pair: an army has none of its
    // own (the original's starts with its commander's 4000 and 650)
    f64 held_mass = 0.0;
    f64 held_energy = 0.0;

    // Moho's two classes of request (ProcessArmyEconomy), by what each is
    // still short of, in floats: those that want both resources, and those
    // that want one
    EconValue dual;
    EconValue single;
    std::vector<EconRequest*> requests;
    const auto pool = [&](EconRequest& r) {
        const EconValue short_of = r.outstanding();
        if (short_of.mass == 0.0f && short_of.energy == 0.0f) {
            return;
        }
        EconValue& pooled = short_of.mass != 0.0f && short_of.energy != 0.0f ? dual : single;
        pooled.mass += short_of.mass;
        pooled.energy += short_of.energy;
        requests.push_back(&r);
    };
    registry.for_each_unit([&](Entity& e) {
        if (e.army() != index_ || e.destroyed() || !e.is_unit()) return;
        auto& unit = static_cast<Unit&>(e);
        auto& econ = unit.economy();
        econ.ask_consumption(dt);

        if (unit.holds_storage()) {
            held_mass += econ.storage_mass;
            held_energy += econ.storage_energy;
        }

        // Each its own request, as Moho's tasks each hold one: its
        // consumption, a silo's missile under way (M206), a repair on a
        // staging platform (M206r), a capture
        pool(econ.consumption_request);
        econ.silo_request.ask(econ.silo_mass * dt, econ.silo_energy * dt);
        econ.dock_repair_request.ask(econ.dock_repair_mass * dt, econ.dock_repair_energy * dt);
        econ.capture_request.ask(0.0, econ.capture_energy * dt);
        pool(econ.silo_request);
        pool(econ.dock_repair_request);
        pool(econ.capture_request);
    });

    // Its economy events' requests (M206d).
    for (EconRequest* r : std::exchange(requests_, {})) {
        pool(*r);
    }
    const EconValue demand{dual.mass + single.mass, dual.energy + single.energy};

    // Lobby handicap reduces this army's resource income.
    if (handicap_ > 0.0) {
        const f64 factor = 1.0 - handicap_;
        mass_income *= factor;
        energy_income *= factor;
    }

    economy_.mass.income = mass_income;
    economy_.energy.income = energy_income;
    economy_.mass.requested = demand.mass / dt;
    economy_.energy.requested = demand.energy / dt;
    count_storage(held_mass, held_energy);

    // ProcessArmyEconomy: what the army holds this tick against what is
    // asked. Both classes' demand bounds the pooled ratio, energy first; the
    // resource that bounds it last is the limiting one.
    const f64 mass_avail = mass_income * dt + economy_.mass.stored;
    const f64 energy_avail = energy_income * dt + economy_.energy.stored;
    const EconValue avail{static_cast<f32>(mass_avail), static_cast<f32>(energy_avail)};
    GrantRatios g;
    if (demand.energy * g.dual > avail.energy) {
        g.dual = avail.energy / demand.energy;
        g.mass_limits = false;
    }
    if (demand.mass * g.dual > avail.mass) {
        g.dual = avail.mass / demand.mass;
        g.mass_limits = true;
    }
    // Single-resource requests of the other resource share what the pooled
    // grants left of it
    const f32 single_needed = g.mass_limits ? single.energy : single.mass;
    const f32 left = std::max(0.0f, g.mass_limits ? avail.energy - dual.energy * g.dual
                                                  : avail.mass - dual.mass * g.dual);
    if (single_needed * g.single > left) {
        g.single = left / single_needed;
    }
    grant_ratios_ = g;
    EconValue consumed;
    for (EconRequest* r : requests) {
        const EconValue short_of = r->outstanding();
        const EconValue given = r->grant(g.of(short_of.mass, short_of.energy));
        consumed.mass += given.mass;
        consumed.energy += given.energy;
    }
    const f64 mass_raw = mass_avail - consumed.mass;
    const f64 energy_raw = energy_avail - consumed.energy;
    const f64 excess_mass = std::max(0.0, mass_raw - economy_.mass.max_storage);
    const f64 excess_energy = std::max(0.0, energy_raw - economy_.energy.max_storage);
    const auto [shared_mass, shared_energy] =
        resource_sharing_ ? share_overflow(allies, dt, excess_mass, excess_energy)
                          : std::pair<f64, f64>{0.0, 0.0};
    economy_.mass.stored = std::clamp(mass_raw, 0.0, economy_.mass.max_storage);
    economy_.energy.stored = std::clamp(energy_raw, 0.0, economy_.energy.max_storage);
    // What Moho reports used (mLastUseActual): each class at its own ratio
    const f32 mass_used = dual.mass * g.dual + single.mass * g.single;
    const f32 energy_used = dual.energy * g.dual + single.energy * g.single;
    mass_efficiency_ = demand.mass > 0.0f ? mass_used / demand.mass : 1.0;
    energy_efficiency_ = demand.energy > 0.0f ? energy_used / demand.energy : 1.0;

    // An upkeep stops when its own request isn't granted in full (which
    // Moho's report of usage can hide): an energy stall's, never a mass
    // stall's, which grants energy-only requests from what is left
    if (g.dual < 1.0 || g.single < 1.0) {
        registry.for_each_unit([&](Entity& e) {
            if (e.army() != index_ || e.destroyed() || !e.is_unit()) return;
            auto& unit = static_cast<Unit&>(e);
            auto& econ = unit.economy();
            if (!econ.maintenance_active ||
                econ.energy_maintenance_override <= 0.0) return;
            if (g.of(0.0, econ.energy_maintenance_override) >= 1.0) return;

            bool stopped = disable_maintenance_intel(unit);
            stopped = turn_off_maintained_shield(unit, registry) || stopped;
            if (stopped) econ.maintenance_active = false;
        });
    }

    // Moho's economy stats (the score reads them): totals produced and spent,
    // this tick's income and spending (Moho's are a tick's amounts, like its
    // other economy figures), and what full storage wasted.
    stats_["Economy_TotalProduced_Mass"] += economy_.mass.income * dt;
    stats_["Economy_TotalProduced_Energy"] += economy_.energy.income * dt;
    stats_["Economy_TotalConsumed_Mass"] += consumed.mass;
    stats_["Economy_TotalConsumed_Energy"] += consumed.energy;
    stats_["Economy_Income_Mass"] = mass_income * dt;
    stats_["Economy_Income_Energy"] = energy_income * dt;
    stats_["Economy_Output_Mass"] = consumed.mass;
    stats_["Economy_Output_Energy"] = consumed.energy;
    stats_["Economy_Shared_Mass"] += shared_mass;
    stats_["Economy_Shared_Energy"] += shared_energy;
    stats_["Economy_AccumExcess_Mass"] += excess_mass - shared_mass;
    stats_["Economy_AccumExcess_Energy"] += excess_energy - shared_energy;
    stats_["Economy_Ratio_Mass"] = get_economy_stored_ratio("MASS");
    stats_["Economy_Ratio_Energy"] = get_economy_stored_ratio("ENERGY");
    stats_["Economy_Stored_Mass"] = economy_.mass.stored;
    stats_["Economy_Stored_Energy"] = economy_.energy.stored;
    stats_["Economy_MaxStorage_Mass"] = economy_.mass.max_storage;
    stats_["Economy_MaxStorage_Energy"] = economy_.energy.max_storage;
    f64& peak_mass = stats_["Economy_PeakStorage_Mass"];
    peak_mass = std::max(peak_mass, economy_.mass.max_storage);
    f64& peak_energy = stats_["Economy_PeakStorage_Energy"];
    peak_energy = std::max(peak_energy, economy_.energy.max_storage);
    stats_["Economy_Reclaimed_Mass"] = economy_.mass.reclaimed;
    stats_["Economy_Reclaimed_Energy"] = economy_.energy.reclaimed;
    stats_["Economy_Trend_Mass"] = pending_mass - economy_.mass.requested;
    stats_["Economy_Trend_Energy"] = pending_energy - economy_.energy.requested;
}

void ArmyBrain::add_blueprint_stat(const std::string& stat, const std::string& bp_id, f64 delta) {
    stats_.try_emplace(stat, 0.0);
    blueprint_stats_[stat][bp_id] += delta;
}

void ArmyBrain::count_ready(const std::string& bp_id) {
    for (const char* stat : {"Units_Active", "Units_History"}) {
        stats_[stat] += 1.0;
        add_blueprint_stat(stat, bp_id, 1.0);
    }
}

f64 ArmyBrain::blueprint_stat(const std::string& stat,
                              const std::set<std::string>& blueprints) const {
    f64 total = 0.0;
    if (const auto* lanes = blueprint_stats(stat)) {
        for (const auto& [bp_id, value] : *lanes) {
            if (blueprints.contains(bp_id)) {
                total += value;
            }
        }
    }
    return total;
}

void ArmyBrain::record_unit_made(const Unit& unit) {
    if (unit.is_being_built()) {
        add_blueprint_stat("Units_BeingBuilt", unit.blueprint_id(), 1.0);
    } else if (unit.cap_cost() > 0.0f) {
        count_ready(unit.blueprint_id());
    }
}

void ArmyBrain::record_unit_built(const Unit& unit, f64 mass, f64 energy) {
    if (unit.cap_cost() <= 0.0f) {
        return;
    }
    count_ready(unit.blueprint_id());
    stats_["Units_MassValue_Built"] += mass;
    stats_["Units_EnergyValue_Built"] += energy;
    add_blueprint_stat("Units_BeingBuilt", unit.blueprint_id(), -1.0);
}

void ArmyBrain::record_unit_removed(const Unit& unit) {
    if (unit.is_being_built()) {
        add_blueprint_stat("Units_BeingBuilt", unit.blueprint_id(), -1.0);
    } else if (unit.cap_cost() > 0.0f) {
        stats_["Units_Active"] -= 1.0;
        add_blueprint_stat("Units_Active", unit.blueprint_id(), -1.0);
    }
}

void ArmyBrain::record_unit_lost(const std::string& bp_id, f64 mass, f64 energy) {
    stats_["Units_Killed"] += 1.0;
    blueprint_stats_["Units_Killed"][bp_id] += 1.0;
    stats_["Units_MassValue_Lost"] += mass;
    stats_["Units_EnergyValue_Lost"] += energy;
}

void ArmyBrain::record_enemy_killed(const std::string& bp_id, f64 mass, f64 energy,
                                    bool commander) {
    stats_["Enemies_Killed"] += 1.0;
    blueprint_stats_["Enemies_Killed"][bp_id] += 1.0;
    stats_["Enemies_MassValue_Destroyed"] += mass;
    stats_["Enemies_EnergyValue_Destroyed"] += energy;
    if (commander) stats_["Enemies_Commanders_Destroyed"] += 1.0;
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
    // Moho's GetPlatoonByName: the unique name, in any case.
    const auto same = [&](const std::string& other) {
        return other.size() == name.size() &&
               std::equal(other.begin(), other.end(), name.begin(), [](char a, char b) {
                   return std::tolower(static_cast<unsigned char>(a)) ==
                          std::tolower(static_cast<unsigned char>(b));
               });
    };
    for (auto& p : platoons_) {
        if (!p->destroyed() && same(p->unique_name())) return p.get();
    }
    return nullptr;
}

void ArmyBrain::destroy_platoon(Platoon* p) {
    if (p) p->mark_destroyed();
    // Don't erase from vector — pointer stability for lightuserdata
}

} // namespace osc::sim
