#include <catch2/catch_test_macros.hpp>

#include "app/match_telemetry.hpp"
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
    CHECK(match[0]["schema"] == 2);

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

    CHECK(records(path, "end").size() == 1);
    std::filesystem::remove(path);
}
