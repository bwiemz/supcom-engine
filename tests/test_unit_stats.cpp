#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "blueprints/blueprint_store.hpp"
#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "lua/sim_bindings.hpp"
#include "map/heightmap.hpp"
#include "map/terrain.hpp"
#include "sim/army_brain.hpp"
#include "sim/sim_snapshot.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <memory>
#include <string>
#include <vector>

using osc::f32;
using osc::f64;
using osc::sim::Unit;

namespace {

struct StatsWorld {
    osc::lua::LuaState state;
    osc::blueprints::BlueprintStore store{state.raw()};
    osc::sim::SimState sim{state.raw(), &store};
    f32 next_x = 20.0f;

    StatsWorld() {
        osc::sim::GameSetup game;
        game.scenario = "/maps/test/test_scenario.lua";
        game.seed = 3;
        osc::lua::register_moho_bindings(state, sim);
        osc::lua::register_sim_bindings(state, sim);
        std::vector<osc::u16> heights(static_cast<size_t>(257) * 257, 1280);
        osc::map::Heightmap hm(256, 256, 1.0f / 128.0f, std::move(heights));
        sim.set_terrain(std::make_unique<osc::map::Terrain>(std::move(hm), 0.0f, false));
        sim.build_pathfinding_grid();
        sim.add_army("ARMY_1", "ARMY_1");
        sim.add_army("ARMY_2", "ARMY_2");
        sim.set_fog_of_war("none");
        sim.set_victory_condition("sandbox");
        sim.set_game_setup(game);
        lua_State* L = state.raw();
        for (const char* bp : {
                 "{BlueprintId = 'tank', Categories = {'LAND', 'MOBILE'},"
                 " Defense = {MaxHealth = 100}, Footprint = {SizeX = 1, SizeZ = 1},"
                 " Economy = {BuildCostMass = 56, BuildCostEnergy = 266, BuildTime = 280},"
                 " Physics = {MotionType = 'RULEUMT_Land'}}",
                 "{BlueprintId = 'eng', Categories = {'LAND', 'MOBILE', 'ENGINEER'},"
                 " Defense = {MaxHealth = 100}, Footprint = {SizeX = 1, SizeZ = 1},"
                 " Economy = {BuildCostMass = 52, BuildCostEnergy = 260, BuildRate = 5},"
                 " Physics = {MotionType = 'RULEUMT_Land'}}",
                 "{BlueprintId = 'pgen', Categories = {'STRUCTURE'},"
                 " Defense = {MaxHealth = 100}, Footprint = {SizeX = 2, SizeZ = 2},"
                 " Economy = {BuildCostMass = 75, BuildCostEnergy = 750, BuildTime = 125},"
                 " Physics = {MotionType = 'RULEUMT_None'}}",
                 "{BlueprintId = 'beacon', Categories = {'STRUCTURE'}, General = {CapCost = 0},"
                 " Defense = {MaxHealth = 100}, Footprint = {SizeX = 1, SizeZ = 1},"
                 " Economy = {BuildCostMass = 5, BuildCostEnergy = 50, BuildTime = 50},"
                 " Physics = {MotionType = 'RULEUMT_None'}}",
             }) {
            REQUIRE(state.do_string(std::string("return ") + bp).ok());
            store.register_blueprint(L, osc::blueprints::BlueprintType::Unit, lua_gettop(L));
            lua_pop(L, 1);
        }
        store.expose_to_lua(L);
        REQUIRE(run("Plain = setmetatable({}, {__index = moho.unit_methods})"
                    " Plain.__index = Plain"));
        lua_pushstring(L, "__osc_unit_script_classes");
        lua_newtable(L);
        for (const char* id : {"tank", "eng", "pgen", "beacon"}) {
            lua_pushstring(L, id);
            lua_getglobal(L, "Plain");
            lua_rawset(L, -3);
        }
        lua_rawset(L, LUA_REGISTRYINDEX);
        lua_newtable(L);
        lua_pushstring(L, "_c_object");
        lua_pushlightuserdata(L, sim.get_army(0));
        lua_rawset(L, -3);
        lua_pushvalue(L, -1);
        sim.get_army(0)->set_lua_table_ref(luaL_ref(L, LUA_REGISTRYINDEX));
        lua_setglobal(L, "brain");
        REQUIRE(run("setmetatable(brain, {__index = moho.aibrain_methods})"));
        lua_settop(L, 0);
    }

    bool run(const std::string& code) {
        auto r = state.do_string(code);
        UNSCOPED_INFO((r.ok() ? std::string() : r.error().message));
        return r.ok();
    }

    Unit& unit_of(const char* global) {
        lua_State* L = state.raw();
        lua_getglobal(L, global);
        lua_pushstring(L, "_c_object");
        lua_rawget(L, -2);
        auto* u = static_cast<Unit*>(lua_touserdata(L, -1));
        lua_settop(L, 0);
        REQUIRE(u);
        return *u;
    }

    Unit& make(const char* global, const char* bp, int army) {
        next_x += 8.0f;
        REQUIRE(run(std::string(global) + " = CreateUnit('" + bp + "', " + std::to_string(army) +
                    ", " + std::to_string(next_x) + ", 10, 60)"));
        return unit_of(global);
    }

    Unit& start(const char* global, const char* bp, int army) {
        next_x += 8.0f;
        lua_State* L = state.raw();
        lua_pushstring(L, "__osc_create_building_unit");
        lua_rawget(L, LUA_REGISTRYINDEX);
        lua_pushstring(L, bp);
        lua_pushnumber(L, army);
        lua_pushnumber(L, next_x);
        lua_pushnil(L);
        lua_pushnumber(L, 60);
        REQUIRE(lua_pcall(L, 5, 2, 0) == 0);
        lua_setglobal(L, global);
        lua_settop(L, 0);
        return unit_of(global);
    }

    void finish(Unit& built) {
        Unit& eng = make("builder", "eng", built.army() + 1);
        eng.set_build_target_id(built.entity_id());
        eng.finish_build(sim.entity_registry(), state.raw(), true);
    }

    void destroy(Unit& u) {
        sim.entity_registry().destroy(u);
        sim.run_deletion_queue();
    }

    f64 stat(int army, const char* name) { return sim.get_army(army - 1)->get_stat(name); }

    f64 lane(int army, const char* name, const char* bp) {
        const auto* lanes = sim.get_army(army - 1)->blueprint_stats(name);
        if (!lanes) {
            return 0.0;
        }
        const auto it = lanes->find(bp);
        return it == lanes->end() ? 0.0 : it->second;
    }
};

void field_units(StatsWorld& w) {
    w.make("t1", "tank", 1);
    w.make("t2", "tank", 1);
    w.make("p1", "pgen", 1);
    w.start("t3", "tank", 1);
    REQUIRE(w.run("brain:SetArmyStat('OSC_Plain', 5) brain:GetArmyStat('OSC_Read', 0)"));
}

} // namespace

TEST_CASE("A unit with a cap cost is active, by blueprint, from when it is ready until it is "
          "deleted",
          "[army][stats]") {
    struct Row {
        const char* how;
        const char* bp;
        f64 active;
        f64 history;
        f64 being_built_left;
        f64 mass_built;
        f64 energy_built;
    };
    const Row row =
        GENERATE(Row{"made", "tank", 1, 1, 0, 0, 0}, Row{"made", "beacon", 0, 0, 0, 0, 0},
                 Row{"built", "tank", 1, 1, 0, 56, 266}, Row{"built", "beacon", 0, 0, 1, 0, 0},
                 Row{"cancelled", "tank", 0, 0, 0, 0, 0});
    CAPTURE(row.how, row.bp);
    StatsWorld w;
    Unit* u = nullptr;
    if (std::string(row.how) == "made") {
        u = &w.make("u", row.bp, 1);
    } else {
        u = &w.start("u", row.bp, 1);
        CHECK(w.lane(1, "Units_BeingBuilt", row.bp) == 1);
        CHECK(w.lane(1, "Units_Active", row.bp) == 0);
        if (std::string(row.how) == "built") {
            w.finish(*u);
        }
    }
    CHECK(w.lane(1, "Units_Active", row.bp) == row.active);
    CHECK(w.stat(1, "Units_Active") ==
          w.lane(1, "Units_Active", row.bp) + w.lane(1, "Units_Active", "eng"));
    CHECK(w.lane(1, "Units_History", row.bp) == row.history);
    CHECK(w.stat(1, "Units_MassValue_Built") == row.mass_built);
    CHECK(w.stat(1, "Units_EnergyValue_Built") == row.energy_built);

    w.destroy(*u);
    CHECK(w.lane(1, "Units_Active", row.bp) == 0);
    CHECK(w.lane(1, "Units_BeingBuilt", row.bp) == row.being_built_left);
    CHECK(w.lane(1, "Units_History", row.bp) == row.history);
}

TEST_CASE("Joined builders count one construction completion after its callback",
          "[army][stats][construction-review]") {
    const int callback = GENERATE(0, 1, 2); // keep, destroy, destroy and replace
    StatsWorld w;
    Unit& frame = w.start("u", "pgen", 1);
    Unit& first = w.make("first", "eng", 1);
    Unit& second = w.make("second", "eng", 1);
    first.set_build_target_id(frame.entity_id());
    second.set_build_target_id(frame.entity_id());
    if (callback != 0) {
        REQUIRE(w.run(callback == 1
                          ? "function u:OnStopBeingBuilt() "
                            "moho.entity_methods.Destroy(self) completed_callback = true end"
                          : "function u:OnStopBeingBuilt() moho.entity_methods.Destroy(self) "
                            "replacement = CreateUnit('pgen', 1, 80, 10, 60) "
                            "completed_callback = true end"));
    }
    first.finish_build(w.sim.entity_registry(), w.state.raw(), true);
    second.finish_build(w.sim.entity_registry(), w.state.raw(), true);
    if (callback != 0) REQUIRE(w.run("assert(completed_callback)"));
    w.sim.run_deletion_queue();
    const f64 active = callback == 1 ? 0.0 : 1.0;
    const f64 history = callback == 2 ? 2.0 : 1.0;
    CHECK(w.lane(1, "Units_Active", "pgen") == active);
    CHECK(w.stat(1, "Units_Active") == active + 2.0);
    CHECK(w.lane(1, "Units_History", "pgen") == history);
    CHECK(w.stat(1, "Units_History") == history + 2.0);
    CHECK(w.lane(1, "Units_BeingBuilt", "pgen") == 0.0);
    CHECK(w.stat(1, "Units_MassValue_Built") == 75.0);
    CHECK(w.stat(1, "Units_EnergyValue_Built") == 750.0);
}

TEST_CASE("A killed unit is worth its blueprint's cost, the part built while it is being built",
          "[army][stats]") {
    const f32 built = GENERATE(1.0f, 0.4f);
    CAPTURE(built);
    StatsWorld w;
    w.make("attacker", "tank", 1);
    if (built == 1.0f) {
        w.make("victim", "tank", 2);
    } else {
        Unit& v = w.start("victim", "tank", 2);
        v.set_fraction_complete(built);
        v.set_health(50.0f);
    }
    REQUIRE(w.run("Damage(attacker, victim, 1, 'Normal') victim:Kill(attacker, 'Normal', 0)"));
    CHECK(w.stat(2, "Units_MassValue_Lost") == Catch::Approx(56.0 * built));
    CHECK(w.stat(2, "Units_EnergyValue_Lost") == Catch::Approx(266.0 * built));
    CHECK(w.stat(1, "Enemies_MassValue_Destroyed") == Catch::Approx(56.0 * built));
    CHECK(w.stat(1, "Enemies_EnergyValue_Destroyed") == Catch::Approx(266.0 * built));
}

TEST_CASE("A stats trigger with a category holds the stat summed over the category's blueprints",
          "[army][stats]") {
    struct Row {
        const char* stat;
        const char* op;
        double value;
        const char* category;
        bool fires;
    };
    const Row row = GENERATE(
        Row{"Units_Active", "GreaterThanOrEqual", 2, "categories.tank", true},
        Row{"Units_Active", "GreaterThanOrEqual", 3, "categories.tank", false},
        Row{"Units_Active", "GreaterThanOrEqual", 3, "categories.tank + categories.pgen", true},
        Row{"Units_Active", "LessThanOrEqual", 0, "categories.eng", true},
        Row{"Units_Active", "LessThanOrEqual", 0, "categories.STRUCTURE", false},
        Row{"Units_BeingBuilt", "GreaterThanOrEqual", 1, "categories.tank", true},
        Row{"Units_BeingBuilt", "GreaterThanOrEqual", 1, "nil", false},
        Row{"Units_Active", "GreaterThanOrEqual", 3, "nil", true},
        Row{"OSC_Plain", "GreaterThan", 0, "categories.ALLUNITS", false},
        Row{"OSC_Plain", "GreaterThan", 0, "nil", true},
        Row{"OSC_Read", "LessThan", 1, "nil", true}, Row{"OSC_None", "LessThan", 1, "nil", false});
    CAPTURE(row.stat, row.op, row.value, row.category);
    StatsWorld w;
    field_units(w);
    REQUIRE(w.run(std::string("brain:SetArmyStatsTrigger('") + row.stat + "', 'T', '" + row.op +
                  "', " + std::to_string(row.value) + ", " + row.category + ")"));
    const std::vector<std::string> fired = w.sim.get_army(0)->take_fired_triggers();
    CHECK(fired == (row.fires ? std::vector<std::string>{"T"} : std::vector<std::string>{}));
}

TEST_CASE("GetArmyStat makes a stat the army doesn't have at 0, whatever its default",
          "[army][stats]") {
    const int fallback = GENERATE(0, 5);
    CAPTURE(fallback);
    StatsWorld w;
    REQUIRE(w.run("got = brain:GetArmyStat('OSC_New', " + std::to_string(fallback) +
                  ").Value brain:SetArmyStatsTrigger('OSC_New', 'T', 'LessThan', 1)"));
    lua_State* L = w.state.raw();
    lua_getglobal(L, "got");
    CHECK(lua_tonumber(L, -1) == 0);
    lua_pop(L, 1);
    CHECK(w.sim.get_army(0)->take_fired_triggers() == std::vector<std::string>{"T"});
}

TEST_CASE("GetBlueprintStat sums a stat's per-blueprint counts over a category, and 0 for "
          "a stat without them",
          "[army][stats]") {
    struct Row {
        const char* stat;
        const char* category;
        double value;
    };
    const Row row = GENERATE(
        Row{"Units_Active", "categories.tank", 2}, Row{"Units_Active", "categories.ALLUNITS", 3},
        Row{"Units_BeingBuilt", "categories.tank", 1}, Row{"Units_History", "categories.MOBILE", 2},
        Row{"OSC_Plain", "categories.ALLUNITS", 0}, Row{"OSC_None", "categories.ALLUNITS", 0});
    CAPTURE(row.stat, row.category);
    StatsWorld w;
    field_units(w);
    REQUIRE(w.run(std::string("got = brain:GetBlueprintStat('") + row.stat + "', " + row.category +
                  ")"));
    lua_State* L = w.state.raw();
    lua_getglobal(L, "got");
    CHECK(lua_tonumber(L, -1) == row.value);
    lua_pop(L, 1);
}

TEST_CASE("A stats trigger keeps its category's blueprints through a saved game",
          "[army][stats][save]") {
    StatsWorld a;
    field_units(a);
    REQUIRE(a.run("brain:SetArmyStatsTrigger('Units_Active', 'Two', 'GreaterThanOrEqual', 3,"
                  " categories.tank)"));
    std::vector<osc::u8> bytes;
    REQUIRE(osc::sim::save_snapshot(a.sim, bytes).empty());

    StatsWorld b;
    const std::string err = osc::sim::load_snapshot(b.sim, bytes);
    INFO(err);
    REQUIRE(err.empty());
    CHECK(b.sim.get_army(0)->take_fired_triggers().empty());
    REQUIRE(b.run("t4 = CreateUnit('tank', 1, 200, 10, 60)"));
    CHECK(b.sim.get_army(0)->take_fired_triggers() == std::vector<std::string>{"Two"});
}
