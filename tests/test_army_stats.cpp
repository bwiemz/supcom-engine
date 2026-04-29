#include <catch2/catch_test_macros.hpp>
#include "sim/army_brain.hpp"
#include "sim/entity_registry.hpp"
#include "sim/manipulator.hpp"
#include "sim/unit.hpp"
#include "lua/session_manager.hpp"

#include <memory>

TEST_CASE("ArmyBrain stat storage", "[army][stats]") {
    osc::sim::ArmyBrain brain;
    SECTION("default stat returns default value") {
        REQUIRE(brain.get_stat("Units_Built", 0.0) == 0.0);
        REQUIRE(brain.get_stat("Mass_Collected", 42.0) == 42.0);
    }
    SECTION("set and get stat") {
        brain.set_stat("Units_Built", 5.0);
        REQUIRE(brain.get_stat("Units_Built") == 5.0);
    }
    SECTION("add_stat accumulates") {
        brain.add_stat("Units_Killed", 1.0);
        brain.add_stat("Units_Killed", 1.0);
        brain.add_stat("Units_Killed", 1.0);
        REQUIRE(brain.get_stat("Units_Killed") == 3.0);
    }
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

    unit.enable_intel("Cloak");
    REQUIRE(unit.is_cloaked());

    unit.disable_intel("Cloak");
    REQUIRE_FALSE(unit.is_cloaked());
}

TEST_CASE("Energy stall disables cloak maintenance", "[cloak][economy]") {
    osc::sim::EntityRegistry registry;
    osc::sim::ArmyBrain brain;
    brain.set_index(0);
    brain.set_stored_resources(0.0, 0.0);

    auto cloaked = std::make_unique<osc::sim::Unit>();
    cloaked->set_army(0);
    cloaked->set_cloaked(true);
    cloaked->enable_intel("Cloak");
    cloaked->economy().maintenance_active = true;
    cloaked->economy().energy_maintenance_override = 100.0;
    auto unit_id = registry.register_entity(std::move(cloaked));

    brain.update_economy(registry, 1.0);

    auto* unit = static_cast<osc::sim::Unit*>(registry.find(unit_id));
    REQUIRE(unit != nullptr);
    REQUIRE(brain.energy_efficiency() == 0.0);
    REQUIRE_FALSE(unit->is_cloaked());
    REQUIRE_FALSE(unit->is_intel_enabled("Cloak"));
    REQUIRE_FALSE(unit->economy().maintenance_active);
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
