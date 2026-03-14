#include <catch2/catch_test_macros.hpp>
#include "lua/smoke_test.hpp"
#include "lua/lua_state.hpp"

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

TEST_CASE("SmokeTestHarness intercepts missing globals", "[smoke]") {
    osc::lua::LuaState state;
    osc::lua::SmokeTestHarness harness;
    harness.install_global_interceptor(state.raw());

    // Access a global that doesn't exist - should be logged, return nil
    auto result = state.do_string("local x = SomeMissingGlobal\nreturn type(x)");
    REQUIRE(result.ok());

    // Check that the missing access was recorded
    auto report = harness.generate_report();
    bool found = false;
    for (auto& e : report) {
        if (e.name == "SomeMissingGlobal" &&
            e.category == osc::lua::SmokeCategory::MissingGlobal) {
            found = true;
        }
    }
    REQUIRE(found);
}

TEST_CASE("Global interceptor does not log existing globals", "[smoke]") {
    osc::lua::LuaState state;
    osc::lua::SmokeTestHarness harness;

    // Set a global first, then install interceptor
    state.do_string("MyGlobal = 42");
    harness.install_global_interceptor(state.raw());

    state.do_string("local x = MyGlobal");
    REQUIRE(harness.total_count() == 0);
}
