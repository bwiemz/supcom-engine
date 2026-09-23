#include <catch2/catch_test_macros.hpp>

#include "renderer/overlay_renderer.hpp"

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
