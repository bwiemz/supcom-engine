#include <catch2/catch_test_macros.hpp>

#include "renderer/overlay_renderer.hpp"

#include <array>
#include <string>
#include <unordered_set>
#include <vector>

using osc::renderer::intel_ring_types_for_filters;
using Types = std::unordered_set<std::string>;

TEST_CASE("Overlay filters: no active filter shows no intel ring", "[overlay]") {
    CHECK(intel_ring_types_for_filters({}).empty());
}

TEST_CASE("Overlay filters: each intel filter enables its own ring", "[overlay]") {
    CHECK(intel_ring_types_for_filters({"Radar"}) == Types{"Radar"});
    CHECK(intel_ring_types_for_filters({"Sonar", "Omni"}) == Types{"Sonar", "Omni"});
}

TEST_CASE("Overlay filters: AllIntel enables radar, sonar and omni", "[overlay]") {
    CHECK(intel_ring_types_for_filters({"AllIntel"}) == Types{"Radar", "Sonar", "Omni"});
}

TEST_CASE("Overlay filters: military and counter-intel filters draw no intel ring",
          "[overlay]") {
    // Names are RangeOverlayParams keys, matched exactly: CounterIntel is a
    // different overlay, and the lowercase pref keys are never passed.
    CHECK(intel_ring_types_for_filters({"AllMilitary", "AntiAir", "DirectFire",
                                        "CounterIntel", "radar"})
              .empty());
}

TEST_CASE("An overlay line is drawn along its segment, not as the box it spans",
          "[renderer][overlay]") {
    using osc::renderer::line_runs;
    const auto area = [](const std::vector<std::array<float, 4>>& runs) {
        float a = 0;
        for (const auto& r : runs) {
            a += r[2] * r[3];
        }
        return a;
    };
    const auto covers = [](const std::vector<std::array<float, 4>>& runs, float x, float y) {
        for (const auto& r : runs) {
            if (x >= r[0] && x <= r[0] + r[2] && y >= r[1] && y <= r[1] + r[3]) {
                return true;
            }
        }
        return false;
    };

    // Level: one rect, the line's width tall
    const auto level = line_runs(10, 20, 110, 20, 1.5f);
    REQUIRE(level.size() == 1);
    CHECK(level[0] == std::array<float, 4>{10, 18.5f, 100, 3});

    // Slanted, 400 across and 300 down (a command line across the screen):
    // every point along it drawn, and not much more than its length by its
    // width -- not the 400x300 box it spans
    const auto slanted = line_runs(100, 100, 500, 400, 1.5f);
    for (int i = 0; i <= 100; ++i) {
        const float t = static_cast<float>(i) / 100.0f;
        CHECK(covers(slanted, 100 + 400 * t, 100 + 300 * t));
    }
    CHECK_FALSE(covers(slanted, 480, 120)); // the box's far corner
    CHECK(area(slanted) < 500.0f * 3.0f * 2.0f);

    // However long, no more runs than asked for, and still along the line
    const auto capped = line_runs(0, 0, 3000, 2000, 1.0f, 64);
    CHECK(capped.size() == 64);
    CHECK(covers(capped, 1500, 1000));
    CHECK_FALSE(covers(capped, 2900, 100));
}
