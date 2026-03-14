#include "lua/smoke_test.hpp"
#include <spdlog/spdlog.h>
#include <algorithm>

namespace osc::lua {

void SmokeTestHarness::record(SmokeCategory category, const std::string& name,
                               const std::string& location) {
    EntryKey key{category, name};
    auto& data = entries_[key];
    if (data.count == 0) data.first_location = location;
    data.count++;
}

std::vector<SmokeReportEntry> SmokeTestHarness::generate_report() const {
    std::vector<SmokeReportEntry> result;
    result.reserve(entries_.size());
    for (auto& [key, data] : entries_) {
        result.push_back({key.category, key.name, data.first_location, data.count});
    }
    // Sort by count descending (highest-impact first)
    std::sort(result.begin(), result.end(),
              [](const auto& a, const auto& b) { return a.count > b.count; });
    return result;
}

u32 SmokeTestHarness::total_count() const {
    u32 total = 0;
    for (auto& [key, data] : entries_) total += data.count;
    return total;
}

void SmokeTestHarness::print_report() const {
    auto report = generate_report();
    spdlog::info("=== Smoke Test Report ({} unique issues, {} total occurrences) ===",
                 report.size(), total_count());
    const char* cat_names[] = {"MISSING_GLOBAL", "MISSING_METHOD", "PCALL_ERROR", "WRONG_RETURN"};
    for (auto& e : report) {
        spdlog::info("  [{:15s}] {:40s} x{:4d}  (first: {})",
                     cat_names[static_cast<int>(e.category)], e.name, e.count, e.first_location);
    }
}

} // namespace osc::lua
