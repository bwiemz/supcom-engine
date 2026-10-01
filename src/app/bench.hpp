#pragma once

// --bench <report.json> (M223): how long a headless run's sim ticks took,
// and which game it was, for tools/bench.py to compare with a baseline.
// Measuring reads clocks only: the game is the one it would be without it.

#include <chrono>
#include <string>
#include <vector>

namespace osc::sim {
class SimState;
}

namespace osc::app {

/// A run of tick times' figures, in milliseconds.
struct TickStats {
    size_t ticks = 0;
    double total_ms = 0;
    double mean_ms = 0;
    double p50_ms = 0;
    double p90_ms = 0;
    double p95_ms = 0;
    double p99_ms = 0;
    double max_ms = 0;
    size_t max_tick = 0; ///< which tick (1-based) was the slowest
};

/// The figures of `tick_ms` (one per tick, in order); percentiles by the
/// nearest-rank method.
TickStats tick_stats(const std::vector<double>& tick_ms);

class BenchRecorder {
public:
    using Clock = std::chrono::steady_clock;

    /// Loading counts from here to the first tick.
    explicit BenchRecorder(std::string report_path);

    /// One tick's time.
    void record(Clock::duration tick);

    /// Write the report: the ticks' figures, overall and for each 1,000
    /// ticks, and the ten slowest ticks; the load time; the game's checksum
    /// and entity and unit counts at the end; peak memory; and the build.
    /// False if it can't be written.
    bool write(const sim::SimState& sim) const;

    [[nodiscard]] const std::vector<double>& tick_ms() const { return tick_ms_; }

private:
    std::string path_;
    Clock::time_point created_;
    Clock::time_point first_tick_{};
    std::vector<double> tick_ms_;
};

} // namespace osc::app
