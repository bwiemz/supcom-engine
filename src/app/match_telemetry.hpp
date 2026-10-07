#pragma once

// --match-telemetry <file>: a match's statistics, as JSON Lines, for a
// database of matches to be compared across builds and content. One `match`
// record at the first tick (the build, the seed, the armies), a `sample` per
// army every kSampleEvery ticks (Moho's army statistics: the economy's
// totals, units built and lost, enemies destroyed), and an `end` record as
// the run ends. Recording reads the sim only: the game is the one it would be
// without it.

#include "core/types.hpp"

#include <chrono>
#include <fstream>
#include <string>
#include <vector>

namespace osc::sim {
class SimState;
}

namespace osc::app {

class MatchTelemetry {
public:
    /// A sample every 30 game seconds.
    static constexpr u32 kSampleEvery = 300;

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
};

} // namespace osc::app
