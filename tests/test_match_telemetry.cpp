#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "app/match_telemetry.hpp"
#include "lua/match_records.hpp"
#include "lua/lua_state.hpp"
#include "sim/manipulator.hpp"
#include "sim/shield.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {

/// The file's records of one kind (an event's `kind`, or a record's name).
std::vector<nlohmann::json> records(const std::filesystem::path& path, const std::string& kind) {
    std::vector<nlohmann::json> found;
    std::ifstream in(path);
    for (std::string line; std::getline(in, line);) {
        if (line.empty()) continue;
        auto record = nlohmann::json::parse(line);
        const std::string is = record["record"] == "event" ? record["kind"] : record["record"];
        if (is == kind) found.push_back(std::move(record));
    }
    return found;
}

osc::sim::Unit* add_unit(osc::sim::SimState& sim, osc::i32 army, const std::string& bp) {
    auto unit = std::make_unique<osc::sim::Unit>();
    unit->set_army(army);
    unit->set_blueprint_id(bp);
    unit->set_max_health(100.0f);
    unit->set_health(100.0f);
    auto& registry = sim.entity_registry();
    return static_cast<osc::sim::Unit*>(registry.find(registry.register_entity(std::move(unit))));
}

} // namespace

TEST_CASE("A match's record says what happened to its units, and when", "[telemetry]") {
    const auto path = std::filesystem::temp_directory_path() / "osc_match_events_test.jsonl";
    osc::lua::LuaState lua;
    osc::sim::SimState sim(lua.raw(), nullptr);
    sim.add_army("ARMY_1", "ARMY_1");
    sim.add_army("ARMY_2", "ARMY_2");
    auto* commander = add_unit(sim, 0, "uel0001");
    commander->add_category("COMMAND");
    commander->economy().production_mass = 1.0;
    commander->economy().production_energy = 20.0;
    commander->economy().production_active = true;
    auto* tank = add_unit(sim, 1, "url0107");
    tank->set_is_being_built(true);
    add_unit(sim, 1, "dummy")->add_category("INSIGNIFICANTUNIT");
    {
        osc::app::MatchTelemetry telemetry(path.string());
        REQUIRE(telemetry.is_open());
        const auto tick = [&] {
            sim.tick();
            telemetry.after_tick(sim);
        };

        tick(); // both first seen: one whole, one being built
        tank->set_is_being_built(false);
        tick(); // the tank is finished
        commander->set_health(95.0f);
        commander->set_last_attacker_id(tank->entity_id());
        tick(); // a scratch: not a tenth
        commander->set_health(80.0f);
        tick(); // a fifth down
        commander->add_enhancement("Back", "Shield");
        tick();
        tank->set_last_attacker_id(commander->entity_id());
        tank->begin_dying();
        tick(); // lost, once, however long it takes to die
        tick();
        telemetry.finish(&sim, "ticks");
    }

    const auto match = records(path, "match");
    REQUIRE(match.size() == 1);
    CHECK(match[0]["schema"] == 4);
    CHECK(match[0]["armies"][0]["ai"].get<std::string>().empty());


    const auto built = records(path, "unit_built");
    REQUIRE(built.size() == 2);
    CHECK(built[0]["bp"] == "uel0001");
    CHECK(built[0]["army"] == 1);
    CHECK(built[0]["tick"] == 1);
    CHECK(built[1]["bp"] == "url0107");
    CHECK(built[1]["tick"] == 2);

    const auto started = records(path, "unit_started");
    REQUIRE(started.size() == 1); // the insignificant unit is not told of
    CHECK(started[0]["bp"] == "url0107");
    CHECK(started[0]["army"] == 2);

    const auto hit = records(path, "commander_hit");
    REQUIRE(hit.size() == 1);
    CHECK(hit[0]["tick"] == 4);
    CHECK(hit[0]["health"].get<double>() < 0.81);
    CHECK(hit[0]["by_bp"] == "url0107");
    CHECK(hit[0]["by_army"] == 2);

    const auto enhanced = records(path, "enhanced");
    REQUIRE(enhanced.size() == 1);
    CHECK(enhanced[0]["slot"] == "Back");
    CHECK(enhanced[0]["name"] == "Shield");

    const auto lost = records(path, "unit_lost");
    REQUIRE(lost.size() == 1);
    CHECK(lost[0]["bp"] == "url0107");
    CHECK(lost[0]["complete"] == true);
    CHECK(lost[0]["by_bp"] == "uel0001");
    CHECK(lost[0]["by_army"] == 1);
    CHECK(lost[0]["tick"] == 6);

    // The last sample says what each army is made of and where its income
    // comes from: the commander, counted with the engineers, makes it all
    const auto samples = records(path, "sample");
    REQUIRE_FALSE(samples.empty());
    const auto& last = samples[samples.size() - 2]; // army 1's (army 2's is after it)
    REQUIRE(last["army"] == 1);
    CHECK(last["units"]["engineers"] == 1);
    CHECK(last["units"]["land"] == 0);
    CHECK(last["mass_from"]["commander"].get<double>() == 1.0);
    CHECK(last["energy_from"]["commander"].get<double>() == 20.0);
    CHECK(last["mass_from"]["extractors"].get<double>() == 0.0);

    CHECK(records(path, "end").size() == 1);
    std::filesystem::remove(path);
}

TEST_CASE("A kept match record is read back for the game's match history", "[telemetry]") {
    const auto path = std::filesystem::temp_directory_path() / "osc_match_history_test.jsonl";
    {
        std::ofstream out(path);
        out << R"({"record":"match","schema":3,"seed":7,"build":"b","armies":[)"
            << R"({"idx":1,"name":"ARMY_1","nickname":"Ryan","faction":1,"human":true,"ai":""},)"
            << R"({"idx":2,"name":"ARMY_2","faction":3,"human":false,"ai":"rush"}]})" << '\n';
        for (int t = 50; t <= 1000; t += 50) {
            for (int army = 1; army <= 2; ++army) {
                out << R"({"record":"sample","army":)" << army << R"(,"tick":)" << t
                    << R"(,"Economy_Income_Mass":1.5,"Units_Active":4,"units":{"land":3},)"
                    << R"("mass_from":{"extractors":14,"commander":1}})" << '\n';
            }
        }
        out << R"({"record":"event","kind":"unit_built","tick":100,"army":1,"bp":"ueb1103"})"
            << '\n';
        out << R"({"record":"event","kind":"unit_built","tick":120,"army":1,"bp":"uel0106"})"
            << '\n';
        out << R"({"record":"event","kind":"unit_lost","tick":900,"army":2,"bp":"url0106",)"
            << R"("complete":true,"by_army":1,"by_bp":"uel0106"})" << '\n';
        out << R"({"record":"end","ended":"victory","army_results":["victory","defeat"]})" << '\n';
    }
    const auto full = osc::lua::read_match_record(path, false);
    REQUIRE(full);
    const auto& armies = (*full)["armies"];
    REQUIRE(armies.size() == 2);
    CHECK(armies[0]["name"] == "Ryan");
    CHECK(armies[0]["result"] == "victory");
    CHECK(armies[0]["built"] == 2);
    CHECK(armies[0]["destroyed"] == 1);
    CHECK(armies[1]["ai"] == "rush");
    CHECK(armies[1]["lost"] == 1);
    CHECK(armies[0]["structures"].size() == 1);
    CHECK(armies[0]["structures"][0][1] == "ueb1103");
    CHECK(armies[0]["units"]["uel0106"] == 1);
    REQUIRE(armies[0]["samples"].size() == 20);
    CHECK(armies[0]["samples"][19][0].get<double>() == 1000.0); // the tick
    CHECK(armies[0]["samples"][19][1].get<double>() == 15.0);   // mass a second
    CHECK(armies[0]["samples"][19][4].get<double>() == 3.0);    // land units
    CHECK(armies[0]["samples"][19][9].get<double>() == 14.0);   // mass from extractors
    CHECK((*full)["notable"].size() == 1);
    CHECK((*full)["ticks"].get<double>() == 1000.0);

    const auto summary = osc::lua::read_match_record(path, true);
    REQUIRE(summary);
    CHECK_FALSE((*summary)["armies"][0].contains("samples"));
    CHECK((*summary)["armies"][0]["last"][1].get<double>() == 15.0);
    CHECK_FALSE(summary->contains("notable"));

    CHECK_FALSE(osc::lua::read_match_record(path.string() + ".none", false));
    std::filesystem::remove(path);
}

TEST_CASE("Match income sources retain banked output after rate changes", "[telemetry][banked]") {
    const auto path = std::filesystem::temp_directory_path() / "osc_match_banked_test.jsonl";
    osc::lua::LuaState lua;
    osc::sim::SimState sim(lua.raw(), nullptr);
    sim.add_army("ARMY_1", "ARMY_1");
    auto* producer = add_unit(sim, 0, "ueb1103");
    producer->add_category("MASSEXTRACTION");
    auto& econ = producer->economy();
    econ.production_active = true;
    econ.production_mass = 2;
    econ.production_energy = 4;
    sim.tick();
    REQUIRE(econ.produced_mass == 2);
    REQUIRE(econ.produced_energy == 4);
    econ.production_mass = 99;
    econ.production_energy = 199;
    econ.reclaim_mass = 20; // configured work has not banked anything
    {
        osc::app::MatchTelemetry telemetry(path.string());
        telemetry.after_tick(sim);
        telemetry.finish(&sim, "ticks");
    }
    const auto samples = records(path, "sample");
    REQUIRE_FALSE(samples.empty());
    CHECK(samples.back()["mass_from"]["extractors"].get<double>() == 2);
    CHECK(samples.back()["energy_from"]["other"].get<double>() == 4);
    CHECK(samples.back()["mass_from"]["reclaim"].get<double>() == 0);
    std::filesystem::remove(path);
}

TEST_CASE("How a game ran, and the moments its player marked, are kept and read back",
          "[telemetry]") {
    const auto path = std::filesystem::temp_directory_path() / "osc_match_perf_test.jsonl";
    osc::lua::LuaState lua;
    osc::sim::SimState sim(lua.raw(), nullptr);
    sim.add_army("ARMY_1", "ARMY_1");
    {
        osc::app::MatchTelemetry telemetry(path.string());
        REQUIRE(telemetry.is_open());
        sim.tick();
        telemetry.after_tick(sim);
        osc::app::PerfSample p;
        p.wall_seconds = 10.0;
        p.tick = 41;
        p.ticks = 40;
        p.speed = 3.98;
        p.frames = 600;
        p.frame_ms_mean = 16.7;
        p.frame_ms_p95 = 18.2;
        p.frame_ms_worst = 41.0;
        p.frames_over_33 = 2;
        p.held = 3;
        p.tick_ms_mean = 22.5;
        p.units = 120;
        telemetry.write_perf(p);
        telemetry.write_mark(41, "lag");
        telemetry.finish(&sim, "ticks");
    }
    const auto perf = records(path, "perf");
    REQUIRE(perf.size() == 1);
    CHECK(perf[0]["frame_ms_p95"] == 18.2);
    CHECK(perf[0]["units"] == 120);
    const auto marks = records(path, "player_mark");
    REQUIRE(marks.size() == 1);
    CHECK(marks[0]["note"] == "lag");

    // The screens': the speed got (4 game seconds in 10), and the marks;
    // a mark is no unit's event
    const auto full = osc::lua::read_match_record(path, false);
    REQUIRE(full);
    REQUIRE((*full)["perf"].size() == 1);
    CHECK((*full)["perf"][0]["achieved"].get<double>() == Catch::Approx(0.4));
    CHECK((*full)["perf"][0]["held"].get<double>() == 3.0);
    REQUIRE((*full)["marks"].size() == 1);
    CHECK((*full)["marks"][0]["tick"].get<double>() == 41.0);
    CHECK((*full)["marks"][0]["note"] == "lag");

    std::filesystem::remove(path);
}
