// An aircraft's climb: Moho's CUnitMotion::CalcMoveAir and ComputeAirControl
// (faf-re CUnitMotion.cpp) under its blueprint's KLift, KLiftDamping and
// LiftFactor.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "blueprints/blueprint_store.hpp"
#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "lua/sim_bindings.hpp"
#include "map/heightmap.hpp"
#include "map/terrain.hpp"
#include "sim/entity_registry.hpp"
#include "sim/flight_math.hpp"
#include "sim/manipulator.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/unit_command.hpp"

extern "C" {
#include <lua.h>
}

#include <memory>
#include <string>
#include <vector>

using Catch::Approx;
using osc::f32;
using osc::u32;
using osc::sim::Unit;

namespace {

struct World {
    osc::lua::LuaState state;
    osc::blueprints::BlueprintStore store{state.raw()};
    osc::sim::SimState sim{state.raw(), &store};

    World() {
        osc::sim::GameSetup game;
        game.scenario = "/maps/test/test_scenario.lua";
        game.seed = 3;
        osc::lua::register_moho_bindings(state, sim);
        osc::lua::register_sim_bindings(state, sim);
        std::vector<osc::u16> heights(static_cast<size_t>(129) * 129, 1280);
        osc::map::Heightmap hm(128, 128, 1.0f / 128.0f, std::move(heights));
        sim.set_terrain(std::make_unique<osc::map::Terrain>(std::move(hm), 0.0f, false));
        sim.build_pathfinding_grid();
        sim.add_army("ARMY_1", "ARMY_1");
        sim.set_game_setup(game);
        lua_State* L = state.raw();
        for (const char* bp : {
                 "{BlueprintId = 'gunship', Categories = {'AIR', 'MOBILE'},"
                 " Defense = {MaxHealth = 100}, SizeX = 2, SizeZ = 2,"
                 " Air = {CanFly = true, MaxAirspeed = 10, KLift = 3, KLiftDamping = 2.5,"
                 " LiftFactor = 7},"
                 " Physics = {MotionType = 'RULEUMT_Air', Elevation = 10}}",
                 "{BlueprintId = 'fighter', Categories = {'AIR', 'MOBILE'},"
                 " Defense = {MaxHealth = 100}, SizeX = 2, SizeZ = 2,"
                 " Air = {CanFly = true, Winged = true, MaxAirspeed = 10, KLift = 3,"
                 " KLiftDamping = 2.5, LiftFactor = 7},"
                 " Physics = {MotionType = 'RULEUMT_Air', Elevation = 10}}",
             }) {
            REQUIRE(state.do_string(std::string("return ") + bp).ok());
            store.register_blueprint(L, osc::blueprints::BlueprintType::Unit, lua_gettop(L));
            lua_pop(L, 1);
        }
        store.expose_to_lua(L);
        REQUIRE(state
                    .do_string("Plain = setmetatable({}, {__index = moho.unit_methods})"
                               " Plain.__index = Plain")
                    .ok());
        lua_pushstring(L, "__osc_unit_script_classes");
        lua_newtable(L);
        for (const char* id : {"gunship", "fighter"}) {
            lua_pushstring(L, id);
            lua_getglobal(L, "Plain");
            lua_rawset(L, -3);
        }
        lua_rawset(L, LUA_REGISTRYINDEX);
        lua_settop(L, 0);
    }

    Unit& make(const char* bp) {
        REQUIRE(
            state.do_string("made = CreateUnit('" + std::string(bp) + "', 1, 20, 10, 64)").ok());
        Unit* found = nullptr;
        sim.entity_registry().for_each_unit([&](osc::sim::Entity& e) {
            auto& u = static_cast<Unit&>(e);
            if (u.blueprint_id() == bp) {
                found = &u;
            }
        });
        REQUIRE(found);
        lua_settop(state.raw(), 0);
        return *found;
    }

    std::vector<f32> rises_from(Unit& u, f32 height, int ticks) {
        u.set_position({u.position().x, 10.0f + height, u.position().z});
        u.set_current_altitude(height);
        osc::sim::UnitCommand cmd;
        cmd.type = osc::sim::CommandType::Move;
        cmd.target_pos = {110.0f, 20.0f, 64.0f};
        u.push_command(cmd, true);
        std::vector<f32> rises;
        f32 y = u.position().y;
        for (int t = 0; t < 40 && static_cast<int>(rises.size()) < ticks; ++t) {
            sim.tick();
            if (u.position().y != y || !rises.empty()) {
                rises.push_back(u.position().y - y);
            }
            y = u.position().y;
        }
        return rises;
    }
};

} // namespace

TEST_CASE("An aircraft climbs under KLift, against KLiftDamping", "[air_lift]") {
    World w;
    Unit& gunship = w.make("gunship");
    const std::vector<f32> rises = w.rises_from(gunship, 2.0f, 2);
    REQUIRE(rises.size() == 2);
    CHECK(rises[0] == Approx(0.12f).margin(1e-4));
    CHECK(rises[1] == Approx(0.3282f).margin(1e-4));
}

TEST_CASE("A winged aircraft climbs by its wings' lift, LiftFactor x (up - 0.5) at most",
          "[air_lift]") {
    World w;
    Unit& fighter = w.make("fighter");
    const std::vector<f32> rises = w.rises_from(fighter, 2.0f, 1);
    REQUIRE(rises.size() == 1);
    CHECK(rises[0] == Approx(0.0525f).margin(1e-4));
}

TEST_CASE("CalcWingedLift: tilted past 60 degrees the wings give none", "[air_lift]") {
    CHECK(osc::sim::winged_lift(8.0f, 1.0f, 7.0f, 5.0f, 10.0f) == Approx(3.5f));
    CHECK(osc::sim::winged_lift(-4.0f, 1.0f, 7.0f, 5.0f, 10.0f) == Approx(-4.0f));
    CHECK(osc::sim::winged_lift(8.0f, 0.3f, 7.0f, 5.0f, 10.0f) == Approx(-1.4f));
    CHECK(osc::sim::winged_lift(8.0f, 0.3f, 7.0f, 5.0f, 2.0f) == Approx(3.0f));
}

TEST_CASE("A transport's lift is KLift over its load", "[air_lift]") {
    osc::sim::EntityRegistry registry;
    auto make = [&](f32 size) {
        auto u = std::make_unique<Unit>();
        u->set_size_xz(size, size);
        u->set_size_y(size);
        return static_cast<Unit*>(registry.find(registry.register_entity(std::move(u))));
    };
    Unit* transport = make(2.0f);
    Unit* tank = make(1.0f);
    Unit* bot = make(1.0f);
    CHECK(transport->transport_load_factor(registry) == Approx(1.0f));
    transport->add_cargo(tank->entity_id());
    transport->add_cargo(bot->entity_id());
    CHECK(transport->transport_load_factor(registry) == Approx(1.25f));

    const osc::sim::LiftStep light = osc::sim::lift_step(0.0f, 8.0f, 3.0f, 2.5f, 1.0f, 0.1f);
    const osc::sim::LiftStep laden = osc::sim::lift_step(0.0f, 8.0f, 3.0f, 2.5f, 2.0f, 0.1f);
    CHECK(laden.velocity == Approx(light.velocity * 0.5f));
}

TEST_CASE("The ground it flies over follows the highest ground ahead, by LiftFactor a second",
          "[air_lift]") {
    std::vector<osc::u16> heights(static_cast<size_t>(65) * 65, 0);
    heights[static_cast<size_t>(10) * 65 + 13] = 50 * 128;
    const osc::map::Heightmap hm(64, 64, 1.0f / 128.0f, std::move(heights));
    CHECK(hm.look_ahead_max(10.4f, 10.0f, 0.5f) == Approx(0.0f));
    CHECK(hm.look_ahead_max(10.4f, 10.0f, 2.0f) == Approx(0.0f));
    CHECK(hm.look_ahead_max(10.4f, 10.0f, 4.0f) == Approx(0.0f));
    CHECK(hm.look_ahead_max(10.4f, 10.0f, 8.0f) == Approx(50.0f));
    CHECK(hm.look_ahead_max(2.0f, 2.0f, 40.0f) == Approx(50.0f));
    CHECK(hm.look_ahead_max(40.0f, 40.0f, 40.0f) == Approx(0.0f));

    CHECK(osc::sim::next_lift_ground(10.0f, 50.0f, 7.0f, 0.1f) == Approx(10.7f));
    CHECK(osc::sim::next_lift_ground(10.0f, 10.2f, 7.0f, 0.1f) == Approx(10.2f));
    CHECK(osc::sim::next_lift_ground(10.0f, 0.0f, 7.0f, 0.1f) == Approx(9.65f));
    CHECK(osc::sim::rising_ground_slowdown(0.0f, 10.0f) == Approx(1.0f));
    CHECK(osc::sim::rising_ground_slowdown(5.0f, 10.0f) == Approx(0.25f));
    CHECK(osc::sim::rising_ground_slowdown(20.0f, 10.0f) == Approx(0.04f));
}
