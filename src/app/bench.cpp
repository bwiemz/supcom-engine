#include "app/bench.hpp"

#include "core/version.hpp"
#include "platform/system_info.hpp"
#include "sim/army_brain.hpp"
#include "sim/entity.hpp"
#include "sim/sim_state.hpp"

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <numeric>

namespace osc::app {

namespace {

/// The nearest-rank `p` percentile of `sorted` (ascending, not empty).
double percentile(const std::vector<double>& sorted, double p) {
    const auto rank =
        static_cast<size_t>(std::ceil(p / 100.0 * static_cast<double>(sorted.size())));
    return sorted[std::clamp<size_t>(rank, 1, sorted.size()) - 1];
}

nlohmann::json to_json(const TickStats& s) {
    return {{"ticks", s.ticks},   {"total_ms", s.total_ms}, {"mean_ms", s.mean_ms},
            {"p50_ms", s.p50_ms}, {"p90_ms", s.p90_ms},     {"p95_ms", s.p95_ms},
            {"p99_ms", s.p99_ms}, {"max_ms", s.max_ms},     {"max_tick", s.max_tick}};
}

constexpr size_t kWindow = 1000; ///< ticks per window of the report
constexpr size_t kSlowest = 10;  ///< the slowest ticks the report lists

} // namespace

TickStats tick_stats(const std::vector<double>& tick_ms) {
    TickStats s;
    s.ticks = tick_ms.size();
    if (tick_ms.empty()) return s;
    s.total_ms = std::accumulate(tick_ms.begin(), tick_ms.end(), 0.0);
    s.mean_ms = s.total_ms / static_cast<double>(s.ticks);
    const auto slowest = std::max_element(tick_ms.begin(), tick_ms.end());
    s.max_ms = *slowest;
    s.max_tick = static_cast<size_t>(slowest - tick_ms.begin()) + 1;
    std::vector<double> sorted = tick_ms;
    std::sort(sorted.begin(), sorted.end());
    s.p50_ms = percentile(sorted, 50);
    s.p90_ms = percentile(sorted, 90);
    s.p95_ms = percentile(sorted, 95);
    s.p99_ms = percentile(sorted, 99);
    return s;
}

BenchRecorder::BenchRecorder(std::string report_path)
    : path_(std::move(report_path)), created_(Clock::now()) {}

void BenchRecorder::record(Clock::duration tick) {
    if (tick_ms_.empty()) first_tick_ = Clock::now() - tick;
    tick_ms_.push_back(std::chrono::duration<double, std::milli>(tick).count());
}

bool BenchRecorder::write(const sim::SimState& sim) const {
    nlohmann::json windows = nlohmann::json::array();
    for (size_t from = 0; from < tick_ms_.size(); from += kWindow) {
        const size_t to = std::min(from + kWindow, tick_ms_.size());
        const std::vector<double> part(tick_ms_.begin() + static_cast<std::ptrdiff_t>(from),
                                       tick_ms_.begin() + static_cast<std::ptrdiff_t>(to));
        nlohmann::json w = to_json(tick_stats(part));
        w["from_tick"] = from + 1;
        w["to_tick"] = to;
        windows.push_back(std::move(w));
    }
    // The slowest ticks and when: a periodic cost (a collection every n
    // ticks) shows in their tick numbers
    std::vector<size_t> order(tick_ms_.size());
    std::iota(order.begin(), order.end(), size_t{0});
    const size_t slow_n = std::min<size_t>(kSlowest, order.size());
    std::partial_sort(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(slow_n),
                      order.end(), [&](size_t a, size_t b) { return tick_ms_[a] > tick_ms_[b]; });
    nlohmann::json slowest = nlohmann::json::array();
    for (size_t i = 0; i < slow_n; ++i)
        slowest.push_back({{"tick", order[i] + 1}, {"ms", tick_ms_[order[i]]}});

    size_t units = 0;
    sim.entity_registry().for_each_unit([&](const sim::Entity& e) {
        if (!e.destroyed()) ++units;
    });
    // The path searches' work (Moho pathing's army queues; none without it)
    u64 searches = 0;
    u64 expansions = 0;
    for (size_t i = 0; i < sim.army_count(); ++i)
        if (const sim::ArmyBrain* army = sim.army_at(i)) {
            searches += army->path_queue().searches_done();
            expansions += army->path_queue().expansions_done();
        }
    const double load_s =
        tick_ms_.empty() ? 0.0 : std::chrono::duration<double>(first_tick_ - created_).count();

#ifdef NDEBUG
    constexpr const char* kBuildType = "release";
#else
    constexpr const char* kBuildType = "debug";
#endif
    const nlohmann::json report = {
        {"version", core::version()},
        {"build", core::build_id()},
        {"build_type", kBuildType},
        {"os", platform::os_description()},
        {"load_s", load_s},
        {"ticks", to_json(tick_stats(tick_ms_))},
        {"windows", windows},
        {"slowest", slowest},
        {"game",
         {{"tick", sim.tick_count()},
          {"checksum", fmt::format("{:08x}", sim.compute_sync_checksum())},
          {"entities", sim.entity_registry().count()},
          {"units", units},
          {"moho_pathing", sim.moho_pathing()},
          {"path_searches", searches},
          {"path_expansions", expansions}}},
        {"peak_memory_mb", static_cast<double>(platform::peak_memory_bytes()) / (1024.0 * 1024.0)},
    };
    std::ofstream out(path_, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << report.dump(2) << '\n';
    return static_cast<bool>(out.flush());
}

} // namespace osc::app
