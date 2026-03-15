#include <catch2/catch_test_macros.hpp>
#include "lua/smoke_test.hpp"

TEST_CASE("SmokeTestHarness phase tracking", "[smoke]") {
    osc::lua::SmokeTestHarness harness;

    SECTION("entries record current phase") {
        harness.set_phase("GAME");
        harness.record(osc::lua::SmokeCategory::MissingMethod,
                       "Brain.PBMAddBuildLocation", "ai.lua:10");
        harness.record(osc::lua::SmokeCategory::MissingMethod,
                       "Brain.PBMAddBuildLocation", "ai.lua:20");

        harness.set_phase("LOBBY");
        harness.record(osc::lua::SmokeCategory::MissingMethod,
                       "Brain.PBMAddBuildLocation", "lobby.lua:5");

        auto report = harness.generate_report();
        REQUIRE(report.size() == 2);

        bool found_game = false, found_lobby = false;
        for (const auto& e : report) {
            if (e.phase == "GAME") {
                REQUIRE(e.count == 2);
                REQUIRE(e.first_location == "ai.lua:10");
                found_game = true;
            }
            if (e.phase == "LOBBY") {
                REQUIRE(e.count == 1);
                found_lobby = true;
            }
        }
        REQUIRE(found_game);
        REQUIRE(found_lobby);
    }

    SECTION("default phase is empty string") {
        harness.record(osc::lua::SmokeCategory::MissingGlobal,
                       "SomeGlobal", "test.lua:1");
        auto report = harness.generate_report();
        REQUIRE(report.size() == 1);
        REQUIRE(report[0].phase.empty());
    }
}
