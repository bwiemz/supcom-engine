#include "app/match_telemetry.hpp"

#include "core/version.hpp"
#include "sim/army_brain.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <array>
#include <cmath>

namespace osc::app {

namespace {

/// Moho's army statistics a sample carries, by their own names (retail's
/// score screen reads the same ones).
constexpr std::array kStats = {
    "Economy_TotalProduced_Mass",  "Economy_TotalProduced_Energy",
    "Economy_TotalConsumed_Mass",  "Economy_TotalConsumed_Energy",
    "Economy_Income_Mass",         "Economy_Income_Energy",
    "Economy_Output_Mass",         "Economy_Output_Energy",
    "Economy_AccumExcess_Mass",    "Economy_AccumExcess_Energy",
    "Units_Active",                "Units_History",
    "Units_Killed",                "Units_MassValue_Built",
    "Units_MassValue_Lost",        "Units_EnergyValue_Built",
    "Units_EnergyValue_Lost",      "Enemies_Killed",
    "Enemies_MassValue_Destroyed", "Enemies_EnergyValue_Destroyed",
    "Enemies_Commanders_Destroyed",
};

const char* result_name(sim::BrainState state) {
    switch (state) {
    case sim::BrainState::InProgress: return "in_progress";
    case sim::BrainState::Victory: return "victory";
    case sim::BrainState::Defeat: return "defeat";
    case sim::BrainState::Draw: return "draw";
    case sim::BrainState::Recalled: return "recalled";
    }
    return "unknown";
}

} // namespace

MatchTelemetry::MatchTelemetry(const std::string& path)
    : out_(path, std::ios::out | std::ios::trunc) {
    if (!out_.is_open()) spdlog::warn("--match-telemetry: cannot write {}", path);
}

MatchTelemetry::~MatchTelemetry() { finish(nullptr, "closed"); }

void MatchTelemetry::write_match(const sim::SimState& sim) {
    nlohmann::json armies = nlohmann::json::array();
    for (size_t a = 0; a < sim.army_count(); ++a) {
        const auto* brain = sim.army_at(a);
        if (!brain) continue;
        armies.push_back({{"idx", a + 1},
                          {"name", brain->name()},
                          {"nickname", brain->nickname()},
                          {"faction", brain->faction()},
                          {"ai", brain->ai_personality()},
                          {"human", brain->is_human()},
                          {"civilian", brain->is_civilian()}});
    }
    out_ << nlohmann::json{{"record", "match"},
                           {"schema", 3},
                           {"engine", "opensupcom"},
                           {"version", osc::core::version()},
                           {"build", osc::core::build_id()},
                           {"seed", sim.seed()},
                           {"sample_every_ticks", kSampleEvery},
                           {"armies", std::move(armies)}}
                .dump()
         << '\n';
}

namespace {

/// What an army is made of and where its income comes from, at one tick
struct Makeup {
    i32 land = 0, air = 0, naval = 0, engineers = 0, structures = 0;
    f64 mass[5] = {};   // extractors, fabricators, commander, reclaim, other
    f64 energy[5] = {}; // generators, fabricators, commander, reclaim, other
};

std::vector<Makeup> makeups(const sim::SimState& sim) {
    std::vector<Makeup> out(sim.army_count());
    sim.entity_registry().for_each_unit([&](const sim::Entity& entity) {
        const auto& unit = static_cast<const sim::Unit&>(entity);
        const i32 army = unit.army();
        if (army < 0 || static_cast<size_t>(army) >= out.size() || unit.destroyed() ||
            unit.is_dying() || unit.has_category("INSIGNIFICANTUNIT")) {
            return;
        }
        Makeup& m = out[static_cast<size_t>(army)];
        if (!unit.is_being_built()) {
            if (!unit.is_mobile()) ++m.structures;
            else if (unit.has_category("ENGINEER") || unit.has_category("COMMAND")) ++m.engineers;
            else if (unit.can_fly() || unit.has_category("AIR")) ++m.air;
            else if (unit.has_category("NAVAL")) ++m.naval;
            else ++m.land;
        }
        const auto& econ = unit.economy();
        m.mass[3] += econ.reclaim_mass;
        m.energy[3] += econ.reclaim_energy;
        if (!econ.production_active || unit.is_being_built()) return;
        const bool commander = unit.has_category("COMMAND");
        const bool fabricator = unit.has_category("MASSFABRICATION");
        const int mass_from = commander                             ? 2
                              : unit.has_category("MASSEXTRACTION") ? 0
                              : fabricator                          ? 1
                                                                    : 4;
        const int energy_from = commander                               ? 2
                                : unit.has_category("ENERGYPRODUCTION") ? 0
                                : fabricator                            ? 1
                                                                        : 4;
        m.mass[mass_from] += econ.production_mass;
        m.energy[energy_from] += econ.production_energy;
    });
    return out;
}

nlohmann::json sources(const f64 (&from)[5], const char* first) {
    const auto r = [](f64 v) { return std::round(v * 100.0) / 100.0; };
    return {{first, r(from[0])},
            {"fabricators", r(from[1])},
            {"commander", r(from[2])},
            {"reclaim", r(from[3])},
            {"other", r(from[4])}};
}

} // namespace

void MatchTelemetry::write_samples(const sim::SimState& sim) {
    const auto made_of = makeups(sim);
    for (size_t a = 0; a < sim.army_count(); ++a) {
        const auto* brain = sim.army_at(a);
        if (!brain || brain->is_civilian()) continue;
        nlohmann::json sample{{"record", "sample"},
                              {"tick", sim.tick_count()},
                              {"army", a + 1},
                              {"result", result_name(brain->state())}};
        for (const char* stat : kStats) sample[stat] = brain->get_stat(stat);
        const Makeup& m = made_of[a];
        sample["units"] = {{"land", m.land},
                           {"air", m.air},
                           {"naval", m.naval},
                           {"engineers", m.engineers},
                           {"structures", m.structures}};
        sample["mass_from"] = sources(m.mass, "extractors");
        sample["energy_from"] = sources(m.energy, "generators");
        out_ << sample.dump() << '\n';
    }
    last_sample_tick_ = sim.tick_count();
    out_.flush();
}

void MatchTelemetry::write_events(const sim::SimState& sim) {
    const u32 tick = sim.tick_count();
    ++walk_;
    const auto event = [&](const char* kind, u32 id, const Seen& unit) {
        return nlohmann::json{
            {"record", "event"},     {"tick", tick},  {"kind", kind}, {"id", id},
            {"army", unit.army + 1}, {"bp", unit.bp}, {"x", unit.x},  {"z", unit.z}};
    };
    // Who last damaged a unit, as far as it is still there to ask
    const auto blame = [&](nlohmann::json& record, u32 attacker) {
        if (attacker == 0) return;
        record["by_id"] = attacker;
        if (const auto known = seen_.find(attacker); known != seen_.end()) {
            record["by_bp"] = known->second.bp;
            record["by_army"] = known->second.army + 1;
        } else if (const auto* by = sim.entity_registry().find(attacker)) {
            record["by_bp"] = by->blueprint_id();
            record["by_army"] = by->army() + 1;
        }
    };

    sim.entity_registry().for_each_unit([&](const sim::Entity& entity) {
        const auto& unit = static_cast<const sim::Unit&>(entity);
        if (unit.has_category("INSIGNIFICANTUNIT")) return;
        const u32 id = unit.entity_id();
        auto [it, first] = seen_.try_emplace(id);
        Seen& seen = it->second;
        seen.seen_at = walk_;
        seen.x = unit.position().x;
        seen.z = unit.position().z;
        if (unit.last_attacker_id() != 0) seen.last_attacker = unit.last_attacker_id();
        const bool gone = unit.destroyed() || unit.is_dying();
        if (first) {
            seen.army = unit.army();
            seen.bp = unit.blueprint_id();
            seen.commander = unit.has_category("COMMAND");
            seen.enhancements = unit.enhancements().size();
            if (gone) {
                seen.lost = true; // made and unmade within a tick: nothing to tell
                return;
            }
            seen.complete = !unit.is_being_built();
            out_ << event(seen.complete ? "unit_built" : "unit_started", id, seen).dump() << '\n';
        }
        if (seen.lost) return;
        if (gone) {
            seen.lost = true;
            auto record = event("unit_lost", id, seen);
            record["complete"] = seen.complete;
            blame(record, seen.last_attacker);
            out_ << record.dump() << '\n';
            return;
        }
        if (!seen.complete && !unit.is_being_built()) {
            seen.complete = true;
            out_ << event("unit_built", id, seen).dump() << '\n';
        }
        if (unit.enhancements().size() != seen.enhancements) {
            // Which is new is not kept: all it has now are told again
            if (unit.enhancements().size() > seen.enhancements) {
                for (const auto& [slot, name] : unit.enhancements()) {
                    auto record = event("enhanced", id, seen);
                    record["slot"] = slot;
                    record["name"] = name;
                    out_ << record.dump() << '\n';
                }
            }
            seen.enhancements = unit.enhancements().size();
        }
        if (seen.commander && unit.max_health() > 0) {
            const f32 health = unit.health() / unit.max_health();
            if (health <= seen.health_told - 0.1f) {
                auto record = event("commander_hit", id, seen);
                record["health"] = health;
                blame(record, seen.last_attacker);
                out_ << record.dump() << '\n';
                seen.health_told = health;
            } else if (health > seen.health_told) {
                seen.health_told = health; // repaired: the next tenth is from here
            }
        }
    });
    // Those the walk did not find are gone from the sim without a death seen
    for (auto it = seen_.begin(); it != seen_.end();) {
        Seen& seen = it->second;
        if (seen.seen_at == walk_) {
            ++it;
            continue;
        }
        if (!seen.lost) {
            auto record = event("unit_lost", it->first, seen);
            record["complete"] = seen.complete;
            blame(record, seen.last_attacker);
            out_ << record.dump() << '\n';
        }
        it = seen_.erase(it);
    }
    if (results_told_.size() != army_results_.size()) results_told_ = army_results_;
    for (size_t a = 0; a < army_results_.size(); ++a) {
        if (army_results_[a] == results_told_[a]) continue;
        results_told_[a] = army_results_[a];
        out_ << nlohmann::json{{"record", "event"},
                               {"tick", tick},
                               {"kind", "army_result"},
                               {"army", a + 1},
                               {"result", army_results_[a]}}
                    .dump()
             << '\n';
    }
}

void MatchTelemetry::after_tick(const sim::SimState& sim) {
    if (!out_.is_open() || finished_) return;
    const u32 tick = sim.tick_count();
    if (tick == 0 || tick == last_tick_) return;
    const auto now = Clock::now();
    if (!started_) {
        started_ = true;
        write_match(sim);
    } else {
        // A frame may run several ticks: the time since the last one seen,
        // shared among them.
        const f64 ms = std::chrono::duration<f64, std::milli>(now - last_tick_at_).count() /
                       static_cast<f64>(tick - last_tick_);
        tick_wall_ms_total_ += ms * static_cast<f64>(tick - last_tick_);
        tick_wall_count_ += tick - last_tick_;
        if (ms > tick_wall_ms_worst_) tick_wall_ms_worst_ = ms;
    }
    last_tick_at_ = now;
    last_tick_ = tick;
    army_results_.clear();
    for (size_t a = 0; a < sim.army_count(); ++a) {
        const auto* brain = sim.army_at(a);
        army_results_.emplace_back(brain ? result_name(brain->state()) : "unknown");
    }
    write_events(sim);
    if (tick - last_sample_tick_ >= kSampleEvery) write_samples(sim);
}

void MatchTelemetry::finish(const sim::SimState* sim, const std::string& ended) {
    if (!out_.is_open() || finished_) return;
    finished_ = true;
    if (sim && started_ && sim->tick_count() != last_sample_tick_) write_samples(*sim);
    const f64 wall =
        std::chrono::duration<f64>(Clock::now() - created_).count();
    out_ << nlohmann::json{{"record", "end"},
                           {"ended", sim && sim->game_ended() ? "victory" : ended},
                           {"last_tick", last_tick_},
                           {"game_seconds", static_cast<f64>(last_tick_) *
                                                sim::SimState::SECONDS_PER_TICK},
                           {"wall_seconds", wall},
                           {"tick_wall_ms_mean",
                            tick_wall_count_ ? tick_wall_ms_total_ / tick_wall_count_ : 0.0},
                           {"tick_wall_ms_worst", tick_wall_ms_worst_},
                           {"army_results", army_results_}}
                .dump()
         << '\n';
    out_.flush();
}

} // namespace osc::app
