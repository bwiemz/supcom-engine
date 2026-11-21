// A factory's skirt and the moves its units take from it, as Moho's: a
// factory reserves the cells under its skirt (Unit's constructor), and a
// factory-issued move that is its unit's last order settles its goal off
// them (CUnitMoveTask's constructor, Unit::PrepareMove).

#include <catch2/catch_test_macros.hpp>

#include "blueprints/blueprint_store.hpp"
#include "blueprints/footprint.hpp"
#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "lua/sim_bindings.hpp"
#include "map/heightmap.hpp"
#include "map/terrain.hpp"
#include "sim/build_placement.hpp"
#include "sim/occupancy.hpp"
#include "sim/saved_game.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/unit_command.hpp"
#include "support/tick_until.hpp"

extern "C" {
#include <lua.h>
}

#include <cmath>
#include <memory>
#include <string>
#include <vector>

using osc::f32;
using osc::u32;
using osc::u8;
using osc::sim::OccupancyRect;
using osc::sim::Unit;
using osc::sim::UnitCommand;
using osc::test::tick_until;
namespace oc = osc::blueprints::occupancy;

namespace {

constexpr u32 kMap = 128;

/// A sim on a flat map, its land units pathing as Moho's: 'walker', an
/// engineer's size, 'builder', one that builds 'pgen' (a 2x2 footprint), and
/// 'yard', a land factory's (a 5x5 footprint, an 8x8 skirt 1.5 out from it).
struct World {
    osc::lua::LuaState state;
    osc::blueprints::BlueprintStore store{state.raw()};
    osc::sim::SimState sim{state.raw(), &store};

    World() {
        osc::sim::GameSetup game;
        game.scenario = "/maps/test/test_scenario.lua";
        game.seed = 3;
        osc::blueprints::NamedFootprint land;
        land.name = "Vehicle1x1";
        land.size_x = 1;
        land.size_z = 1;
        land.caps = oc::kLand;
        land.max_water_depth = 0.05f;
        land.max_slope = 0.75f;
        store.add_footprint_class(land);
        osc::lua::register_moho_bindings(state, sim);
        osc::lua::register_sim_bindings(state, sim);
        std::vector<osc::u16> heights(static_cast<size_t>(kMap + 1) * (kMap + 1), 1280);
        osc::map::Heightmap hm(kMap, kMap, 1.0f / 128.0f, std::move(heights));
        sim.set_terrain(std::make_unique<osc::map::Terrain>(std::move(hm), 0.0f, false));
        sim.build_pathfinding_grid();
        sim.add_army("ARMY_1", "ARMY_1");
        sim.set_game_setup(game);
        sim.set_moho_pathing(true);
        lua_State* L = state.raw();
        for (const char* bp : {
                 "{BlueprintId = 'walker', Categories = {'LAND', 'MOBILE'},"
                 " Defense = {MaxHealth = 100}, SizeX = 0.6, SizeY = 0.5, SizeZ = 0.6,"
                 " Footprint = {SizeX = 1, SizeZ = 1},"
                 " Physics = {MotionType = 'RULEUMT_Land', MaxSpeed = 2, MaxAcceleration = 2,"
                 "  MaxBrake = 2, TurnRate = 180, TurnRadius = 0}}",
                 "{BlueprintId = 'builder', Categories = {'LAND', 'MOBILE', 'ENGINEER'},"
                 " Defense = {MaxHealth = 100}, SizeX = 0.6, SizeY = 0.5, SizeZ = 0.6,"
                 " Footprint = {SizeX = 1, SizeZ = 1},"
                 " Economy = {BuildRate = 10, MaxBuildDistance = 5, BuildableCategory = {'pgen'}},"
                 " Physics = {MotionType = 'RULEUMT_Land', MaxSpeed = 2, MaxAcceleration = 2,"
                 "  MaxBrake = 2, TurnRate = 180, TurnRadius = 0}}",
                 "{BlueprintId = 'pgen', Categories = {'STRUCTURE'},"
                 " Defense = {MaxHealth = 100}, SizeX = 1.6, SizeY = 1, SizeZ = 1.6,"
                 " Footprint = {SizeX = 2, SizeZ = 2},"
                 " Economy = {BuildTime = 10, BuildCostEnergy = 1, BuildCostMass = 1},"
                 " Physics = {MotionType = 'RULEUMT_None'}}",
                 "{BlueprintId = 'yard', Categories = {'FACTORY', 'STRUCTURE'},"
                 " Defense = {MaxHealth = 100}, SizeX = 4.2, SizeY = 0.6, SizeZ = 4.4,"
                 " Footprint = {SizeX = 5, SizeZ = 5},"
                 " Physics = {MotionType = 'RULEUMT_None', SkirtOffsetX = -1.5,"
                 "  SkirtOffsetZ = -1.5, SkirtSizeX = 8, SkirtSizeZ = 8}}",
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
        for (const char* id : {"walker", "builder", "pgen", "yard"}) {
            lua_pushstring(L, id);
            lua_getglobal(L, "Plain");
            lua_rawset(L, -3);
        }
        lua_rawset(L, LUA_REGISTRYINDEX);
        lua_settop(L, 0);
    }

    Unit* make(const char* bp, f32 x, f32 z, const char* global) {
        REQUIRE(state
                    .do_string(std::string(global) + " = CreateUnit('" + bp + "', 1, " +
                               std::to_string(x) + ", 10, " + std::to_string(z) + ")")
                    .ok());
        Unit* found = nullptr;
        sim.entity_registry().for_each_unit([&](osc::sim::Entity& e) {
            auto& u = static_cast<Unit&>(e);
            if (u.blueprint_id() == bp && u.position().x == x && u.position().z == z) {
                found = &u;
            }
        });
        REQUIRE(found);
        lua_settop(state.raw(), 0);
        return found;
    }

    bool reserved(int x, int z) const {
        return sim.occupancy().reserved_any(OccupancyRect{x, z, x + 1, z + 1});
    }
};

UnitCommand move_to(f32 x, f32 z, bool from_factory) {
    UnitCommand cmd;
    cmd.type = osc::sim::CommandType::Move;
    cmd.target_pos = {x, 10.0f, z};
    cmd.from_factory = from_factory;
    return cmd;
}

/// Whether the unit stands within the cell whose corner is (x, z), or its
/// edge.
bool stands_at(const Unit& u, int x, int z) {
    const auto& at = u.position();
    return std::abs(at.x - (static_cast<f32>(x) + 0.5f)) <= 0.75f &&
           std::abs(at.z - (static_cast<f32>(z) + 0.5f)) <= 0.75f;
}

} // namespace

TEST_CASE("A factory reserves the cells under its skirt while it stands", "[factory_move]") {
    World w;
    w.make("yard", 35.5f, 71.5f, "a");
    // Its skirt, 31.5 to 39.5 and 67.5 to 75.5, out to whole cells.
    CHECK(w.reserved(31, 67));
    CHECK(w.reserved(39, 75));
    CHECK(w.reserved(35, 71));
    CHECK_FALSE(w.reserved(30, 71));
    CHECK_FALSE(w.reserved(40, 71));
    CHECK_FALSE(w.reserved(35, 76));

    // A second, whose skirt overlaps its column 39.
    w.make("yard", 43.5f, 71.5f, "b");
    SECTION("destroyed, the one it overlaps reserves its cells again") {
        REQUIRE(w.state.do_string("moho.entity_methods.Destroy(a)").ok());
        w.sim.tick();
        CHECK_FALSE(w.reserved(31, 67));
        CHECK_FALSE(w.reserved(38, 71));
        CHECK(w.reserved(39, 71));
        CHECK(w.reserved(47, 75));
    }
    SECTION("killed, it frees the overlap too") {
        REQUIRE(w.state.do_string("a:Kill()").ok());
        w.sim.tick();
        CHECK_FALSE(w.reserved(38, 71));
        CHECK_FALSE(w.reserved(39, 71));
        CHECK(w.reserved(40, 71));
    }
}

TEST_CASE("A factory-issued move that is its unit's last order goes to the nearest place off "
          "reserved ground, and reserves it",
          "[factory_move]") {
    World w;
    w.make("yard", 35.5f, 71.5f, "f");
    // The point, rounded to (32, 72), is on the skirt: the nearest of the
    // ring two cells out, off it, is the cell (30, 72).
    Unit& u = *w.make("walker", 32.98f, 71.52f, "u");

    SECTION("its only order") {
        u.push_command(move_to(32.21f, 71.71f, true), true);
        w.sim.tick();
        REQUIRE_FALSE(u.command_queue().empty());
        const UnitCommand& cmd = u.command_queue().front();
        CHECK(cmd.goal_prepared);
        CHECK(cmd.prepared_goal.x == 30.5f);
        CHECK(cmd.prepared_goal.z == 72.5f);
        CHECK(u.order_reservation().held);
        CHECK(u.order_reservation().reserved.x0 == 30);
        CHECK(u.order_reservation().reserved.z0 == 72);
        CHECK(w.reserved(30, 72));
        tick_until([&] { w.sim.tick(); }, "the move ends", 200,
                   [&] { return u.command_queue().empty(); });
        CHECK(stands_at(u, 30, 72));
        CHECK_FALSE(u.order_reservation().held);
        CHECK_FALSE(w.reserved(30, 72));
    }
    SECTION("with an order after it, it goes to the point") {
        u.push_command(move_to(32.21f, 71.71f, true), true);
        u.push_command(move_to(32.21f, 60.0f, false), false);
        w.sim.tick();
        REQUIRE_FALSE(u.command_queue().empty());
        CHECK_FALSE(u.command_queue().front().goal_prepared);
        CHECK_FALSE(u.order_reservation().held);
    }
}

TEST_CASE("A move not from a factory goes to its point, on a skirt or not", "[factory_move]") {
    World w;
    w.make("yard", 35.5f, 71.5f, "f");
    Unit& u = *w.make("walker", 20.5f, 71.5f, "u");
    u.push_command(move_to(32.21f, 71.71f, false), true);
    w.sim.tick();
    REQUIRE_FALSE(u.command_queue().empty());
    CHECK_FALSE(u.command_queue().front().goal_prepared);
    tick_until([&] { w.sim.tick(); }, "the move ends", 300,
               [&] { return u.command_queue().empty(); });
    CHECK(stands_at(u, 32, 71));
}

TEST_CASE("A factory-issued move's goal is no place another unit stands on", "[factory_move]") {
    World w;
    w.make("yard", 70.5f, 46.5f, "f");
    // The yard's initial rally, (70.5, 51.5), rounds to (70, 52), where the
    // unit before stands; the ring's row 50 is on the skirt, and (72, 52)
    // is the nearest left.
    w.make("walker", 70.79f, 52.19f, "a");
    Unit& b = *w.make("walker", 73.02f, 46.52f, "b");
    b.push_command(move_to(70.5f, 51.5f, true), true);
    w.sim.tick();
    REQUIRE_FALSE(b.command_queue().empty());
    const UnitCommand& cmd = b.command_queue().front();
    CHECK(cmd.goal_prepared);
    CHECK(cmd.prepared_goal.x == 72.5f);
    CHECK(cmd.prepared_goal.z == 52.5f);
}

TEST_CASE("A structure next to a factory is placed and built as before, from off the factory's "
          "skirt",
          "[factory_move]") {
    World w;
    w.make("yard", 35.5f, 71.5f, "f");
    Unit& b = *w.make("builder", 36.5f, 62.5f, "b");
    const osc::sim::StructurePlacement placement(w.sim, 0, [&](const std::string& bp) {
        return osc::lua::structure_rules(w.state.raw(), w.sim, bp);
    });
    CHECK(placement.can_build("pgen", 41.0f, 71.0f));
    CHECK_FALSE(placement.can_build("pgen", 40.0f, 71.0f));
    REQUIRE(w.state.do_string("IssueBuildMobile({b}, {41, 0, 71}, 'pgen', {})").ok());
    const Unit* built = nullptr;
    tick_until([&] { w.sim.tick(); }, "the build starts", 300,
               [&] {
                   w.sim.entity_registry().for_each_unit([&](osc::sim::Entity& e) {
                       if (static_cast<Unit&>(e).blueprint_id() == "pgen") {
                           built = &static_cast<Unit&>(e);
                       }
                   });
                   return built != nullptr;
               });
    CHECK(built->position().x == 41.0f);
    CHECK(built->position().z == 71.0f);
    // The nearest place a cell clear of the site, (37, 67), is on the skirt.
    CHECK(stands_at(b, 40, 67));
}
