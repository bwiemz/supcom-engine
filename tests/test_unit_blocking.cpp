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
        sim.set_victory_condition("sandbox"); // no commanders here: nobody is eliminated
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
                 "{BlueprintId = 'fasttank', Categories = {'LAND', 'MOBILE'},"
                 " Defense = {MaxHealth = 100}, SizeX = 0.8, SizeY = 0.5, SizeZ = 0.8,"
                 " Footprint = {SizeX = 1, SizeZ = 1},"
                 " Physics = {MotionType = 'RULEUMT_Land', MaxSpeed = 8}}",
                 "{BlueprintId = 'engineer', Categories = {'LAND', 'MOBILE', 'ENGINEER'},"
                 " Defense = {MaxHealth = 100}, SizeX = 0.8, SizeY = 0.5, SizeZ = 0.8,"
                 " Footprint = {SizeX = 1, SizeZ = 1},"
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
        for (const char* id : {"tank", "fasttank", "bigtank", "engineer", "gunship"}) {
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

TEST_CASE("UnitIsBlocked sees a box as far as it reaches turned, and stretched by a quaternion "
          "off unit length",
          "[unit_blocking]") {
    World w;
    Unit& owner = *w.make("tank", 10.5f, 10.5f);
    Unit& parked = *w.make("bigtank", 30.0f, 30.0f);
    w.sim.tick();
    w.sim.tick();
    const u32 id = owner.entity_id();
    // Turned 45 degrees, its corner (0.9 * sqrt 2 = 1.27 out) reaches the
    // next cell along x.
    const f32 half = std::sqrt(0.5f);
    parked.set_orientation({0.0f, std::sin(0.3926991f), 0.0f, std::cos(0.3926991f)});
    CHECK(w.blockers().unit_blocked(id, Cell{31, 30}, 2));
    CHECK_FALSE(w.blockers().unit_blocked(id, Cell{32, 30}, 2));
    // A script's quaternion of length 2 stretches the box sevenfold along x
    // and z (the rotation formula assumes unit length): 6.3 out, far past
    // the shape's own reach, as far as the grid's COLLIDER_REACH looks.
    parked.set_orientation({0.0f, 2.0f, 0.0f, 0.0f});
    CHECK(w.blockers().unit_blocked(id, Cell{35, 30}, 2));
    CHECK_FALSE(w.blockers().unit_blocked(id, Cell{37, 30}, 2));
    parked.set_orientation({0.0f, half, 0.0f, half});
    CHECK_FALSE(w.blockers().unit_blocked(id, Cell{35, 30}, 2));
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

namespace {

/// Give `u` an order of `type` (at the head, the queue cleared).
void order(Unit& u, osc::sim::CommandType type, u32 command_id, bool formed, u32 target = 0) {
    osc::sim::UnitCommand cmd;
    cmd.type = type;
    cmd.command_id = command_id;
    cmd.formed = formed;
    cmd.target_id = target;
    cmd.target_pos = u.position();
    u.push_command(cmd, true);
}

} // namespace

TEST_CASE("Formation layers: one formation order's slots, and a unit's guards (no engineers)",
          "[unit_blocking]") {
    World w;
    using osc::sim::CommandType;
    using osc::sim::formation_layer;
    using osc::sim::same_formation_layer;
    Unit& a = *w.make("tank", 10.5f, 10.5f);
    Unit& b = *w.make("tank", 12.5f, 10.5f);
    Unit& c = *w.make("tank", 14.5f, 10.5f);
    Unit& guarded = *w.make("bigtank", 30.0f, 30.0f);
    Unit& g1 = *w.make("tank", 20.5f, 20.5f);
    Unit& g2 = *w.make("tank", 22.5f, 20.5f);
    Unit& eng = *w.make("engineer", 24.5f, 20.5f);
    CHECK(formation_layer(a) == 0); // no orders
    order(a, CommandType::Move, 7, true);
    order(b, CommandType::Move, 7, true);
    order(c, CommandType::Move, 7, false); // the same order, not laid out
    CHECK(formation_layer(a) != 0);
    CHECK(same_formation_layer(a, b));
    CHECK_FALSE(same_formation_layer(a, c));
    // Attacking, it leaves the formation's exemptions.
    b.set_unit_state("Attacking", true);
    CHECK_FALSE(same_formation_layer(a, b));
    b.set_unit_state("Attacking", false);
    // A unit's guards share its guard formation; an engineer guards alone.
    order(g1, CommandType::Guard, 11, false, guarded.entity_id());
    order(g2, CommandType::Guard, 12, false, guarded.entity_id());
    order(eng, CommandType::Guard, 13, false, guarded.entity_id());
    CHECK(same_formation_layer(g1, g2));
    CHECK(formation_layer(eng) == 0);
    // An ally's guard of the same unit is in its own army's formation.
    Unit& ally = *w.make("tank", 26.5f, 20.5f, 2);
    order(ally, CommandType::Guard, 14, false, guarded.entity_id());
    CHECK(formation_layer(ally) == formation_layer(g1));
    CHECK_FALSE(same_formation_layer(g1, ally));
    CHECK_FALSE(same_formation_layer(g1, a));
}

TEST_CASE("Units in one formation don't block each other's way", "[unit_blocking]") {
    World w;
    using osc::sim::CommandType;
    Unit& owner = *w.make("tank", 10.5f, 10.5f);
    Unit& parked = *w.make("bigtank", 30.0f, 30.0f);
    w.sim.tick();
    w.sim.tick();
    const u32 id = owner.entity_id();
    order(owner, CommandType::Move, 21, true);
    order(parked, CommandType::Move, 21, true);
    CHECK_FALSE(w.blockers().unit_blocked(id, Cell{30, 30}, 2));
    using osc::sim::path::WorldPoint;
    CHECK_FALSE(w.blockers().swept_blocked(id, WorldPoint{20, 0, 30}, WorldPoint{40, 0, 30}, 2));
    order(parked, CommandType::Move, 22, true); // another formation
    CHECK(w.blockers().unit_blocked(id, Cell{30, 30}, 2));
    CHECK(w.blockers().swept_blocked(id, WorldPoint{20, 0, 30}, WorldPoint{40, 0, 30}, 2));
}

TEST_CASE("Overtaking in one formation, the slower unit isn't stopped for the faster",
          "[unit_blocking]") {
    using osc::sim::CommandType;
    // A fast tank comes up behind a slow one on its line, both going north;
    // whether the slow one is ever held for it.
    const auto slow_held = [](bool formed) {
        World w;
        Unit& slow = *w.make("tank", 64.5f, 20.5f);
        Unit& fast = *w.make("fasttank", 64.5f, 12.5f);
        const auto go = [&](Unit& u, f32 z) {
            osc::sim::UnitCommand cmd;
            cmd.type = CommandType::Move;
            cmd.command_id = 31;
            cmd.formed = formed;
            cmd.target_pos = {64.5f, 10.0f, z};
            u.push_command(cmd, true);
        };
        go(slow, 60.5f);
        go(fast, 100.5f);
        bool held = false;
        for (int t = 0; t < 200; ++t) {
            w.sim.tick();
            held = held || slow.navigator().holding();
        }
        return held;
    };
    CHECK(slow_held(false)); // the steering stops it for the overtaker
    CHECK_FALSE(slow_held(true));
}
