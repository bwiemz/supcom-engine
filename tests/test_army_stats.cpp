#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "app/support.hpp"
#include "core/front_end_data.hpp"
#include "core/game_state.hpp"
#include "sim/army_brain.hpp"
#include "sim/entity_registry.hpp"
#include "sim/manipulator.hpp"
#include "sim/shield.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "lua/session_manager.hpp"
#include "support/tick_until.hpp"
#include "ui/ui_control.hpp"
#include "vfs/virtual_file_system.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using osc::test::tick_until;

namespace {

void economy_tick(osc::sim::ArmyBrain& brain, osc::sim::EntityRegistry& registry, double dt) {
    registry.for_each_unit([&](osc::sim::Entity& e) {
        auto& unit = static_cast<osc::sim::Unit&>(e);
        unit.bank_reclaim(&brain);
        unit.manage_resources(&brain);
    });
    brain.update_economy(registry, dt);
    brain.update_unit_cap_stats(registry);
}

} // namespace

TEST_CASE("ArmyBrain stat storage", "[army][stats]") {
    osc::sim::ArmyBrain brain;
    SECTION("default stat returns default value") {
        REQUIRE(brain.get_stat("Units_History", 0.0) == 0.0);
        REQUIRE(brain.get_stat("Mass_Collected", 42.0) == 42.0);
    }
    SECTION("set and get stat") {
        brain.set_stat("Units_History", 5.0);
        REQUIRE(brain.get_stat("Units_History") == 5.0);
    }
    SECTION("add_stat accumulates") {
        brain.add_stat("Units_Killed", 1.0);
        brain.add_stat("Units_Killed", 1.0);
        brain.add_stat("Units_Killed", 1.0);
        REQUIRE(brain.get_stat("Units_Killed") == 3.0);
    }
}

TEST_CASE("Army stats use Moho's names and meanings", "[army][stats]") {
    // Retail's score (aibrain.lua) reads these: Units_Killed is the army's
    // *losses*, Enemies_Killed its kills, Units_History what it built.
    osc::sim::ArmyBrain brain;
    osc::sim::Unit scout;
    scout.set_blueprint_id("uel0101");
    scout.set_is_being_built(true);
    brain.record_unit_made(scout);
    brain.record_unit_made(scout);
    scout.set_is_being_built(false);
    brain.record_unit_built(scout, 52.0, 260.0);
    brain.record_unit_built(scout, 52.0, 260.0);
    brain.record_unit_lost("uel0101", 52.0, 260.0);
    brain.record_enemy_killed("url0001", 18000.0, 5000000.0, /*commander=*/true);
    brain.record_enemy_killed("url0106", 30.0, 150.0, false);

    CHECK(brain.get_stat("Units_History") == 2.0);
    CHECK(brain.get_stat("Units_MassValue_Built") == 104.0);
    CHECK(brain.get_stat("Units_Killed") == 1.0);
    CHECK(brain.get_stat("Units_MassValue_Lost") == 52.0);
    CHECK(brain.get_stat("Units_EnergyValue_Lost") == 260.0);
    CHECK(brain.get_stat("Enemies_Killed") == 2.0);
    CHECK(brain.get_stat("Enemies_MassValue_Destroyed") == 18030.0);
    CHECK(brain.get_stat("Enemies_Commanders_Destroyed") == 1.0);

    // Counts are kept per blueprint too (GetBlueprintStat by category).
    const auto* killed = brain.blueprint_stats("Enemies_Killed");
    REQUIRE(killed);
    CHECK(killed->at("url0001") == 1.0);
    CHECK(killed->at("url0106") == 1.0);
    CHECK(brain.blueprint_stats("Units_History")->at("uel0101") == 2.0);
    CHECK(brain.blueprint_stats("Nope") == nullptr);
}

TEST_CASE("Army economy stats: totals, rates and waste", "[army][stats][economy]") {
    osc::sim::EntityRegistry registry;
    osc::sim::ArmyBrain brain;
    brain.set_index(0);
    brain.set_unit_cap(500);

    auto producer = std::make_unique<osc::sim::Unit>();
    producer->set_army(0);
    producer->economy().production_mass = 10.0; // per second
    producer->economy().production_active = true;
    registry.register_entity(std::move(producer));
    auto spender = std::make_unique<osc::sim::Unit>();
    spender->set_army(0);
    spender->economy().consumption_mass = 4.0;
    spender->economy().consumption_active = true;
    registry.register_entity(std::move(spender));

    for (int i = 0; i < 10; ++i) {
        economy_tick(brain, registry, 0.1); // 1 s
    }
    // The rates are a tick's worth, as Moho's: 10/s and 4/s, 0.1 s a tick.
    CHECK(std::abs(brain.get_stat("Economy_Income_Mass") - 1.0) < 1e-9);
    CHECK(std::abs(brain.get_stat("Economy_TotalProduced_Mass") - 10.0) < 1e-9);
    CHECK(brain.get_stat("Economy_TotalConsumed_Mass") == Catch::Approx(4.0));
    CHECK(brain.get_stat("Economy_Output_Mass") == Catch::Approx(0.4));
    CHECK(brain.get_stat("UnitCap_Current") == 2.0);
    CHECK(brain.get_stat("UnitCap_MaxCap") == 500.0);

    // Full storage wastes the surplus: 6/s over the 200 base storage.
    tick_until([&] { economy_tick(brain, registry, 0.1); }, "the full storage wastes the surplus",
               800, [&] { return brain.get_stat("Economy_AccumExcess_Mass") > 0.0; });
}

TEST_CASE("The economy sets the stored, storage and peak storage stats each tick",
          "[army][stats][economy]") {
    const bool mass = GENERATE(true, false);
    const auto [first, second] =
        GENERATE(std::pair{500.0, 100.0}, std::pair{100.0, 800.0}, std::pair{300.0, 300.0});
    const std::string lane = mass ? "Mass" : "Energy";
    osc::sim::EntityRegistry registry;
    osc::sim::ArmyBrain brain;
    brain.set_index(0);
    auto store = std::make_unique<osc::sim::Unit>();
    store->set_army(0);
    store->economy().production_active = true;
    (mass ? store->economy().production_mass : store->economy().production_energy) = 30.0;
    double& storage = mass ? store->economy().storage_mass : store->economy().storage_energy;
    storage = first;
    registry.register_entity(std::move(store));

    for (int i = 0; i < 10; ++i) {
        economy_tick(brain, registry, 0.1);
    }
    CHECK(brain.get_stat("Economy_Stored_" + lane) == Catch::Approx(30.0));
    CHECK(brain.get_stat("Economy_MaxStorage_" + lane) == first);
    CHECK(brain.get_stat("Economy_PeakStorage_" + lane) == first);

    storage = second;
    tick_until([&] { economy_tick(brain, registry, 0.1); }, "the store fills to its new storage",
               800, [&] { return brain.get_stat("Economy_Stored_" + lane) == second; });
    CHECK(brain.get_stat("Economy_MaxStorage_" + lane) == second);
    CHECK(brain.get_stat("Economy_PeakStorage_" + lane) == std::max(first, second));
}

TEST_CASE("Economy_Reclaimed totals what reclaim brought in, and nothing else",
          "[army][stats][economy]") {
    const bool mass = GENERATE(true, false);
    const double rate = GENERATE(0.0, 3.0, 25.0);
    const std::string lane = mass ? "Mass" : "Energy";
    osc::sim::EntityRegistry registry;
    osc::sim::ArmyBrain brain;
    brain.set_index(0);
    auto engineer = std::make_unique<osc::sim::Unit>();
    engineer->set_army(0);
    engineer->economy().production_active = true;
    engineer->economy().production_mass = 1.0;
    engineer->economy().production_energy = 20.0;
    (mass ? engineer->economy().reclaim_mass : engineer->economy().reclaim_energy) = rate;
    auto* e = engineer.get();
    registry.register_entity(std::move(engineer));

    for (int i = 0; i < 10; ++i) {
        brain.bank(5.0, 50.0);
        economy_tick(brain, registry, 0.1);
    }
    CHECK(brain.get_stat("Economy_Reclaimed_" + lane) == Catch::Approx(rate).margin(1e-9));
    CHECK(brain.get_stat(mass ? "Economy_Reclaimed_Energy" : "Economy_Reclaimed_Mass") == 0.0);

    e->stop_reclaiming();
    for (int i = 0; i < 10; ++i) {
        economy_tick(brain, registry, 0.1);
    }
    CHECK(brain.get_stat("Economy_Reclaimed_" + lane) == Catch::Approx(rate).margin(1e-9));
}

TEST_CASE("Economy_Trend is what the units produce less what the army asks, a second",
          "[army][stats][economy]") {
    const bool mass = GENERATE(true, false);
    const auto [produced, asked] =
        GENERATE(std::pair{10.0, 4.0}, std::pair{2.0, 9.0}, std::pair{0.0, 0.0});
    const std::string lane = mass ? "Mass" : "Energy";
    osc::sim::EntityRegistry registry;
    osc::sim::ArmyBrain brain;
    brain.set_index(0);
    auto unit = std::make_unique<osc::sim::Unit>();
    unit->set_army(0);
    unit->economy().production_active = true;
    unit->economy().consumption_active = true;
    (mass ? unit->economy().production_mass : unit->economy().production_energy) = produced;
    (mass ? unit->economy().consumption_mass : unit->economy().consumption_energy) = asked;
    (mass ? unit->economy().reclaim_mass : unit->economy().reclaim_energy) = 7.0;
    registry.register_entity(std::move(unit));

    for (int i = 0; i < 5; ++i) {
        brain.bank(30.0, 300.0);
        economy_tick(brain, registry, 0.1);
    }
    CHECK(brain.get_stat("Economy_Trend_" + lane) == Catch::Approx(produced - asked).margin(1e-9));
}

TEST_CASE("Economy_TotalProduced sums each tick's Economy_Income, whatever its sign",
          "[army][stats][economy]") {
    const double given = GENERATE(-30.0, -10.0, 0.0, 20.0);
    osc::sim::EntityRegistry registry;
    osc::sim::ArmyBrain brain;
    brain.set_index(0);
    auto producer = std::make_unique<osc::sim::Unit>();
    producer->set_army(0);
    producer->economy().production_active = true;
    producer->economy().production_mass = 10.0;
    producer->economy().production_energy = 10.0;
    registry.register_entity(std::move(producer));

    double incomes_mass = 0.0;
    double incomes_energy = 0.0;
    for (int i = 0; i < 10; ++i) {
        brain.bank(given, given);
        economy_tick(brain, registry, 0.1);
        incomes_mass += brain.get_stat("Economy_Income_Mass");
        incomes_energy += brain.get_stat("Economy_Income_Energy");
    }
    CHECK(incomes_mass == Catch::Approx(10.0 + given).margin(1e-9));
    CHECK(brain.get_stat("Economy_TotalProduced_Mass") == Catch::Approx(incomes_mass).margin(1e-9));
    CHECK(brain.get_stat("Economy_TotalProduced_Energy") ==
          Catch::Approx(incomes_energy).margin(1e-9));
}

TEST_CASE("A paused unit keeps producing, and its army pays what the unit asks",
          "[army][economy][pause]") {
    osc::sim::EntityRegistry registry;
    osc::sim::ArmyBrain brain;
    brain.set_index(0);
    auto extractor = std::make_unique<osc::sim::Unit>();
    extractor->set_army(0);
    extractor->economy().production_mass = 2.0;
    extractor->economy().production_active = true;
    extractor->set_paused(true);
    auto shield = std::make_unique<osc::sim::Unit>();
    shield->set_army(0);
    shield->economy().consumption_mass = 4.0;
    shield->economy().consumption_active = true;
    shield->set_paused(true);
    auto* s = shield.get();
    registry.register_entity(std::move(extractor));
    registry.register_entity(std::move(shield));

    for (int i = 0; i < 10; ++i) {
        economy_tick(brain, registry, 0.1);
    }
    CHECK(std::abs(brain.get_stat("Economy_TotalProduced_Mass") - 2.0) < 1e-9);
    CHECK(brain.economy().mass.requested == Catch::Approx(4.0));
    CHECK(brain.get_stat("Economy_TotalConsumed_Mass") > 0.0);

    s->economy().consumption_active = false;
    tick_until([&] { economy_tick(brain, registry, 0.1); }, "the shield stops asking", 20,
               [&] { return brain.economy().mass.requested == 0.0; });
}

TEST_CASE("A reclaimer's own production stays beside its reclaim, and after it",
          "[army][economy]") {
    osc::sim::EntityRegistry registry;
    osc::sim::ArmyBrain brain;
    brain.set_index(0);
    auto acu = std::make_unique<osc::sim::Unit>();
    acu->set_army(0);
    acu->economy().production_mass = 1.0;
    acu->economy().production_active = true;
    acu->economy().reclaim_mass = 3.0;
    auto* a = acu.get();
    registry.register_entity(std::move(acu));

    for (int i = 0; i < 10; ++i) {
        economy_tick(brain, registry, 0.1);
    }
    CHECK(std::abs(brain.get_stat("Economy_TotalProduced_Mass") - 4.0) < 1e-9);

    a->stop_reclaiming();
    CHECK(a->economy().production_mass == 1.0);
    CHECK(a->economy().production_active);
    CHECK(a->economy().reclaim_mass == 0.0);
}

TEST_CASE("A producer with a request produces at its grant, unless it is a natural producer",
          "[army][economy]") {
    const bool natural = GENERATE(false, true);
    const double share = GENERATE(0.0, 0.25, 0.5, 1.0);
    CAPTURE(natural, share);
    osc::sim::EntityRegistry registry;
    osc::sim::ArmyBrain brain;
    brain.set_index(0);
    auto generator = std::make_unique<osc::sim::Unit>();
    generator->set_army(0);
    generator->economy().production_energy = 20.0 * share;
    generator->economy().production_active = true;
    auto extractor = std::make_unique<osc::sim::Unit>();
    extractor->set_army(0);
    extractor->economy().production_mass = 4.0;
    extractor->economy().production_active = true;
    extractor->economy().consumption_energy = 20.0;
    extractor->economy().consumption_active = true;
    extractor->economy().natural_producer = natural;
    auto* x = extractor.get();
    registry.register_entity(std::move(generator));
    registry.register_entity(std::move(extractor));

    for (int i = 0; i < 3; ++i) {
        economy_tick(brain, registry, 0.1);
    }
    CHECK(x->economy().resource_consumed == Catch::Approx(share));
    CHECK(brain.economy().energy.income == Catch::Approx(20.0 * share));
    CHECK(brain.economy().mass.income == Catch::Approx(natural ? 4.0 : 4.0 * share));

    x->economy().consumption_active = false;
    economy_tick(brain, registry, 0.1);
    CHECK(brain.economy().mass.income == Catch::Approx(4.0));
}

TEST_CASE("A stats trigger fires once, when all its conditions hold", "[army][stats]") {
    osc::sim::ArmyBrain brain;
    brain.set_stat("Units_Active", 3);
    brain.set_stat("Economy_Ratio_Mass", 0.05);
    brain.add_stat_trigger({"Economy_Ratio_Mass", "Low", "LessThan", 0.1, std::nullopt});
    brain.add_stat_trigger({"Units_Active", "Both", "GreaterThanOrEqual", 3, std::nullopt});
    brain.add_stat_trigger({"Economy_Ratio_Mass", "Both", "GreaterThan", 0.5, std::nullopt});
    brain.add_stat_trigger({"Units_Active", "Cat", "GreaterThan", 0, std::set<std::string>{}});

    CHECK(brain.take_fired_triggers() == std::vector<std::string>{"Low"});
    CHECK(brain.take_fired_triggers().empty());

    brain.set_stat("Economy_Ratio_Mass", 0.6);
    CHECK(brain.take_fired_triggers() == std::vector<std::string>{"Both"});

    brain.remove_stat_trigger("Units_Active", "Cat");
    brain.add_stat_trigger({"Units_Active", "Equal3", "Equal", 3, std::nullopt});
    CHECK(brain.take_fired_triggers() == std::vector<std::string>{"Equal3"});
}

TEST_CASE("ArmyBrain explicit color state", "[army][color]") {
    osc::sim::ArmyBrain brain;

    SECTION("default brain has no explicit color override") {
        REQUIRE_FALSE(brain.has_color());
    }

    SECTION("set_color marks color as explicit") {
        brain.set_color(32, 64, 128);
        REQUIRE(brain.has_color());
        REQUIRE(brain.color_r() == 32);
        REQUIRE(brain.color_g() == 64);
        REQUIRE(brain.color_b() == 128);
    }
}

TEST_CASE("Cloak intel drives cloak flag", "[cloak][economy]") {
    osc::sim::Unit unit;
    unit.add_intel("Cloak", 0.0f);
    REQUIRE_FALSE(unit.is_cloaked()); // blueprint intel starts off

    unit.enable_intel("Cloak");
    REQUIRE(unit.is_cloaked());

    unit.disable_intel("Cloak");
    REQUIRE_FALSE(unit.is_cloaked());
}

TEST_CASE("GiveStorage replaces the army's one extra pair, which outlives the recount",
          "[army][economy]") {
    osc::sim::EntityRegistry registry;
    osc::sim::ArmyBrain brain;
    brain.set_index(0);

    auto unit = std::make_unique<osc::sim::Unit>();
    unit->set_army(0);
    unit->economy().storage_energy = 4000.0;
    unit->economy().storage_mass = 650.0;
    unit->economy().production_active = true;
    registry.register_entity(std::move(unit));

    // An army has no storage of its own: its commander's, as the original's
    // starts with 4000 and 650
    economy_tick(brain, registry, 0.1);
    REQUIRE(brain.economy().energy.max_storage == 4000.0);
    REQUIRE(brain.economy().mass.max_storage == 650.0);

    brain.set_extra_storage(0.0, 10000.0);
    // Usable at once: storage can be filled before the next economy tick.
    REQUIRE(brain.economy().energy.max_storage == 14000.0);
    economy_tick(brain, registry, 0.1);
    economy_tick(brain, registry, 0.1);
    REQUIRE(brain.economy().energy.max_storage == 14000.0);
    REQUIRE(brain.economy().mass.max_storage == 650.0);

    // A second call replaces the pair: GiveStorage('MASS', 500) after
    // GiveStorage('ENERGY', 10000) leaves no extra energy (the original:
    // energy stays at its units' 4000)
    brain.set_extra_storage(500.0, 0.0);
    REQUIRE(brain.economy().energy.max_storage == 4000.0);
    REQUIRE(brain.economy().mass.max_storage == 1150.0);
    economy_tick(brain, registry, 0.1);
    REQUIRE(brain.economy().energy.max_storage == 4000.0);
    REQUIRE(brain.economy().mass.max_storage == 1150.0);

    // Never under nothing
    brain.set_extra_storage(-5000.0, 0.0);
    economy_tick(brain, registry, 0.1);
    REQUIRE(brain.economy().mass.max_storage == 0.0);
}

TEST_CASE("EnableIntel only enables intel the unit has", "[intel]") {
    // Retail SetupIntel calls EnableIntel for every intel type it knows
    // (Radar, Sonar, Omni, Cloak, stealth, Jammer, ...) and checks
    // IsIntelEnabled afterwards: in Moho the call does nothing for intel the
    // unit lacks. Enabling it anyway cloaked and stealthed every unit.
    osc::sim::Unit unit;
    unit.add_intel("Vision", 26.0f);
    unit.enable_intel("Vision");
    unit.add_intel("RadarStealth", 0.0f);

    for (const char* intel : {"Radar", "Omni", "Cloak", "RadarStealth",
                              "SonarStealth", "Jammer"}) {
        unit.enable_intel(intel);
    }
    REQUIRE(unit.is_intel_enabled("Vision"));
    REQUIRE(unit.is_intel_enabled("RadarStealth"));
    REQUIRE(unit.has_radar_stealth());
    REQUIRE_FALSE(unit.is_intel_enabled("Cloak"));
    REQUIRE_FALSE(unit.is_cloaked());
    REQUIRE_FALSE(unit.is_intel_enabled("SonarStealth"));
    REQUIRE_FALSE(unit.has_sonar_stealth());
    REQUIRE_FALSE(unit.is_intel_enabled("Radar"));
    REQUIRE_FALSE(unit.is_intel_enabled("Jammer"));

    // add_intel never resets intel the unit already has.
    unit.add_intel("Vision", 5.0f);
    REQUIRE(unit.is_intel_enabled("Vision"));
    REQUIRE(unit.get_intel_radius("Vision") == 26.0f);

    // InitIntel makes a new handle, off until EnableIntel (Moho's; retail
    // always enables it next).
    unit.init_intel("Omni", 30.0f);
    REQUIRE_FALSE(unit.is_intel_enabled("Omni"));
    unit.enable_intel("Omni");
    REQUIRE(unit.is_intel_enabled("Omni"));
}

TEST_CASE("Energy stall disables cloak maintenance", "[cloak][economy]") {
    osc::sim::EntityRegistry registry;
    osc::sim::ArmyBrain brain;
    brain.set_index(0);
    brain.set_stored_resources(0.0, 0.0);

    auto cloaked = std::make_unique<osc::sim::Unit>();
    cloaked->set_army(0);
    cloaked->add_intel("Cloak", 0.0f);
    cloaked->enable_intel("Cloak");
    REQUIRE(cloaked->is_cloaked());
    cloaked->economy().maintenance_active = true;
    cloaked->economy().energy_maintenance_override = 100.0;
    auto unit_id = registry.register_entity(std::move(cloaked));

    brain.update_economy(registry, 1.0);

    auto* unit = static_cast<osc::sim::Unit*>(registry.find(unit_id));
    REQUIRE(unit != nullptr);
    // Nothing granted (Moho's report of usage reads full here: see GrantRatios)
    REQUIRE(brain.grant_ratios().of(0.0, 100.0) == 0.0);
    REQUIRE_FALSE(unit->is_cloaked());
    REQUIRE_FALSE(unit->is_intel_enabled("Cloak"));
    REQUIRE_FALSE(unit->economy().maintenance_active);
}

TEST_CASE("Energy stall disables active intel maintenance toggles", "[intel][economy]") {
    osc::sim::EntityRegistry registry;
    osc::sim::ArmyBrain brain;
    brain.set_index(0);
    brain.set_stored_resources(0.0, 0.0);

    auto intel_unit = std::make_unique<osc::sim::Unit>();
    intel_unit->set_army(0);
    for (std::string_view intel : {
             "Radar", "Sonar", "Omni", "Jammer",
             "RadarStealth", "SonarStealth", "CloakField",
         }) {
        intel_unit->add_intel(std::string(intel), 10.0f);
        intel_unit->enable_intel(std::string(intel));
    }
    intel_unit->economy().maintenance_active = true;
    intel_unit->economy().energy_maintenance_override = 100.0;
    auto unit_id = registry.register_entity(std::move(intel_unit));

    brain.update_economy(registry, 1.0);

    auto* unit = static_cast<osc::sim::Unit*>(registry.find(unit_id));
    REQUIRE(unit != nullptr);
    // Nothing granted (Moho's report of usage reads full here: see GrantRatios)
    REQUIRE(brain.grant_ratios().of(0.0, 100.0) == 0.0);
    for (std::string_view intel : {
             "Radar", "Sonar", "Omni", "Jammer",
             "RadarStealth", "SonarStealth", "CloakField",
         }) {
        REQUIRE_FALSE(unit->is_intel_enabled(std::string(intel)));
    }
    REQUIRE_FALSE(unit->has_radar_stealth());
    REQUIRE_FALSE(unit->has_sonar_stealth());
    REQUIRE_FALSE(unit->economy().maintenance_active);
}

TEST_CASE("Energy stall turns off maintained shields", "[shield][economy]") {
    osc::sim::EntityRegistry registry;
    osc::sim::ArmyBrain brain;
    brain.set_index(0);
    brain.set_stored_resources(0.0, 0.0);

    auto owner = std::make_unique<osc::sim::Unit>();
    owner->set_army(0);
    owner->economy().maintenance_active = true;
    owner->economy().energy_maintenance_override = 100.0;
    auto owner_id = registry.register_entity(std::move(owner));

    auto shield = std::make_unique<osc::sim::Shield>();
    shield->set_army(0);
    shield->owner_id = owner_id;
    shield->is_on = true;
    auto shield_id = registry.register_entity(std::move(shield));

    auto* owner_unit = static_cast<osc::sim::Unit*>(registry.find(owner_id));
    REQUIRE(owner_unit != nullptr);
    owner_unit->set_shield_entity_id(shield_id);

    brain.update_economy(registry, 1.0);

    auto* updated_owner = static_cast<osc::sim::Unit*>(registry.find(owner_id));
    auto* updated_shield = static_cast<osc::sim::Shield*>(registry.find(shield_id));
    REQUIRE(updated_owner != nullptr);
    REQUIRE(updated_shield != nullptr);
    // Nothing granted (Moho's report of usage reads full here: see GrantRatios)
    REQUIRE(brain.grant_ratios().of(0.0, 100.0) == 0.0);
    REQUIRE_FALSE(updated_shield->is_on);
    REQUIRE_FALSE(updated_owner->economy().maintenance_active);
}

TEST_CASE("Moho cloak and stealth helpers update intel state", "[intel][lua]") {
    osc::lua::LuaState lua;
    osc::sim::SimState sim(lua.raw(), nullptr);
    osc::lua::register_moho_bindings(lua, sim);

    osc::sim::Unit unit;
    lua_State* L = lua.raw();
    lua_newtable(L);
    lua_pushstring(L, "_c_object");
    lua_pushlightuserdata(L, &unit);
    lua_rawset(L, -3);
    lua_setglobal(L, "unit");

    auto result = lua.do_string(R"(
        moho.unit_methods.EnableCloak(unit)
        cloak_enabled = moho.unit_methods.IsUnitCloaked(unit)
        moho.unit_methods.EnableStealth(unit)
        moho.unit_methods.EnableSonarStealth(unit)
    )");
    REQUIRE(result.ok());

    REQUIRE(unit.is_cloaked());
    REQUIRE(unit.is_intel_enabled("Cloak"));
    REQUIRE(unit.has_radar_stealth());
    REQUIRE(unit.is_intel_enabled("RadarStealth"));
    REQUIRE(unit.has_sonar_stealth());
    REQUIRE(unit.is_intel_enabled("SonarStealth"));

    result = lua.do_string(R"(
        moho.unit_methods.DisableCloak(unit)
        moho.unit_methods.DisableStealth(unit)
        moho.unit_methods.DisableSonarStealth(unit)
    )");
    REQUIRE(result.ok());

    REQUIRE_FALSE(unit.is_cloaked());
    REQUIRE_FALSE(unit.is_intel_enabled("Cloak"));
    REQUIRE_FALSE(unit.has_radar_stealth());
    REQUIRE_FALSE(unit.is_intel_enabled("RadarStealth"));
    REQUIRE_FALSE(unit.has_sonar_stealth());
    REQUIRE_FALSE(unit.is_intel_enabled("SonarStealth"));
}

TEST_CASE("LaunchSinglePlayerSession accepts GameOptions ScenarioFile", "[launch][lua]") {
    osc::lua::LuaState lua;
    osc::sim::SimState sim(lua.raw(), nullptr);
    osc::ui::UIControlRegistry ui_registry;
    osc::FrontEndData front_end_data;

    osc::lua::register_moho_bindings(lua, sim);
    osc::lua::register_ui_bindings(lua, ui_registry);

    lua_State* L = lua.raw();
    lua_pushstring(L, "__osc_front_end_data");
    lua_pushlightuserdata(L, &front_end_data);
    lua_rawset(L, LUA_REGISTRYINDEX);

    auto result = lua.do_string(R"(
        LaunchSinglePlayerSession({
            GameOptions = {
                ScenarioFile = '/maps/the_pass/the_pass_scenario.lua',
            },
            PlayerOptions = {
                [1] = { Human = true, Faction = 1, Team = 1, StartSpot = 1 },
            },
        })
    )");
    REQUIRE(result.ok());

    lua_pushstring(L, "__osc_launch_requested");
    lua_rawget(L, LUA_REGISTRYINDEX);
    bool launch_requested = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);

    lua_pushstring(L, "__osc_launch_scenario");
    lua_rawget(L, LUA_REGISTRYINDEX);
    std::string scenario = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "";
    lua_pop(L, 1);

    REQUIRE(launch_requested);
    REQUIRE(scenario == "/maps/the_pass/the_pass_scenario.lua");

    front_end_data.clear();
}

TEST_CASE("ReturnToLobby is available from UI bindings", "[score][lua]") {
    osc::lua::LuaState lua;
    osc::sim::SimState sim(lua.raw(), nullptr);
    osc::ui::UIControlRegistry ui_registry;

    osc::lua::register_moho_bindings(lua, sim);
    osc::lua::register_ui_bindings(lua, ui_registry);

    auto result = lua.do_string("ReturnToLobby()");
    REQUIRE(result.ok());

    lua_State* L = lua.raw();
    lua_pushstring(L, "__osc_return_to_lobby");
    lua_rawget(L, LUA_REGISTRYINDEX);
    bool return_requested = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);

    REQUIRE(return_requested);
}

TEST_CASE("UI GetFocusArmy answers the focus army, 1-based", "[frontend][lua]") {
    osc::lua::LuaState lua;
    osc::sim::SimState sim(lua.raw(), nullptr);
    osc::ui::UIControlRegistry ui_registry;

    osc::lua::register_moho_bindings(lua, sim);
    osc::lua::register_ui_bindings(lua, ui_registry);

    lua_State* L = lua.raw();
    lua_pushstring(L, "__osc_focus_army");
    lua_pushnumber(L, 1);
    lua_rawset(L, LUA_REGISTRYINDEX);

    auto result = lua.do_string("focus_army = GetFocusArmy()");
    REQUIRE(result.ok());

    lua_getglobal(L, "focus_army");
    double focus_army = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : 0.0;
    lua_pop(L, 1);
    CHECK(focus_army == 2.0);
}

TEST_CASE("UI unit handles stop resolving once the unit is unregistered", "[ui][lua]") {
    // UI scripts keep unit objects across beats (avatars, idle lists,
    // selections). They must resolve through the registry, never hold a
    // pointer: an unregistered entity waits in the graveyard and is freed at
    // the beat's end, while the script's handle lives on.
    osc::lua::LuaState lua;
    osc::sim::SimState sim(lua.raw(), nullptr);
    osc::ui::UIControlRegistry ui_registry;
    osc::lua::register_moho_bindings(lua, sim);
    osc::lua::register_ui_bindings(lua, ui_registry);

    auto acu = std::make_unique<osc::sim::Unit>();
    acu->set_army(0);
    acu->add_category("COMMAND");
    const auto id = sim.entity_registry().register_entity(std::move(acu));

    auto result = lua.do_string(R"(
        kept = GetArmyAvatars()[1]
        alive_idle = kept:IsIdle() and 1 or 0
    )");
    INFO((result.ok() ? std::string() : result.error().message));
    REQUIRE(result.ok());
    lua_State* L = lua.raw();
    lua_getglobal(L, "alive_idle");
    CHECK(lua_tonumber(L, -1) == 1.0); // resolves: an idle unit
    lua_pop(L, 1);

    sim.entity_registry().unregister_entity(id); // into the graveyard
    sim.tick(); // the UI sees the world a tick at a time (M191 step 3)
    result = lua.do_string(R"(
        dead_idle = kept:IsIdle() and 1 or 0
        dead_avatars = GetArmyAvatars() == nil and 0 or 1 -- none: nil, as retail tests
    )");
    REQUIRE(result.ok());
    lua_getglobal(L, "dead_idle");
    CHECK(lua_tonumber(L, -1) == 0.0); // no longer resolves to the old unit
    lua_pop(L, 1);
    lua_getglobal(L, "dead_avatars");
    CHECK(lua_tonumber(L, -1) == 0.0);
    lua_pop(L, 1);
    sim.entity_registry().collect_garbage();
}

TEST_CASE("UI SetFocusArmy updates focus army registry state", "[frontend][lua]") {
    osc::lua::LuaState lua;
    osc::sim::SimState sim(lua.raw(), nullptr);
    osc::ui::UIControlRegistry ui_registry;

    osc::lua::register_moho_bindings(lua, sim);
    osc::lua::register_ui_bindings(lua, ui_registry);
    // A game loading: a session (with none, Moho's SetFocusArmy raises), but
    // no game interface yet, so the focus changes at once.
    osc::GameStateManager game_state;
    game_state.transition_to(osc::GameState::LOADING, nullptr);
    lua_pushstring(lua.raw(), "__osc_game_state_mgr");
    lua_pushlightuserdata(lua.raw(), &game_state);
    lua_rawset(lua.raw(), LUA_REGISTRYINDEX);

    auto result = lua.do_string(R"(
        SetFocusArmy(2)
        focus_army = GetFocusArmy()
    )");
    REQUIRE(result.ok());

    lua_State* L = lua.raw();
    lua_getglobal(L, "focus_army");
    double focus_army = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : 0.0;
    lua_pop(L, 1);

    REQUIRE(focus_army == 2.0);
}

TEST_CASE("UI focus army preserves observer mode", "[frontend][lua]") {
    osc::lua::LuaState lua;
    osc::sim::SimState sim(lua.raw(), nullptr);
    osc::ui::UIControlRegistry ui_registry;

    osc::lua::register_moho_bindings(lua, sim);
    osc::lua::register_ui_bindings(lua, ui_registry);
    osc::GameStateManager game_state; // a game loading, as above
    game_state.transition_to(osc::GameState::LOADING, nullptr);
    lua_pushstring(lua.raw(), "__osc_game_state_mgr");
    lua_pushlightuserdata(lua.raw(), &game_state);
    lua_rawset(lua.raw(), LUA_REGISTRYINDEX);

    auto result = lua.do_string(R"(
        SetFocusArmy(-1)
        focus_army = GetFocusArmy()
    )");
    REQUIRE(result.ok());

    lua_State* L = lua.raw();
    lua_getglobal(L, "focus_army");
    double focus_army = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : 0.0;
    lua_pop(L, 1);

    REQUIRE(focus_army == -1.0);
}

TEST_CASE("SessionManager slot configs override AI and setup defaults", "[session][config]") {
    osc::lua::SessionManager mgr;

    std::vector<osc::lua::ArmySlotConfig> slots(2);
    slots[0].configured = true;
    slots[0].human = true;
    slots[0].faction = 3;
    slots[0].team = 2;
    slots[0].start_spot = 4;
    slots[0].player_color = 6;
    slots[0].army_color = 7;

    slots[1].configured = true;
    slots[1].human = false;
    slots[1].faction = 2;
    slots[1].team = 2;
    slots[1].start_spot = 1;
    slots[1].ai_personality = "rushcheat";

    mgr.set_army_slot_configs(slots);

    REQUIRE_FALSE(mgr.is_ai_army(0));
    REQUIRE(mgr.is_ai_army(1));

    auto first = mgr.slot_config_for_army(0);
    REQUIRE(first != nullptr);
    REQUIRE(first->faction == 3);
    REQUIRE(first->team == 2);
    REQUIRE(first->start_spot == 4);
    REQUIRE(first->player_color == 6);
    REQUIRE(first->army_color == 7);

    auto second = mgr.slot_config_for_army(1);
    REQUIRE(second != nullptr);
    REQUIRE(second->ai_personality == "rushcheat");
}

TEST_CASE("SessionManager applies lobby game option overrides", "[session][config]") {
    osc::lua::LuaState lua;
    osc::sim::SimState sim(lua.raw(), nullptr);
    osc::lua::SessionManager mgr;
    osc::lua::ScenarioMetadata meta;

    auto setup = lua.do_string("ScenarioInfo = {}");
    REQUIRE(setup.ok());

    osc::lua::GameOptionsConfig options;
    options.configured = true;
    options.set_string("Victory", "sandbox");
    options.set_string("FogOfWar", "none");
    options.set_number("UnitCap", 750);
    options.set_string("PrebuiltUnits", "On");
    options.set_bool("CheatsEnabled", true);
    options.set_number("CheatMult", 3.5);
    options.set_number("BuildMult", 4.25);
    options.restricted_categories = {"NUKE", "EXPERIMENTAL"};
    mgr.set_game_options(options);

    auto result = mgr.start_session(lua, osc::vfs::VirtualFileSystem{}, sim, meta);
    REQUIRE(result.ok());

    auto check_string = [&](const char* key) {
        lua_getglobal(lua.raw(), "ScenarioInfo");
        lua_pushstring(lua.raw(), "Options");
        lua_rawget(lua.raw(), -2);
        lua_pushstring(lua.raw(), key);
        lua_rawget(lua.raw(), -2);
        std::string value = lua_type(lua.raw(), -1) == LUA_TSTRING
            ? lua_tostring(lua.raw(), -1)
            : "";
        lua_pop(lua.raw(), 3);
        return value;
    };

    auto check_number = [&](const char* key) {
        lua_getglobal(lua.raw(), "ScenarioInfo");
        lua_pushstring(lua.raw(), "Options");
        lua_rawget(lua.raw(), -2);
        lua_pushstring(lua.raw(), key);
        lua_rawget(lua.raw(), -2);
        double value = lua_isnumber(lua.raw(), -1) ? lua_tonumber(lua.raw(), -1) : 0.0;
        lua_pop(lua.raw(), 3);
        return value;
    };

    auto check_bool = [&](const char* key) {
        lua_getglobal(lua.raw(), "ScenarioInfo");
        lua_pushstring(lua.raw(), "Options");
        lua_rawget(lua.raw(), -2);
        lua_pushstring(lua.raw(), key);
        lua_rawget(lua.raw(), -2);
        bool value = lua_toboolean(lua.raw(), -1) != 0;
        lua_pop(lua.raw(), 3);
        return value;
    };

    CHECK(check_string("Victory") == "sandbox");
    CHECK(check_string("FogOfWar") == "none");
    CHECK(check_string("PrebuiltUnits") == "On");
    CHECK(check_number("UnitCap") == 750);
    CHECK(check_bool("CheatsEnabled"));
    CHECK(check_number("CheatMult") == 3.5);
    CHECK(check_number("BuildMult") == 4.25);

    lua_getglobal(lua.raw(), "ScenarioInfo");
    lua_pushstring(lua.raw(), "Options");
    lua_rawget(lua.raw(), -2);
    lua_pushstring(lua.raw(), "RestrictedCategories");
    lua_rawget(lua.raw(), -2);
    lua_rawgeti(lua.raw(), -1, 1);
    CHECK(std::string(lua_tostring(lua.raw(), -1)) == "NUKE");
    lua_pop(lua.raw(), 1);
    lua_rawgeti(lua.raw(), -1, 2);
    CHECK(std::string(lua_tostring(lua.raw(), -1)) == "EXPERIMENTAL");
    lua_pop(lua.raw(), 4);
}

TEST_CASE("SessionManager seeds native lobby build rules", "[session][rules]") {
    osc::lua::LuaState lua;
    osc::sim::SimState sim(lua.raw(), nullptr);
    osc::lua::SessionManager mgr;
    osc::lua::ScenarioMetadata meta;
    meta.armies = {"ARMY_1", "ARMY_2"};
    sim.add_army("ARMY_1", "ARMY_1");
    sim.add_army("ARMY_2", "ARMY_2");

    auto setup = lua.do_string("ScenarioInfo = {}");
    REQUIRE(setup.ok());

    osc::lua::GameOptionsConfig options;
    options.configured = true;
    options.set_number("UnitCap", 42);
    mgr.set_game_options(options);

    auto result = mgr.start_session(lua, osc::vfs::VirtualFileSystem{}, sim, meta);
    REQUIRE(result.ok());

    for (int army = 0; army < 2; ++army) {
        auto* brain = sim.get_army(army);
        REQUIRE(brain != nullptr);
        CHECK(brain->unit_cap() == 42);
    }
}

TEST_CASE("Sandbox victory option suppresses automatic army defeat", "[session][rules]") {
    osc::lua::LuaState lua;
    osc::sim::SimState sim(lua.raw(), nullptr);
    osc::lua::SessionManager mgr;
    osc::lua::ScenarioMetadata meta;
    meta.armies = {"ARMY_1"};
    sim.add_army("ARMY_1", "ARMY_1");

    auto setup = lua.do_string("ScenarioInfo = {}");
    REQUIRE(setup.ok());

    osc::lua::GameOptionsConfig options;
    options.configured = true;
    options.set_string("Victory", "sandbox");
    mgr.set_game_options(options);

    auto result = mgr.start_session(lua, osc::vfs::VirtualFileSystem{}, sim, meta);
    REQUIRE(result.ok());
    CHECK(sim.sandbox_victory());

    for (int i = 0; i < 60; ++i) {
        sim.tick();
    }

    auto* brain = sim.get_army(0);
    REQUIRE(brain != nullptr);
    CHECK(brain->state() == osc::sim::BrainState::InProgress);
    CHECK(sim.player_result() == 0);
}

TEST_CASE("A lobby's slots taken are the armies, in order, each its slot's", "[session][config]") {
    // Random spawn leaves gaps: slots 2, 5 and 7 taken (a table this Lua
    // iterates 7, 5, 2)
    osc::lua::LuaState lua;
    auto made = lua.do_string(R"(
        config = {PlayerOptions = {}}
        config.PlayerOptions[7] = {Human = true, Faction = 4}
        config.PlayerOptions[5] = {Human = true, Faction = 2}
        config.PlayerOptions[2] = {Human = false, AIPersonality = 'rush'}
    )");
    REQUIRE(made.ok());
    lua_getglobal(lua.raw(), "config");
    const auto setup = osc::lua::read_session_config(lua.raw(), lua_gettop(lua.raw()));
    lua_pop(lua.raw(), 1);

    REQUIRE(setup.slots.size() == 3);
    CHECK(setup.army_count == 3);
    CHECK(setup.slots[0].slot == 2);
    CHECK_FALSE(setup.slots[0].human);
    CHECK(setup.slots[1].slot == 5);
    CHECK(setup.slots[1].faction == 2);
    CHECK(setup.slots[2].slot == 7);
    CHECK(setup.slots[2].faction == 4);
    CHECK(setup.ai_armies == std::vector<int>{0}); // the army, not the slot
    CHECK(setup.slots[1].team == 5);               // FFA: each its own

    // Named for their slots, as Moho's LaunchGame names them
    const std::vector<std::string> eight = {"ARMY_1", "ARMY_2", "ARMY_3", "ARMY_4",
                                            "ARMY_5", "ARMY_6", "ARMY_7", "ARMY_8"};
    CHECK(osc::sim::session_army_names(setup, eight) ==
          std::vector<std::string>{"ARMY_2", "ARMY_5", "ARMY_7"});
    // A slot the scenario hasn't takes its first free army, in its place
    CHECK(osc::sim::session_army_names(setup, {"ARMY_1", "ARMY_2", "ARMY_3", "ARMY_4", "ARMY_5"}) ==
          std::vector<std::string>{"ARMY_2", "ARMY_5", "ARMY_1"});
    // A key that is no slot takes the first army free, after the slots
    // have their own (never a real slot's)
    osc::sim::GameSetup odd;
    odd.slots.resize(2);
    odd.slots[0].slot = 0;
    odd.slots[1].slot = 1;
    CHECK(osc::sim::session_army_names(odd, eight) == std::vector<std::string>{"ARMY_2", "ARMY_1"});
    // Without a lobby: the scenario's first armies
    osc::sim::GameSetup plain;
    plain.army_count = 2;
    CHECK(osc::sim::session_army_names(plain, eight) ==
          std::vector<std::string>{"ARMY_1", "ARMY_2"});
    plain.army_count = 0;
    CHECK(osc::sim::session_army_names(plain, eight).size() == 8);
}

TEST_CASE("Each army starts at the marker bearing its name", "[session]") {
    osc::lua::LuaState lua;
    osc::sim::SimState sim(lua.raw(), nullptr);
    osc::lua::SessionManager mgr;
    osc::lua::ScenarioMetadata meta;
    meta.armies = {"ARMY_1", "ARMY_2", "ARMY_3", "ARMY_4"};
    // A lobby's players in slots 2 and 4
    sim.add_army("ARMY_2", "ARMY_2");
    sim.add_army("ARMY_4", "ARMY_4");
    std::vector<osc::lua::ArmySlotConfig> slots(2);
    for (auto& slot : slots) {
        slot.configured = true;
        slot.start_spot = 1; // retail's lobby leaves it 1: not where they start
    }
    slots[0].slot = 2;
    slots[1].slot = 4;
    mgr.set_army_slot_configs(slots);
    auto setup = lua.do_string(R"(
        ScenarioInfo = {}
        Scenario = {MasterChain = {_MASTERCHAIN_ = {Markers = {
            ARMY_1 = {position = {10, 1, 10}},
            ARMY_2 = {position = {20, 2, 20}},
            ARMY_4 = {position = {40, 4, 40}},
            Mass_01 = {position = {5, 0, 5}},
        }}}}
    )");
    REQUIRE(setup.ok());
    REQUIRE(mgr.start_session(lua, osc::vfs::VirtualFileSystem{}, sim, meta).ok());

    CHECK(sim.get_army(0)->start_position().x == 20.0f);
    CHECK(sim.get_army(0)->start_position().y == 2.0f);
    CHECK(sim.get_army(1)->start_position().x == 40.0f);
    // ArmySetup: the game's armies, by name, in order
    auto result = lua.do_string(R"(
        local setup = ScenarioInfo.ArmySetup
        assert(setup.ARMY_2 and setup.ARMY_2.ArmyIndex == 1, 'ARMY_2')
        assert(setup.ARMY_4 and setup.ARMY_4.ArmyIndex == 2, 'ARMY_4')
        assert(setup.ARMY_1 == nil and setup.ARMY_3 == nil, 'only the armies that play')
    )");
    CHECK(result.ok());
}

TEST_CASE("A lobby's sessionConfig becomes the game's setup", "[session][config][replay]") {
    osc::lua::LuaState lua;
    auto made = lua.do_string(R"(
        config = {
            GameOptions = {Victory = 'domination', UnitCap = 300},
            PlayerOptions = {
                [1] = {Human = true, Faction = 2, Team = 2, StartSpot = 3, PlayerColor = 4},
                [2] = {Human = false, AIPersonality = 'rush', Faction = 3, Handicap = 10},
            },
        }
    )");
    REQUIRE(made.ok());
    lua_getglobal(lua.raw(), "config");
    const auto setup = osc::lua::read_session_config(lua.raw(), lua_gettop(lua.raw()));
    lua_pop(lua.raw(), 1);

    REQUIRE(setup.slots.size() == 2);
    CHECK(setup.slots[0].configured);
    CHECK(setup.slots[0].human);
    CHECK(setup.slots[0].faction == 2);
    CHECK(setup.slots[0].team == 2);
    CHECK(setup.slots[0].start_spot == 3);
    CHECK(setup.slots[0].player_color == 4);
    CHECK_FALSE(setup.slots[1].human);
    CHECK(setup.slots[1].ai_personality == "rush");
    CHECK(setup.slots[1].team == 2); // defaults to the slot
    CHECK(setup.slots[1].start_spot == 2);
    CHECK(setup.slots[1].handicap == 10);
    CHECK(setup.army_count == 2);
    CHECK(setup.ai_armies == std::vector<int>{1});
    CHECK(setup.ai_personality == "rush");
    CHECK(setup.options.configured);
    CHECK(setup.options.values.size() == 2);
}

TEST_CASE("GameOptions parser preserves lobby scalar values and restrictions", "[session][config]") {
    osc::lua::LuaState lua;
    auto setup = lua.do_string(R"(
        options = {
            ScenarioFile = '/maps/example/example_scenario.lua',
            Victory = 'sandbox',
            UnitCap = 500,
            CheatsEnabled = true,
            CheatMult = 2.75,
            RestrictedCategories = {'NUKE', 'EXPERIMENTAL'},
        }
    )");
    REQUIRE(setup.ok());

    lua_getglobal(lua.raw(), "options");
    auto options = osc::lua::read_game_options(lua.raw(), lua_gettop(lua.raw()));
    lua_pop(lua.raw(), 1);

    REQUIRE(options.configured);
    REQUIRE(options.restricted_categories.size() == 2);
    CHECK(options.restricted_categories[0] == "NUKE");
    CHECK(options.restricted_categories[1] == "EXPERIMENTAL");

    bool saw_victory = false;
    bool saw_unit_cap = false;
    bool saw_cheats = false;
    bool saw_cheat_mult = false;
    bool saw_scenario = false;
    for (const auto& [key, value] : options.values) {
        if (key == "Victory") {
            saw_victory = value.type == osc::lua::GameOptionValue::Type::String &&
                          value.string_value == "sandbox";
        } else if (key == "UnitCap") {
            saw_unit_cap = value.type == osc::lua::GameOptionValue::Type::Number &&
                           value.number_value == 500;
        } else if (key == "CheatsEnabled") {
            saw_cheats = value.type == osc::lua::GameOptionValue::Type::Boolean &&
                         value.bool_value;
        } else if (key == "CheatMult") {
            saw_cheat_mult = value.type == osc::lua::GameOptionValue::Type::Number &&
                             value.number_value == 2.75;
        } else if (key == "ScenarioFile") {
            saw_scenario = true;
        }
    }

    CHECK(saw_victory);
    CHECK(saw_unit_cap);
    CHECK(saw_cheats);
    CHECK(saw_cheat_mult);
    CHECK_FALSE(saw_scenario);
}

TEST_CASE("The player plays the army set up as human, not the first", "[session]") {
    osc::lua::LuaState lua;
    osc::sim::SimState sim(lua.raw(), nullptr);
    sim.add_army("ARMY_1", "ARMY_1");
    sim.add_army("ARMY_2", "ARMY_2");
    sim.add_army("ARMY_5", "ARMY_5");

    // No setup (a game not launched from a lobby): every army is human, as
    // InitializeArmyAI makes it, and the player plays the first
    CHECK(osc::app::human_army(lua.raw(), sim) == 0);

    // Random spawn put the player in the lobby's fifth slot, an AI in the
    // first two
    REQUIRE(lua.do_string("ScenarioInfo = { ArmySetup = {\n"
                          "  ARMY_1 = { Human = false }, ARMY_2 = { Human = false },\n"
                          "  ARMY_5 = { Human = true } } }")
                .ok());
    CHECK(osc::app::human_army(lua.raw(), sim) == 2);

    // AIs alone: watched from the first
    REQUIRE(lua.do_string("ScenarioInfo.ArmySetup.ARMY_5.Human = false").ok());
    CHECK(osc::app::human_army(lua.raw(), sim) == 0);
}

TEST_CASE("A request is granted by whether it wants the resource that ran short", "[economy]") {
    // Energy ran short: every request wanting energy gets the pooled share
    osc::sim::GrantRatios energy_short{0.5f, 0.8f, false};
    CHECK(energy_short.of(0.0, 20.0) == 0.5f); // an upkeep
    CHECK(energy_short.of(4.0, 20.0) == 0.5f); // a build
    CHECK(energy_short.of(4.0, 0.0) == 0.8f);  // mass alone
    // Mass ran short: the build gets the pooled share, the upkeep its share
    // of what was left
    osc::sim::GrantRatios mass_short{0.3f, 0.9f, true};
    CHECK(mass_short.of(0.0, 20.0) == 0.9f);
    CHECK(mass_short.of(4.0, 20.0) == 0.3f);
}

namespace {

/// An army of one unit holding storage, and a builder and an upkeep, for
/// the economy's grants.
struct Economy {
    osc::sim::EntityRegistry registry;
    osc::sim::ArmyBrain brain;
    osc::sim::Unit* store = nullptr;
    osc::sim::Unit* builder = nullptr;
    osc::sim::Unit* upkeep = nullptr;

    Economy(double income_mass, double income_energy) {
        brain.set_index(0);
        auto make = [&]() {
            auto u = std::make_unique<osc::sim::Unit>();
            u->set_army(0);
            auto* raw = u.get();
            registry.register_entity(std::move(u));
            return raw;
        };
        store = make();
        store->economy().storage_mass = 1000;
        store->economy().storage_energy = 5000;
        store->economy().production_mass = income_mass;
        store->economy().production_energy = income_energy;
        store->economy().production_active = true;
        builder = make();
        upkeep = make();
        economy_tick(brain, registry, 0.1); // the storage counted
    }
};

} // namespace

TEST_CASE("An energy stall grants a build as little mass as energy, as Moho's economy does",
          "[economy]") {
    // The original game's probe (2026-10-14): a factory asking 4 mass and
    // 19 energy a second beside a Monkeylord's 250 energy upkeep, on 20
    // energy and 1 mass a second, no energy stored. Its progress ran at
    // income over asked; its army's mass rose (it took 0.036 a tick of the
    // 0.4 asked), where the engine had spent all 0.4 and its mass fell.
    Economy e(1.0, 20.0);
    e.brain.set_stored_resources(500.0, 0.0);
    e.builder->economy().consumption_active = true;
    e.builder->economy().consumption_mass = 4.0;
    e.builder->economy().consumption_energy = 19.0;
    e.upkeep->economy().maintenance_active = true;
    e.upkeep->economy().energy_maintenance_override = 250.0;
    economy_tick(e.brain, e.registry, 0.1);

    const osc::sim::GrantRatios& g = e.brain.grant_ratios();
    CHECK_FALSE(g.mass_limits);
    CHECK(g.dual == Catch::Approx(2.0 / 26.9));
    CHECK(g.of(4.0, 19.0) == g.dual);
    CHECK(g.of(0.0, 250.0) == g.dual); // it wants the limiting resource too
    CHECK(e.brain.economy().energy.stored == Catch::Approx(0.0).margin(1e-9));
    // 0.1 income less 0.4 x 2/26.9 spent
    CHECK(e.brain.economy().mass.stored == Catch::Approx(500.0 + 0.1 - 0.4 * 2.0 / 26.9));
    CHECK(e.brain.economy().mass.stored > 500.0);
}

TEST_CASE("A mass stall slows a build but not an energy upkeep", "[economy]") {
    // The pooled request (the build) is bounded by mass; the upkeep, wanting
    // only energy, shares what the build left of the energy, all of it here
    Economy e(0.0, 1000.0);
    e.brain.set_stored_resources(0.5, 4000.0);
    e.builder->economy().consumption_active = true;
    e.builder->economy().consumption_mass = 10.0;
    e.builder->economy().consumption_energy = 50.0;
    e.upkeep->economy().maintenance_active = true;
    e.upkeep->economy().energy_maintenance_override = 100.0;
    e.upkeep->add_intel("Radar", 50.0f);
    e.upkeep->enable_intel("Radar");
    economy_tick(e.brain, e.registry, 0.1);

    const osc::sim::GrantRatios& g = e.brain.grant_ratios();
    CHECK(g.mass_limits);
    CHECK(g.dual == Catch::Approx(0.5));
    CHECK(g.single == 1.0);
    CHECK(g.of(10.0, 50.0) == Catch::Approx(0.5));
    CHECK(g.of(0.0, 100.0) == 1.0);
    CHECK(e.builder->economy().consumption_request.limiting_rate() == Catch::Approx(0.5));
    // The upkeep is granted in full, so it stays on (a mass stall stops no
    // radar, cloak or shield)
    CHECK(e.upkeep->economy().maintenance_active);
    CHECK(e.upkeep->is_intel_enabled("Radar"));
    // The build's energy at its share, the upkeep's in full
    CHECK(e.brain.economy().energy.stored ==
          Catch::Approx(4000.0 + 100.0 - 5.0 * 0.5 - 10.0).margin(1e-9));
    CHECK(e.brain.economy().mass.stored == Catch::Approx(0.0).margin(1e-9));
    // As Moho reports them: mass used at the build's share; energy at each
    // request's
    CHECK(e.brain.mass_efficiency() == Catch::Approx(0.5));
    CHECK(e.brain.energy_efficiency() == Catch::Approx((50.0 * 0.5 + 100.0) / 150.0));
}

TEST_CASE("A request is worked from two ticks after it is made, as Moho's ResourceConsumed",
          "[economy]") {
    // The original game (the timing probe, 2026-10-14): a factory's new
    // unit appears a tick after the order, GetResourceConsumed answers 0 for
    // that tick and the next, and progress starts the tick after. A request
    // made in one tick is granted in the next, and worked from the one after.
    Economy e(5.0, 100.0);
    e.brain.set_stored_resources(800.0, 4000.0);
    auto& econ = e.builder->economy();
    CHECK(econ.resource_consumed == 0.0); // asking for nothing
    // The request is made during a tick's orders, after its economy
    econ.consumption_active = true;
    econ.consumption_mass = 4.0;
    econ.consumption_energy = 19.0;
    economy_tick(e.brain, e.registry, 0.1); // the next tick: granted
    CHECK(econ.consumption_request.limiting_rate() == 1.0f);
    CHECK(econ.resource_consumed == 0.0);
    economy_tick(e.brain, e.registry, 0.1); // the one after: worked
    CHECK(econ.resource_consumed == 1.0);
    // And when it stops asking, its work stops at once (the original: 0 the
    // tick after a factory's unit is done)
    econ.consumption_active = false;
    economy_tick(e.brain, e.registry, 0.1);
    CHECK(econ.resource_consumed == 0.0);
}

TEST_CASE("A request that stops before working its grant gives it back", "[economy]") {
    // Moho's SetConsumptionActive(false) returns the request's mGranted to
    // the army's next income: a build done (or cancelled) the tick after a
    // grant wastes nothing
    Economy e(0.0, 0.0);
    e.brain.set_stored_resources(500.0, 2000.0);
    auto& econ = e.builder->economy();
    econ.consumption_active = true;
    econ.consumption_mass = 4.0;
    econ.consumption_energy = 20.0;
    economy_tick(e.brain, e.registry, 0.1); // granted, taken from the stores
    CHECK(e.brain.economy().mass.stored == Catch::Approx(499.6));
    CHECK(e.brain.economy().energy.stored == Catch::Approx(1998.0));
    e.builder->set_consumption_active(false, &e.brain); // done before working it
    economy_tick(e.brain, e.registry, 0.1);
    CHECK(e.brain.economy().mass.stored == Catch::Approx(500.0));
    CHECK(e.brain.economy().energy.stored == Catch::Approx(2000.0));
    CHECK(e.brain.economy().mass.income == Catch::Approx(4.0));
}

TEST_CASE("An upkeep set through the engine's own fields reads granted too", "[economy]") {
    // Retail's scripts fold upkeep into the unit's consumption request; the
    // engine's SetMaintenanceConsumptionActive binding keeps it apart. Either
    // way GetResourceConsumed reads its grant: retail's IntelWatchThread
    // turns a radar off when it is not 1.
    Economy e(5.0, 100.0);
    e.brain.set_stored_resources(800.0, 4000.0);
    auto& econ = e.upkeep->economy();
    econ.maintenance_active = true;
    econ.energy_maintenance_override = 20.0;
    economy_tick(e.brain, e.registry, 0.1);
    economy_tick(e.brain, e.registry, 0.1);
    CHECK(econ.resource_consumed == 1.0);
    econ.maintenance_active = false;
    economy_tick(e.brain, e.registry, 0.1);
    CHECK(econ.resource_consumed == 0.0);
}

TEST_CASE("An economy with enough grants every request in full", "[economy]") {
    Economy e(5.0, 100.0);
    e.brain.set_stored_resources(800.0, 4000.0);
    e.builder->economy().consumption_active = true;
    e.builder->economy().consumption_mass = 4.0;
    e.builder->economy().consumption_energy = 19.0;
    economy_tick(e.brain, e.registry, 0.1);
    CHECK(e.brain.grant_ratios().dual == 1.0);
    CHECK(e.brain.grant_ratios().single == 1.0);
    CHECK(e.brain.mass_efficiency() == 1.0);
    CHECK(e.brain.economy().mass.stored == Catch::Approx(800.0 + 0.5 - 0.4));
}

TEST_CASE("A unit's request keeps what it was granted, and works at the share of what it asks "
          "that the grant covers",
          "[economy]") {
    enum class Change { None, Raised, Lowered, Stopped, Restarted };
    const Change change =
        GENERATE(Change::None, Change::Raised, Change::Lowered, Change::Stopped, Change::Restarted);
    CAPTURE(static_cast<int>(change));
    Economy e(5.0, 100.0);
    e.brain.set_stored_resources(800.0, 4000.0);
    auto& econ = e.builder->economy();
    econ.consumption_active = true;
    econ.consumption_energy = 2.0;
    for (int i = 0; i < 3; ++i) {
        economy_tick(e.brain, e.registry, 0.1);
    }
    switch (change) {
    case Change::None: break;
    case Change::Raised:
        econ.consumption_energy = 62.0;
        econ.consumption_mass = 10.0;
        break;
    case Change::Lowered: econ.consumption_energy = 1.0; break;
    case Change::Stopped:
    case Change::Restarted: e.builder->set_consumption_active(false, &e.brain); break;
    }
    e.registry.for_each_unit(
        [&](osc::sim::Entity& u) { static_cast<osc::sim::Unit&>(u).manage_resources(&e.brain); });
    const double worked = econ.resource_consumed;
    e.brain.update_economy(e.registry, 0.1);
    const double asked = e.brain.economy().energy.requested;
    const double income = e.brain.economy().energy.income;
    if (change == Change::Restarted) {
        e.builder->set_consumption_active(true, &e.brain);
        e.builder->manage_resources(&e.brain);
    }
    switch (change) {
    case Change::None:
        CHECK(worked == 1.0);
        CHECK(asked == Catch::Approx(2.0));
        CHECK(income == Catch::Approx(100.0));
        break;
    case Change::Raised:
        CHECK(worked == 0.0);
        CHECK(asked == Catch::Approx(60.0));
        CHECK(e.brain.economy().mass.requested == Catch::Approx(10.0));
        break;
    case Change::Lowered:
        CHECK(worked == 1.0);
        CHECK(asked == Catch::Approx(0.0).margin(1e-6));
        break;
    case Change::Stopped:
        CHECK(worked == 0.0);
        CHECK(asked == Catch::Approx(0.0).margin(1e-6));
        CHECK(income == Catch::Approx(102.0));
        break;
    case Change::Restarted:
        CHECK(income == Catch::Approx(102.0));
        CHECK(econ.resource_consumed == 1.0);
        break;
    }
}

TEST_CASE("An army's store is a float, as Moho's economy holds it", "[economy]") {
    // The original game's stallstart probe (2026-10-23, "midbuild"): a
    // factory asking 4 mass and 19 energy a second beside a Monkeylord's 250
    // energy upkeep, on 20 energy and 1 mass a second, 277 energy stored.
    // The store runs dry on the twelfth tick, granting 0.189592 of what was
    // asked, and the mass store reads 647.50 five ticks later. A double
    // store gave 0.189591 and 647.51.
    Economy e(1.0, 20.0);
    e.brain.set_stored_resources(650.5, 277.0);
    e.builder->economy().consumption_active = true;
    e.builder->economy().consumption_mass = 4.0;
    e.builder->economy().consumption_energy = 19.0;
    e.upkeep->economy().maintenance_active = true;
    e.upkeep->economy().energy_maintenance_override = 250.0;
    for (int tick = 0; tick < 12; ++tick) {
        economy_tick(e.brain, e.registry, 0.1);
    }
    CHECK(e.brain.grant_ratios().dual == Catch::Approx(0.1895923).epsilon(0).margin(3e-7));
    for (int tick = 0; tick < 4; ++tick) {
        economy_tick(e.brain, e.registry, 0.1);
    }
    CHECK(e.brain.economy().mass.stored == Catch::Approx(647.50).epsilon(0).margin(0.005));
}

TEST_CASE("An army sums what its requests are short of in floats, as Moho's economy does",
          "[economy]") {
    const float short_of = GENERATE(6e-8f, 1e-6f);
    CAPTURE(short_of);
    Economy e(0.0, 0.0);
    e.brain.set_stored_resources(0.0, 50.0);
    e.builder->economy().consumption_active = true;
    e.builder->economy().consumption_energy = 1000.0;
    osc::sim::EconRequest whole;
    whole.requested.energy = 1.0f;
    whole.granted.energy = 1.0f - short_of;
    e.brain.add_request(whole);
    e.brain.update_economy(e.registry, 0.1);
    CHECK(e.brain.grant_ratios().dual == 0.5);
}

TEST_CASE("GetUnitsAroundPoint and GetNumUnitsAroundPoint find centres strictly within the radius, "
          "as the original game does",
          "[army][lua]") {
    // Retail FA's answers (tools/moho_probes, 2026-10-12) for a 2x2 power
    // generator, asked from points about its centre.
    struct Row {
        osc::f32 dx, dy, dz, r;
        bool found;
    };
    const std::vector<Row> rows = {
        {0.0f, 0.0f, 0.0f, 1.0f, true},  {0.99f, 0.0f, 0.0f, 1.0f, true},
        {1.0f, 0.0f, 0.0f, 1.0f, false}, {1.01f, 0.0f, 0.0f, 1.0f, false},
        {3.0f, 0.0f, 0.0f, 1.0f, false}, {1.99f, 0.0f, 0.0f, 2.0f, true},
        {2.0f, 0.0f, 0.0f, 2.0f, false}, {4.99f, 0.0f, 0.0f, 5.0f, true},
        {5.0f, 0.0f, 0.0f, 5.0f, false}, {0.0f, 0.0f, 0.0f, 0.0f, false},
        {3.0f, 0.0f, 0.0f, 0.0f, false}, {0.6f, 0.9f, 0.0f, 1.0f, true},
        {0.6f, 0.0f, 0.6f, 1.0f, true},  {0.75f, 0.0f, 0.75f, 1.0f, false},
    };
    for (const auto& row : rows) {
        DYNAMIC_SECTION("offset " << row.dx << "," << row.dy << "," << row.dz << " r " << row.r) {
            osc::lua::LuaState lua;
            osc::sim::SimState sim(lua.raw(), nullptr);
            osc::lua::register_moho_bindings(lua, sim);
            sim.add_army("ARMY_1", "ARMY_1");
            lua_State* L = lua.raw();

            auto unit = std::make_unique<osc::sim::Unit>();
            unit->set_army(0);
            unit->add_category("LAND");
            unit->set_position({10.0f, 0.0f, 10.0f});
            osc::sim::CollisionShape box;
            box.type = osc::sim::CollisionShapeType::BOX;
            box.sx = 1.0f;
            box.sy = 0.5f;
            box.sz = 1.0f;
            unit->set_default_collision_shape(box);
            lua_newtable(L);
            lua_pushstring(L, "_c_object");
            lua_pushlightuserdata(L, unit.get());
            lua_rawset(L, -3);
            unit->set_lua_table_ref(luaL_ref(L, LUA_REGISTRYINDEX));
            sim.entity_registry().register_entity(std::move(unit));

            lua_newtable(L);
            lua_pushstring(L, "_c_object");
            lua_pushlightuserdata(L, sim.get_army(0));
            lua_rawset(L, -3);
            lua_setglobal(L, "brain");
            for (const auto& [name, value] :
                 {std::pair{"r", row.r}, std::pair{"qx", 10.0f + row.dx}, std::pair{"qy", row.dy},
                  std::pair{"qz", 10.0f + row.dz}}) {
                lua_pushnumber(L, value);
                lua_setglobal(L, name);
            }

            const auto result = lua.do_string(R"(
                local land = {__name = 'LAND'}
                listed = table.getn(moho.aibrain_methods.GetUnitsAroundPoint(brain, land, {qx, qy, qz}, r))
                counted = moho.aibrain_methods.GetNumUnitsAroundPoint(brain, land, {qx, qy, qz}, r)
            )");
            INFO((result.ok() ? std::string() : result.error().message));
            REQUIRE(result.ok());
            lua_getglobal(L, "listed");
            CHECK(lua_tonumber(L, -1) == (row.found ? 1.0 : 0.0));
            lua_getglobal(L, "counted");
            CHECK(lua_tonumber(L, -1) == (row.found ? 1.0 : 0.0));
            lua_pop(L, 2);
        }
    }
}
