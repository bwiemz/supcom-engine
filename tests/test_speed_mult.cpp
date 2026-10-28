#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "blueprints/blueprint_store.hpp"
#include "blueprints/footprint.hpp"
#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "lua/sim_bindings.hpp"
#include "map/heightmap.hpp"
#include "map/terrain.hpp"
#include "sim/game_setup.hpp"
#include "sim/manipulator.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/unit_command.hpp"

extern "C" {
#include <lua.h>
}

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

using osc::f32;
using osc::sim::CommandType;
using osc::sim::SimState;
using osc::sim::Unit;
using osc::sim::UnitCommand;
namespace oc = osc::blueprints::occupancy;

namespace {

struct Plain {
    osc::lua::LuaState state;
    osc::blueprints::BlueprintStore store{state.raw()};
    SimState sim{state.raw(), &store};

    explicit Plain(bool moho) {
        osc::sim::GameSetup game;
        game.scenario = "/maps/test/test_scenario.lua";
        game.seed = 3;
        osc::blueprints::NamedFootprint land;
        land.name = "Land1x1";
        land.size_x = 1;
        land.size_z = 1;
        land.caps = oc::kLand;
        land.max_water_depth = 0.05f;
        land.max_slope = 0.75f;
        store.add_footprint_class(land);
        osc::lua::register_moho_bindings(state, sim);
        osc::lua::register_sim_bindings(state, sim);
        std::vector<osc::u16> heights(129 * 129, 1280);
        osc::map::Heightmap hm(128, 128, 1.0f / 128.0f, std::move(heights));
        sim.set_terrain(std::make_unique<osc::map::Terrain>(std::move(hm), 0.0f, false));
        sim.build_pathfinding_grid();
        sim.add_army("ARMY_1", "ARMY_1");
        sim.set_game_setup(game);
        sim.set_moho_pathing(moho);
        lua_State* L = state.raw();
        REQUIRE(state
                    .do_string("return {BlueprintId = 'tank', Categories = {'LAND', 'MOBILE'},"
                               " Defense = {MaxHealth = 100}, Footprint = {SizeX = 1, SizeZ = 1},"
                               " SizeX = 0.8, SizeZ = 1, Physics = {MotionType = 'RULEUMT_Land',"
                               " MaxSpeed = 5, MaxAcceleration = 50, MaxBrake = 50,"
                               " TurnRate = 720}}")
                    .ok());
        store.register_blueprint(L, osc::blueprints::BlueprintType::Unit, lua_gettop(L));
        lua_pop(L, 1);
        store.expose_to_lua(L);
        REQUIRE(state
                    .do_string("Tank = setmetatable({}, {__index = moho.unit_methods})"
                               " Tank.__index = Tank")
                    .ok());
        lua_pushstring(L, "__osc_unit_script_classes");
        lua_newtable(L);
        lua_pushstring(L, "tank");
        lua_getglobal(L, "Tank");
        lua_rawset(L, -3);
        lua_rawset(L, LUA_REGISTRYINDEX);
        lua_settop(L, 0);
    }

    Unit* make(f32 x, f32 z) {
        REQUIRE(state
                    .do_string("CreateUnit('tank', 1, " + std::to_string(x) + ", 10, " +
                               std::to_string(z) + ")")
                    .ok());
        Unit* found = nullptr;
        sim.entity_registry().for_each_unit([&](osc::sim::Entity& e) {
            if (e.position().x == x && e.position().z == z) {
                found = static_cast<Unit*>(&e);
            }
        });
        REQUIRE(found);
        return found;
    }
};

} // namespace

TEST_CASE("A ground unit's speed multiplier under 1 counts twice, as Moho's", "[speed-mult]") {
    const bool moho = GENERATE(false, true);
    const f32 mult = GENERATE(1.0f, 0.5f, 0.25f, 2.0f);
    CAPTURE(moho, mult);
    Plain w(moho);
    Unit* u = w.make(10.5f, 64.5f);
    u->set_speed_mult(mult);
    UnitCommand move;
    move.type = CommandType::Move;
    move.target_pos = {120.5f, 0.0f, 64.5f};
    u->push_command(move, true);
    for (int t = 0; t < 20; ++t) {
        w.sim.tick();
    }
    const f32 x0 = u->position().x;
    for (int t = 0; t < 10; ++t) {
        w.sim.tick();
    }
    const f32 per_tick = (u->position().x - x0) / 10.0f;
    CHECK(std::abs(per_tick - std::min(5.0f, 5.0f * mult) * mult * 0.1f) < 0.01f);
    CHECK(u->navigator().moho_active() == moho);
}
