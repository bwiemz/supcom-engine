#include <catch2/catch_test_macros.hpp>

#include "blueprints/blueprint_store.hpp"
#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "lua/sim_bindings.hpp"
#include "sim/sim_state.hpp"

extern "C" {
#include <lua.h>
}

#include <string>
#include <utility>
#include <vector>

namespace {

struct World {
    osc::lua::LuaState state;
    osc::blueprints::BlueprintStore store{state.raw()};
    osc::sim::SimState sim{state.raw(), &store};

    World() {
        osc::lua::register_moho_bindings(state, sim);
        osc::lua::register_sim_bindings(state, sim);
        sim.add_army("ARMY_1", "ARMY_1");
        sim.add_army("ARMY_2", "ARMY_2");
        lua_State* L = state.raw();
        REQUIRE(state
                    .do_string("return {BlueprintId = 'tank', Categories = {'LAND'},"
                               " Defense = {MaxHealth = 100}, Footprint = {SizeX = 1, SizeZ = 1},"
                               " Physics = {MotionType = 'RULEUMT_Land', MaxSpeed = 1}}")
                    .ok());
        store.register_blueprint(L, osc::blueprints::BlueprintType::Unit, lua_gettop(L));
        lua_pop(L, 1);
        store.expose_to_lua(L);
        REQUIRE(state
                    .do_string("Plain = setmetatable({}, {__index = moho.unit_methods})"
                               " Plain.__index = Plain")
                    .ok());
        lua_pushstring(L, "__osc_unit_script_classes");
        lua_newtable(L);
        lua_pushstring(L, "tank");
        lua_getglobal(L, "Plain");
        lua_rawset(L, -3);
        lua_rawset(L, LUA_REGISTRYINDEX);
    }

    /// The error `call(army)` raises, or "" when it runs.
    std::string error_of(const std::string& call, const std::string& army) {
        std::string code = call;
        code.replace(code.find('@'), 1, army);
        auto r = state.do_string(code);
        return r.ok() ? std::string() : r.error().message;
    }
};

bool says(const std::string& error, const std::string& text) {
    return error.find(text) != std::string::npos;
}

} // namespace

TEST_CASE("A function taking an army raises Moho's error for one that isn't there", "[army]") {
    const std::vector<std::string> calls = {
        "GetArmyBrain(@)",
        "ArmyIsCivilian(@)",
        "ArmyIsOutOfGame(@)",
        "ArmyGetHandicap(@)",
        "OkayToMessWithArmy(@)",
        "IsAlly(@, 1)",
        "IsAlly(1, @)",
        "IsEnemy(@, 1)",
        "IsNeutral(@, 1)",
        "GetArmyUnitCap(@)",
        "GetArmyUnitCostTotal(@)",
        "SetArmyUnitCap(@, 500)",
        "SetIgnoreArmyUnitCap(@, false)",
        "SetArmyShowScore(@, true)",
        "SetIgnorePlayableRect(@, false)",
        "SetArmyEconomy(@, 0, 0)",
        "SetArmyColor(@, 255, 0, 0)",
        "SetArmyColorIndex(@, 1)",
        "SetArmyFactionIndex(@, 1)",
        "SetArmyStart(@, 10, 10)",
        "GenerateArmyStart(@)",
        "SetAlliance(@, 1, 'Enemy')",
        "SetAllianceOneWay(@, 1, 'Enemy')",
        "AddBuildRestriction(@, categories.LAND)",
        "RemoveBuildRestriction(@, categories.LAND)",
        "ArmyInitializePrebuiltUnits(@)",
        "SetArmyOutOfGame(@)",
        "CreateInitialArmyUnit(@, 'tank')",
        "CreateUnitHPR('tank', @, 0, 0, 0, 0, 0, 0)",
    };
    const std::vector<std::pair<std::string, std::string>> bad = {
        {"'NOSUCH'", "Unknown army: NOSUCH"},
        {"'1'", "Unknown army: 1"},
        {"0", "Invalid army 0. (Use a 1-based index)"},
        {"-1", "Invalid army -1. (Use a 1-based index)"},
        {"3", "Invalid army 2"},
        {"4", "Invalid army 3"},
        {"nil", "Unexpected type for army object"},
        {"{}", "Unexpected type for army object"},
        {"true", "Unexpected type for army object"},
    };
    World w;
    for (const auto& call : calls) {
        for (const auto& [army, text] : bad) {
            INFO(call << " with " << army);
            CHECK(says(w.error_of(call, army), text));
        }
        INFO(call);
        CHECK(w.error_of(call, "'ARMY_2'").empty());
        CHECK(w.error_of(call, "1.7").empty());
    }
}

TEST_CASE("ChangeUnitArmy raises Moho's error for an army that isn't there", "[army]") {
    World w;
    REQUIRE(w.state.do_string("tank = CreateUnit('tank', 1, 0, 0, 0, 0, 0, 0, 1)").ok());
    CHECK(says(w.error_of("ChangeUnitArmy(tank, @)", "'NOSUCH'"), "Unknown army: NOSUCH"));
    CHECK(says(w.error_of("ChangeUnitArmy(tank, @)", "3"), "Invalid army 2"));
    CHECK(
        says(w.error_of("ChangeUnitArmy(tank, @)", "0"), "Invalid army 0. (Use a 1-based index)"));
    CHECK(says(w.error_of("ChangeUnitArmy(tank, @)", "nil"), "Unexpected type for army object"));
}

TEST_CASE("CreateUnit and CreateUnit2 take their army as a 1-based number", "[army]") {
    World w;
    for (const std::string call : {"CreateUnit('tank', @, 0, 0, 0, 0, 0, 0, 1)",
                                   "CreateUnit2('tank', @, 'Land', 0, 0, 0)"}) {
        INFO(call);
        CHECK(says(w.error_of(call, "'ARMY_1'"), "attempt to use as integer a string value"));
        CHECK(says(w.error_of(call, "nil"), "attempt to use as integer a nil value"));
        CHECK(says(w.error_of(call, "0"), "Invalid army index; must be >= 1 and < 2 but got 0."));
        CHECK(says(w.error_of(call, "3"), "Invalid army index; must be >= 1 and < 2 but got 3."));
        CHECK(w.error_of(call, "2").empty());
    }
}

TEST_CASE("A decal's or a splat's army reads as Moho's ARMY_IndexFromLuaState", "[army]") {
    World w;
    for (const std::string call :
         {"CreateDecal({0, 0, 0}, 0, 'a.dds', '', 'Albedo', 2, 2, 100, 10, @, 0)",
          "CreateSplat({0, 0, 0}, 0, 'a.dds', 2, 2, 100, 10, @, 0)"}) {
        INFO(call);
        CHECK(says(w.error_of(call, "'NOSUCH'"), "Unknown army: NOSUCH"));
        CHECK(says(w.error_of(call, "0"), "Invalid army 0, (Use a 1-based index)"));
        for (const std::string army : {"-1", "3", "nil", "{}", "1", "'ARMY_2'"}) {
            INFO(army);
            CHECK(w.error_of(call, army).empty());
        }
    }
}
