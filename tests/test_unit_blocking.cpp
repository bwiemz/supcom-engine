// Mobile units in a unit's way (roadmap item 4c-3): Moho's COGrid::
// UnitIsBlocked, SweptPathBlockedByUnit and func_IsSourceUnit, which the
// path search and navigator ask.

#include <catch2/catch_test_macros.hpp>

#include "blueprints/blueprint_store.hpp"
#include "blueprints/footprint.hpp"
#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "lua/sim_bindings.hpp"
#include "map/heightmap.hpp"
#include "map/terrain.hpp"
#include "sim/path_finder.hpp"
#include "sim/sim_state.hpp"
#include "sim/steering.hpp"
#include "sim/unit.hpp"
#include "sim/unit_blocking.hpp"
#include "sim/unit_command.hpp"

extern "C" {
#include <lua.h>
}

#include <cmath>
#include <memory>
#include <string>
#include <vector>

using osc::f32;
using osc::u32;
using osc::sim::OrientedBox;
using osc::sim::Unit;
using osc::sim::Vector3;
using osc::sim::path::Cell;

namespace {

OrientedBox axis_box(Vector3 c, f32 ex, f32 ey, f32 ez) {
    OrientedBox b;
    b.centre = c;
    b.axis[0] = {1, 0, 0};
    b.axis[1] = {0, 1, 0};
    b.axis[2] = {0, 0, 1};
    b.extent[0] = ex;
    b.extent[1] = ey;
    b.extent[2] = ez;
    return b;
}

/// A box turned `yaw` radians about Y.
OrientedBox turned_box(Vector3 c, f32 ex, f32 ey, f32 ez, f32 yaw) {
    OrientedBox b = axis_box(c, ex, ey, ez);
    b.axis[0] = {std::cos(yaw), 0, -std::sin(yaw)};
    b.axis[2] = {std::sin(yaw), 0, std::cos(yaw)};
    return b;
}

std::unique_ptr<osc::map::Terrain> flat_terrain(u32 size) {
    std::vector<osc::u16> heights(static_cast<size_t>(size + 1) * (size + 1), 1280);
    osc::map::Heightmap hm(size, size, 1.0f / 128.0f, std::move(heights));
    return std::make_unique<osc::map::Terrain>(std::move(hm), 0.0f, false);
}

/// A sim on a flat map with a 1x1 tank, a 2x2 one and a gunship, each with
/// a collision box from its sizes.
struct World {
    osc::lua::LuaState state;
    osc::blueprints::BlueprintStore store{state.raw()};
    osc::sim::SimState sim{state.raw(), &store};

    World() {
        osc::sim::GameSetup game;
        game.scenario = "/maps/test/test_scenario.lua";
        game.seed = 5;
        osc::lua::register_moho_bindings(state, sim);
        osc::lua::register_sim_bindings(state, sim);
        sim.set_terrain(flat_terrain(128));
        sim.build_pathfinding_grid();
        sim.add_army("ARMY_1", "ARMY_1");
        sim.add_army("ARMY_2", "ARMY_2");
        sim.set_game_setup(game);
        lua_State* L = state.raw();
        for (const char* bp : {
                 "{BlueprintId = 'tank', Categories = {'LAND', 'MOBILE'},"
                 " Defense = {MaxHealth = 100}, SizeX = 0.8, SizeY = 0.5, SizeZ = 0.8,"
                 " Footprint = {SizeX = 1, SizeZ = 1},"
                 " Physics = {MotionType = 'RULEUMT_Land', MaxSpeed = 4}}",
                 "{BlueprintId = 'bigtank', Categories = {'LAND', 'MOBILE'},"
                 " Defense = {MaxHealth = 100}, SizeX = 1.8, SizeY = 1, SizeZ = 1.8,"
                 " Footprint = {SizeX = 2, SizeZ = 2},"
                 " Physics = {MotionType = 'RULEUMT_Land', MaxSpeed = 4}}",
                 "{BlueprintId = 'gunship', Categories = {'AIR', 'MOBILE'},"
                 " Defense = {MaxHealth = 100}, SizeX = 2, SizeY = 1, SizeZ = 2,"
                 " Footprint = {SizeX = 2, SizeZ = 2}, Air = {CanFly = true, MaxAirspeed = 10},"
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
        for (const char* id : {"tank", "bigtank", "gunship"}) {
            lua_pushstring(L, id);
            lua_getglobal(L, "Plain");
            lua_rawset(L, -3);
        }
        lua_rawset(L, LUA_REGISTRYINDEX);
        lua_settop(L, 0);
    }

    Unit* make(const char* bp, f32 x, f32 z, int army = 1) {
        REQUIRE(state
                    .do_string("made = CreateUnit('" + std::string(bp) + "', " +
                               std::to_string(army) + ", " + std::to_string(x) + ", 10, " +
                               std::to_string(z) + ")")
                    .ok());
        Unit* found = nullptr;
        sim.entity_registry().for_each_unit([&](osc::sim::Entity& e) {
            const auto& u = static_cast<Unit&>(e);
            if (u.blueprint_id() == bp && u.position().x == x && u.position().z == z)
                found = static_cast<Unit*>(&e);
        });
        REQUIRE(found);
        lua_settop(state.raw(), 0);
        return found;
    }

    const osc::sim::UnitBlockers& blockers() const { return sim.unit_blockers(); }
};

} // namespace

TEST_CASE("Box overlap: touching counts, turned boxes are tested on their own axes",
          "[unit_blocking]") {
    const OrientedBox a = axis_box({0, 0, 0}, 1, 1, 1);
    CHECK(osc::sim::boxes_overlap(a, axis_box({1.5f, 0, 0}, 1, 1, 1)));
    CHECK(osc::sim::boxes_overlap(a, axis_box({2.0f, 0, 0}, 1, 1, 1))); // touching
    CHECK_FALSE(osc::sim::boxes_overlap(a, axis_box({2.01f, 0, 0}, 1, 1, 1)));
    // Turned 45 degrees, its corner reaches sqrt(2) out: their bounds
    // overlap at 2.3 apart, the boxes don't.
    const OrientedBox t = turned_box({2.3f, 0, 2.3f}, 1, 1, 1, 0.785398f);
    CHECK_FALSE(osc::sim::boxes_overlap(a, t));
    CHECK(osc::sim::boxes_overlap(a, turned_box({2.3f, 0, 0}, 1, 1, 1, 0.785398f)));

    CHECK(osc::sim::box_sphere_overlap(a, {2.0f, 0, 0}, 1.0f));          // touching
    CHECK_FALSE(osc::sim::box_sphere_overlap(a, {2.0f, 0, 2.0f}, 1.0f)); // past the corner
    CHECK(osc::sim::box_sphere_overlap(a, {1.6f, 0, 1.6f}, 1.0f));
}

TEST_CASE("func_IsSourceUnit: whom a unit ignores, planning (1) and as a leader (2)",
          "[unit_blocking]") {
    World w;
    Unit& owner = *w.make("tank", 20.5f, 20.5f);
    Unit& other = *w.make("tank", 22.5f, 20.5f);
    Unit& flier = *w.make("gunship", 24.0f, 20.0f);
    w.sim.tick(); // positions noted: nothing has moved
    using osc::sim::is_source_unit;
    CHECK(is_source_unit(2, owner, owner));
    CHECK_FALSE(is_source_unit(2, owner, other));
    CHECK(is_source_unit(2, owner, flier)); // another layer
    // Planning, an idle unit counts unless the owner outranks it (here the
    // lower id goes first: they tie on everything else).
    CHECK(is_source_unit(1, owner, other) == osc::sim::outranks(owner, other));
    CHECK(is_source_unit(1, other, owner) == osc::sim::outranks(other, owner));
    CHECK(is_source_unit(1, owner, other) != is_source_unit(1, other, owner));
    // One that moved over the last tick doesn't count planning; a leader
    // minds it.
    other.set_position({23.5f, other.position().y, 20.5f});
    w.sim.tick();
    REQUIRE(other.moved_last_tick());
    CHECK(is_source_unit(1, owner, other));
    CHECK_FALSE(is_source_unit(2, owner, other));
    // Carried, or made by the upgrading owner, it is ignored.
    other.set_unit_state("Attached", true);
    CHECK(is_source_unit(2, owner, other));
    other.set_unit_state("Attached", false);
    other.set_creator_id(owner.entity_id());
    owner.set_unit_state("Upgrading", true);
    CHECK(is_source_unit(2, owner, other));
}

TEST_CASE("UnitIsBlocked: a still unit blocks the cells its box reaches; a flier and a far "
          "unit don't",
          "[unit_blocking]") {
    World w;
    Unit& owner = *w.make("tank", 10.5f, 10.5f);
    Unit& parked = *w.make("bigtank", 30.0f, 30.0f); // footprint 29..31
    w.make("gunship", 50.0f, 50.0f);
    w.sim.tick();
    w.sim.tick();
    const u32 id = owner.entity_id();
    REQUIRE_FALSE(parked.moved_last_tick());
    // A leader minds it at every cell its box (0.9 each way) reaches.
    CHECK(w.blockers().unit_blocked(id, Cell{29, 29}, 2));
    CHECK(w.blockers().unit_blocked(id, Cell{30, 30}, 2));
    CHECK_FALSE(w.blockers().unit_blocked(id, Cell{32, 30}, 2));
    CHECK_FALSE(w.blockers().unit_blocked(id, Cell{10, 10}, 2));
    CHECK_FALSE(w.blockers().unit_blocked(id, Cell{50, 50}, 2)); // in the air
    // At a world point: the cell a 1x1 footprint centred there takes.
    CHECK(w.blockers().unit_blocked_at(id, 30.5f, 30.5f, 2));
    // Planning, the bigger one outranks a 1x1 tank, so it counts.
    CHECK(osc::sim::outranks(parked, owner));
    CHECK(w.blockers().unit_blocked(id, Cell{30, 30}, 1));
    // The same unit seen by the bigger one: the 1x1 tank is outranked, and
    // planning ignores it; a leader doesn't.
    CHECK_FALSE(w.blockers().unit_blocked(parked.entity_id(), Cell{10, 10}, 1));
    CHECK(w.blockers().unit_blocked(parked.entity_id(), Cell{10, 10}, 2));
}

TEST_CASE("SweptPathBlockedByUnit: a unit across the way blocks it; beside it, not",
          "[unit_blocking]") {
    World w;
    Unit& owner = *w.make("tank", 10.5f, 10.5f);
    w.make("bigtank", 30.0f, 30.0f);
    w.sim.tick();
    w.sim.tick();
    const u32 id = owner.entity_id();
    using osc::sim::path::WorldPoint;
    CHECK(w.blockers().swept_blocked(id, WorldPoint{20, 0, 30}, WorldPoint{40, 0, 30}, 2));
    // 3 to the side: the box (0.9 each way) and the sweep (0.44 each way)
    // stay apart.
    CHECK_FALSE(w.blockers().swept_blocked(id, WorldPoint{20, 0, 33}, WorldPoint{40, 0, 33}, 2));
    // Stopping short of it.
    CHECK_FALSE(w.blockers().swept_blocked(id, WorldPoint{20, 0, 30}, WorldPoint{28, 0, 30}, 2));
    // No way at all.
    CHECK_FALSE(w.blockers().swept_blocked(id, WorldPoint{30, 0, 30}, WorldPoint{30, 0, 30}, 2));
}

TEST_CASE("A search minds mobile units only past its first: a leader's goes round one",
          "[unit_blocking]") {
    World w;
    Unit& owner = *w.make("tank", 10.5f, 10.5f);
    w.make("bigtank", 30.0f, 30.0f);
    w.sim.tick();
    w.sim.tick();
    osc::sim::path::PathWorld world{
        w.sim.terrain(), &w.sim.occupancy(), {0, 0, 128, 128}, false, 100000};
    world.blockers = &w.sim.unit_blockers();
    world.owner = owner.entity_id();
    osc::sim::path::PathFinder finder;
    finder.set_unit(owner.footprint(), 0, false);
    finder.prepare(world, osc::sim::path::SearchType::None, Cell{10, 10}, 10.5f, 10.5f);
    CHECK(finder.can_traverse(Cell{30, 30}));
    finder.prepare(world, osc::sim::path::SearchType::Leader, Cell{10, 10}, 10.5f, 10.5f);
    CHECK_FALSE(finder.can_traverse(Cell{30, 30}));
    CHECK(finder.can_traverse(Cell{35, 30}));
}
