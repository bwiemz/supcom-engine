#pragma once

// --match-telemetry <file>: a match's statistics, as JSON Lines, for a
// database of matches to be compared across builds and content. One `match`
// record at the first tick (the build, the seed, the armies), a `sample` per
// army every kSampleEvery ticks (Moho's army statistics: the economy's
// totals, units built and lost, enemies destroyed), and an `end` record as
// the run ends. Recording reads the sim only: the game is the one it would be
// without it.
//
// Between the samples, what happened, an `event` record each (schema 2;
// WOPC, 2026-10-13: a snapshot of totals every thirty seconds cannot say
// what happened at minute 8). Each has the tick and a `kind`:
//
//   unit_started   a unit first seen, still being built: id, army, bp, x, z
//   unit_built     a unit complete (or first seen so: a commander): the same
//   unit_lost      a unit dying or gone: the same, `complete` (was it built),
//                  and what last damaged it when anything did: by_id, by_bp,
//                  by_army (an army's own unit when it was let go)
//   enhanced       an upgrade fitted to a unit: slot, name
//   commander_hit  a commander's health a tenth lower than last told:
//                  health (0 to 1), by_bp, by_army
//   army_result    an army's result changed: army, result
//
// They are found by looking at the units each tick, not by the sim telling:
// an event carries the tick it was seen at.

#include "core/types.hpp"

#include <chrono>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace osc::sim {
class SimState;
}

namespace osc::app {

class MatchTelemetry {
public:
    /// A sample every 5 game seconds (30 until schema 2).
    static constexpr u32 kSampleEvery = 50;

    /// Opens (truncates) `path`; is_open() says whether it could.
    explicit MatchTelemetry(const std::string& path);
    /// Writes the `end` record if finish() didn't.
    ~MatchTelemetry();
    MatchTelemetry(const MatchTelemetry&) = delete;
    MatchTelemetry& operator=(const MatchTelemetry&) = delete;

    [[nodiscard]] bool is_open() const { return out_.is_open(); }

    /// After a sim tick: the `match` record the first time, a tick's time,
    /// and the armies' samples when one is due. A tick it has seen is
    /// ignored, so every frame may call it.
    void after_tick(const sim::SimState& sim);

    /// The run's end: a last sample of every army and the `end` record, with
    /// `ended` ("victory", "ticks", "closed"...). Later calls do nothing.
    void finish(const sim::SimState* sim, const std::string& ended);

private:
    void write_match(const sim::SimState& sim);
    void write_samples(const sim::SimState& sim);
    void write_events(const sim::SimState& sim);

    /// A unit as the events last saw it.
    struct Seen {
        i32 army = -1;
        std::string bp;
        f32 x = 0;
        f32 z = 0;
        bool complete = false;
        bool lost = false; ///< told as lost (it may be dying a while)
        bool commander = false;
        size_t enhancements = 0;
        f32 health_told = 1.0f; ///< a commander's health, as last told
        u32 last_attacker = 0;
        u32 seen_at = 0; ///< the walk that last found it
    };

    using Clock = std::chrono::steady_clock;

    std::ofstream out_;
    Clock::time_point created_ = Clock::now();
    Clock::time_point last_tick_at_{};
    bool started_ = false;
    bool finished_ = false;
    u32 last_tick_ = 0;
    u32 last_sample_tick_ = 0;
    /// Wall time from each tick to the next, in ms: the sim's tick and, in a
    /// drawn game, its frames.
    f64 tick_wall_ms_total_ = 0;
    f64 tick_wall_ms_worst_ = 0;
    u32 tick_wall_count_ = 0;
    /// Each army's result as of the last tick seen, for an `end` written
    /// without the sim (the destructor's).
    std::vector<std::string> army_results_;
    /// The units the events know, by id, and the count of the walks made.
    std::unordered_map<u32, Seen> seen_;
    u32 walk_ = 0;
    /// Each army's result as the events last told it.
    std::vector<std::string> results_told_;
};

} // namespace osc::app
