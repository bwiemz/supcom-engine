#include <catch2/catch_test_macros.hpp>
#include "core/front_end_data.hpp"
#include "sim/army_brain.hpp"
#include "sim/entity_registry.hpp"
#include "sim/manipulator.hpp"
#include "sim/shield.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "lua/session_manager.hpp"
#include "ui/ui_control.hpp"
#include "vfs/virtual_file_system.hpp"

extern "C" {
#include <lua.h>
}

#include <memory>
#include <string_view>

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
        intel_unit->enable_intel(std::string(intel));
    }
    intel_unit->economy().maintenance_active = true;
    intel_unit->economy().energy_maintenance_override = 100.0;
    auto unit_id = registry.register_entity(std::move(intel_unit));

    brain.update_economy(registry, 1.0);

    auto* unit = static_cast<osc::sim::Unit*>(registry.find(unit_id));
    REQUIRE(unit != nullptr);
    REQUIRE(brain.energy_efficiency() == 0.0);
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
    REQUIRE(brain.energy_efficiency() == 0.0);
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

    front_end_data.clear(L);
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

TEST_CASE("Front-end fallback bindings preserve real UI globals", "[frontend][lua]") {
    osc::lua::LuaState lua;
    osc::sim::SimState sim(lua.raw(), nullptr);
    osc::ui::UIControlRegistry ui_registry;

    osc::lua::register_moho_bindings(lua, sim);
    osc::lua::register_ui_bindings(lua, ui_registry);

    lua_State* L = lua.raw();
    lua_pushstring(L, "__osc_focus_army");
    lua_pushnumber(L, 1);
    lua_rawset(L, LUA_REGISTRYINDEX);

    osc::lua::register_front_end_fallback_bindings(lua);

    auto result = lua.do_string("focus_army = GetFocusArmy()");
    REQUIRE(result.ok());

    lua_getglobal(L, "focus_army");
    double focus_army = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : 0.0;
    lua_pop(L, 1);

    REQUIRE(focus_army == 2.0);
}

TEST_CASE("UI SetFocusArmy updates focus army registry state", "[frontend][lua]") {
    osc::lua::LuaState lua;
    osc::sim::SimState sim(lua.raw(), nullptr);
    osc::ui::UIControlRegistry ui_registry;

    osc::lua::register_moho_bindings(lua, sim);
    osc::lua::register_ui_bindings(lua, ui_registry);
    osc::lua::register_front_end_fallback_bindings(lua);

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
    options.restricted_categories = {"NUKE", "EXPERIMENTAL"};
    mgr.set_game_options(options);

    auto result = mgr.start_session(lua, osc::vfs::VirtualFileSystem{}, sim, meta);
    REQUIRE(result.ok());

    for (int army = 0; army < 2; ++army) {
        auto* brain = sim.get_army(army);
        REQUIRE(brain != nullptr);
        CHECK(brain->unit_cap() == 42);
        CHECK(brain->is_build_restricted("NUKE"));
        CHECK(brain->is_build_restricted("EXPERIMENTAL"));
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

    for (int i = 0; i < 60; ++i) {
        sim.tick();
    }

    auto* brain = sim.get_army(0);
    REQUIRE(brain != nullptr);
    CHECK(brain->state() == osc::sim::BrainState::InProgress);
    CHECK(sim.player_result() == 0);
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
