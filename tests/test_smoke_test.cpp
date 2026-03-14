#include <catch2/catch_test_macros.hpp>
#include "lua/smoke_test.hpp"

TEST_CASE("SmokeTestHarness records and deduplicates entries", "[smoke]") {
    osc::lua::SmokeTestHarness harness;

    harness.record(osc::lua::SmokeCategory::MissingGlobal, "FooFunc", "test.lua:10");
    harness.record(osc::lua::SmokeCategory::MissingGlobal, "FooFunc", "test.lua:20");
    harness.record(osc::lua::SmokeCategory::MissingMethod, "unit.BarMethod", "unit.lua:5");
    harness.record(osc::lua::SmokeCategory::PcallError, "attempt to index nil", "sim.lua:99");

    auto report = harness.generate_report();

    // 3 unique entries (FooFunc deduplicated)
    REQUIRE(report.size() == 3);

    // Find the FooFunc entry — count should be 2
    bool found_foo = false;
    for (auto& e : report) {
        if (e.name == "FooFunc") {
            REQUIRE(e.category == osc::lua::SmokeCategory::MissingGlobal);
            REQUIRE(e.count == 2);
            REQUIRE(e.first_location == "test.lua:10");
            found_foo = true;
        }
    }
    REQUIRE(found_foo);
}

TEST_CASE("SmokeTestHarness total_count sums all occurrences", "[smoke]") {
    osc::lua::SmokeTestHarness harness;
    harness.record(osc::lua::SmokeCategory::MissingGlobal, "A", "a.lua:1");
    harness.record(osc::lua::SmokeCategory::MissingGlobal, "A", "a.lua:2");
    harness.record(osc::lua::SmokeCategory::MissingMethod, "B", "b.lua:1");
    REQUIRE(harness.total_count() == 3);
}
