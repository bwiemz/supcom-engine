// A zone's own time (self time) is its time less the zones within it: what
// a ranking of a run's costs (the Performance Lab's, --bench's profile) sums
// without counting a nested zone twice.

#include <catch2/catch_test_macros.hpp>

#include "core/profiler.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

namespace {

const osc::ProfileZoneStats* zone(const char* name) {
    const auto& p = osc::Profiler::instance();
    for (osc::u32 i = 0; i < p.zone_count(); ++i)
        if (std::strcmp(p.zone_stats()[i].name, name) == 0) return &p.zone_stats()[i];
    return nullptr;
}

void busy(std::chrono::milliseconds ms) {
    const auto until = std::chrono::steady_clock::now() + ms;
    while (std::chrono::steady_clock::now() < until) {}
}

} // namespace

TEST_CASE("A zone's self time leaves out the zones within it", "[profiler]") {
    auto& p = osc::Profiler::instance();
    const bool was = p.enabled();
    p.set_enabled(true);
    p.begin_frame();
    {
        PROFILE_ZONE("SelfTest::outer");
        busy(std::chrono::milliseconds(4));
        for (int i = 0; i < 2; ++i) {
            PROFILE_ZONE("SelfTest::inner");
            busy(std::chrono::milliseconds(6));
        }
    }
    p.end_frame();
    p.set_enabled(was);

    const auto* outer = zone("SelfTest::outer");
    const auto* inner = zone("SelfTest::inner");
    REQUIRE(outer != nullptr);
    REQUIRE(inner != nullptr);
    // The inner zone has nothing within it: its self time is its time
    CHECK(inner->total_self_us == inner->total_us);
    CHECK(inner->total_calls == 2);
    // The outer zone's self time is its time less the inner zone's, exactly
    CHECK(outer->total_self_us == outer->total_us - inner->total_us);
    // and about the 4 ms it spent itself (12 ms in the inner zone)
    CHECK(outer->total_self_us >= 3'500.0);
    CHECK(outer->total_self_us < 8'000.0);
    CHECK(inner->total_us >= 11'500.0);
}

TEST_CASE("Off-thread profiler zones do not disturb the owner's stack", "[profiler][thread]") {
    auto& p = osc::Profiler::instance();
    const bool was = p.enabled();
    p.set_enabled(true);
    std::atomic<bool> stop{false};
    std::thread other([&] {
        while (!stop.load()) {
            PROFILE_ZONE("ThreadTest::foreign");
        }
    });
    for (int frame = 0; frame < 100; ++frame) {
        p.begin_frame();
        { PROFILE_ZONE("ThreadTest::owner"); }
        p.end_frame();
    }
    stop = true;
    other.join();
    p.set_enabled(was);
    CHECK(zone("ThreadTest::foreign") == nullptr);
    REQUIRE(zone("ThreadTest::owner"));
    CHECK(zone("ThreadTest::owner")->total_calls == 100);
}
