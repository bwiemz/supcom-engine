#include <catch2/catch_test_macros.hpp>
#include "lua/smoke_test.hpp"
#include <fstream>
#include <filesystem>
#include <string>

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

TEST_CASE("SmokeTestHarness file output", "[smoke]") {
    osc::lua::SmokeTestHarness harness;
    harness.set_phase("GAME");
    harness.record(osc::lua::SmokeCategory::MissingMethod,
                   "Brain.Foo", "test.lua:1");

    auto path = std::filesystem::temp_directory_path() / "test_smoke_report.txt";
    harness.write_report_to_file(path.string());

    std::string content;
    {
        std::ifstream in(path);
        REQUIRE(in.good());
        content.assign((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
    }
    REQUIRE(content.find("MISSING_METHOD") != std::string::npos);
    REQUIRE(content.find("Brain.Foo") != std::string::npos);
    REQUIRE(content.find("GAME") != std::string::npos);

    std::filesystem::remove(path);
}

TEST_CASE("SmokeTestHarness error cap", "[smoke]") {
    osc::lua::SmokeTestHarness harness;
    harness.set_phase("GAME");
    for (int i = 0; i < 501; i++) {
        harness.record(osc::lua::SmokeCategory::MissingMethod,
                       "Method_" + std::to_string(i), "test.lua:1");
    }
    auto report = harness.generate_report();
    int game_count = 0;
    for (const auto& e : report) {
        if (e.phase == "GAME") game_count++;
    }
    REQUIRE(game_count == 500);
}

TEST_CASE("SmokeTestHarness active instance", "[smoke]") {
    REQUIRE(osc::lua::SmokeTestHarness::active_instance() == nullptr);
    {
        osc::lua::SmokeTestHarness harness;
        harness.activate();
        REQUIRE(osc::lua::SmokeTestHarness::active_instance() == &harness);
    }
    REQUIRE(osc::lua::SmokeTestHarness::active_instance() == nullptr);
}
