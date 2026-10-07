#include "app/match_telemetry.hpp"

#include "core/version.hpp"
#include "sim/army_brain.hpp"
#include "sim/sim_state.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <array>

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
                          {"human", brain->is_human()},
                          {"civilian", brain->is_civilian()}});
    }
    out_ << nlohmann::json{{"record", "match"},
                           {"schema", 1},
                           {"engine", "opensupcom"},
                           {"version", osc::core::version()},
                           {"build", osc::core::build_id()},
                           {"seed", sim.seed()},
                           {"sample_every_ticks", kSampleEvery},
                           {"armies", std::move(armies)}}
                .dump()
         << '\n';
}

void MatchTelemetry::write_samples(const sim::SimState& sim) {
    for (size_t a = 0; a < sim.army_count(); ++a) {
        const auto* brain = sim.army_at(a);
        if (!brain || brain->is_civilian()) continue;
        nlohmann::json sample{{"record", "sample"},
                              {"tick", sim.tick_count()},
                              {"army", a + 1},
                              {"result", result_name(brain->state())}};
        for (const char* stat : kStats) sample[stat] = brain->get_stat(stat);
        out_ << sample.dump() << '\n';
    }
    last_sample_tick_ = sim.tick_count();
    out_.flush();
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
