// Idle aircraft landing (roadmap item 6): Moho's CUnitMotion landing phase.
// AutoLandTime after its orders run out, near its place, an aircraft finds a
// place (Unit::PrepareMove), reserves it, comes down and lands; an order
// takes it back up.

#include <catch2/catch_test_macros.hpp>

#include "blueprints/blueprint_store.hpp"
#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "lua/sim_bindings.hpp"
#include "map/heightmap.hpp"
#include "map/terrain.hpp"
#include "sim/occupancy.hpp"
#include "sim/saved_game.hpp"
#include "sim/sim_snapshot.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/unit_command.hpp"

extern "C" {
#include <lua.h>
}

#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using osc::f32;
using osc::i32;
using osc::u32;
using osc::sim::Unit;
namespace oc = osc::blueprints::occupancy;

namespace {

std::unique_ptr<osc::map::Terrain> flat_terrain(u32 size, f32 water) {
    std::vector<osc::u16> heights(static_cast<size_t>(size + 1) * (size + 1), 1280);
    osc::map::Heightmap hm(size, size, 1.0f / 128.0f, std::move(heights));
    return std::make_unique<osc::map::Terrain>(std::move(hm), water, water > 0.0f);
}

/// A sim on a flat map with a 2x2 aircraft that lands a second after its
/// orders run out ('plane'), and one that never does ('drone').
struct World {
    osc::lua::LuaState state;
    osc::blueprints::BlueprintStore store{state.raw()};
    osc::sim::SimState sim{state.raw(), &store};

    explicit World(u32 size = 128, f32 water = 0.0f) {
        osc::sim::GameSetup game;
        game.scenario = "/maps/test/test_scenario.lua";
        game.seed = 3;
        osc::lua::register_moho_bindings(state, sim);
        osc::lua::register_sim_bindings(state, sim);
        sim.set_terrain(flat_terrain(size, water));
        sim.build_pathfinding_grid();
        sim.add_army("ARMY_1", "ARMY_1");
        sim.set_game_setup(game);
        lua_State* L = state.raw();
        for (const char* bp : {
                 "{BlueprintId = 'plane', Categories = {'AIR', 'MOBILE'},"
                 " Defense = {MaxHealth = 100}, SizeX = 2, SizeZ = 2,"
                 " Footprint = {SizeX = 2, SizeZ = 2},"
                 " Air = {CanFly = true, MaxAirspeed = 10, AutoLandTime = 1, StartTurnDistance = "
                 "5},"
                 " Physics = {MotionType = 'RULEUMT_Air', Elevation = 10}}",
                 "{BlueprintId = 'drone', Categories = {'AIR', 'MOBILE'},"
                 " Defense = {MaxHealth = 100}, SizeX = 2, SizeZ = 2,"
                 " Footprint = {SizeX = 2, SizeZ = 2},"
                 " Air = {CanFly = true, MaxAirspeed = 10, AutoLandTime = 0},"
                 " Physics = {MotionType = 'RULEUMT_Air', Elevation = 10}}",
                 "{BlueprintId = 'ferry', Categories = {'AIR', 'MOBILE', 'TRANSPORTATION'},"
                 " Defense = {MaxHealth = 100}, SizeX = 2, SizeZ = 2,"
                 " Footprint = {SizeX = 2, SizeZ = 2},"
                 " Air = {CanFly = true, MaxAirspeed = 10, AutoLandTime = 1, StartTurnDistance = 5,"
                 " TransportHoverHeight = 3},"
                 " Physics = {MotionType = 'RULEUMT_Air', Elevation = 10}}",
                 "{BlueprintId = 'pod', Categories = {'AIR', 'MOBILE', 'CANLANDONWATER'},"
                 " Defense = {MaxHealth = 100}, SizeX = 2, SizeZ = 2,"
                 " Footprint = {SizeX = 2, SizeZ = 2},"
                 " Air = {CanFly = true, MaxAirspeed = 10, AutoLandTime = 1, StartTurnDistance = "
                 "5},"
                 " Physics = {MotionType = 'RULEUMT_Air', Elevation = 10}}",
                 "{BlueprintId = 'tank', Categories = {'LAND', 'MOBILE'},"
                 " Defense = {MaxHealth = 100}, SizeX = 1, SizeZ = 1,"
                 " Physics = {MotionType = 'RULEUMT_Land'}}",
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
        for (const char* id : {"plane", "drone", "ferry", "pod", "tank"}) {
            lua_pushstring(L, id);
            lua_getglobal(L, "Plain");
            lua_rawset(L, -3);
        }
        lua_rawset(L, LUA_REGISTRYINDEX);
        lua_settop(L, 0);
    }

    Unit* make(const char* bp, f32 x, f32 z) {
        REQUIRE(state
                    .do_string("made = CreateUnit('" + std::string(bp) + "', 1, " +
                               std::to_string(x) + ", 10, " + std::to_string(z) + ")")
                    .ok());
        Unit* found = nullptr;
        sim.entity_registry().for_each_unit([&](osc::sim::Entity& e) {
            auto& u = static_cast<Unit&>(e);
            if (u.blueprint_id() == bp && u.position().x == x && u.position().z == z) found = &u;
        });
        REQUIRE(found);
        lua_settop(state.raw(), 0);
        return found;
    }

    static void move(Unit& u, f32 x, f32 z) {
        osc::sim::UnitCommand cmd;
        cmd.type = osc::sim::CommandType::Move;
        cmd.target_pos = {x, 20.0f, z};
        u.push_command(cmd, true);
    }

    int run_until(const std::function<bool()>& done, int limit) {
        int t = 0;
        for (; t < limit && !done(); ++t) sim.tick();
        return t;
    }
};

} // namespace

TEST_CASE("An idle aircraft lands a second after its orders run out: it reserves its place, "
          "comes down onto it, and is on the land, still",
          "[air_landing]") {
    World w;
    Unit& plane = *w.make("plane", 30.0f, 30.0f);
    REQUIRE(plane.is_air_unit());
    World::move(plane, 60.0f, 60.0f);
    const int flew = w.run_until([&] { return plane.command_queue().empty(); }, 400);
    CHECK(flew < 400);
    CHECK(plane.is_air_unit());
    // Within the second, still up.
    for (int i = 0; i < 5; ++i) w.sim.tick();
    CHECK(plane.is_air_unit());
    CHECK_FALSE(plane.idle_landing().descending);
    bool reserved_seen = false;
    bool moving_down_seen = false;
    const int landed = w.run_until(
        [&] {
            if (plane.idle_landing().descending) {
                const auto& r = plane.idle_landing().reserved;
                reserved_seen = reserved_seen || w.sim.occupancy().reserved_any(r);
            }
            moving_down_seen = moving_down_seen || plane.has_unit_state("MovingDown");
            return !plane.is_air_unit();
        },
        300);
    CHECK(landed < 300);
    CHECK(reserved_seen);
    CHECK(moving_down_seen);
    CHECK(plane.layer() == "Land");
    CHECK(plane.vert_event() == "Bottom");
    CHECK_FALSE(plane.has_unit_state("MovingDown"));
    CHECK(plane.current_altitude() == 0.0f);
    CHECK(std::abs(plane.position().x - 60.0f) < 6.0f);
    CHECK(std::abs(plane.position().z - 60.0f) < 6.0f);
    // Its place is free again once it is down.
    const auto at =
        osc::sim::footprint_rect(plane.footprint(), plane.position().x, plane.position().z);
    CHECK_FALSE(w.sim.occupancy().reserved_any(at));
}

TEST_CASE("Two aircraft idle at one spot land apart: the second keeps off the first's place",
          "[air_landing]") {
    World w;
    Unit& a = *w.make("plane", 59.0f, 59.0f);
    Unit& b = *w.make("plane", 61.0f, 61.0f);
    const auto both_down = [&] { return !a.is_air_unit() && !b.is_air_unit(); };
    w.run_until(both_down, 400);
    REQUIRE(both_down());
    const f32 dx = a.position().x - b.position().x;
    const f32 dz = a.position().z - b.position().z;
    CHECK(std::sqrt(dx * dx + dz * dz) >= 2.0f); // their 2x2 places apart
}

TEST_CASE("With nowhere to land an aircraft stays up, CannotFindPlaceToLand; one that never "
          "lands idles in the air",
          "[air_landing]") {
    World w(64);
    osc::sim::GroundOccupant everything;
    everything.caps = oc::kLand;
    everything.rects = {{0, 0, 64, 64}};
    w.sim.occupy_ground(9999, everything);
    Unit& plane = *w.make("plane", 32.0f, 32.0f);
    for (int i = 0; i < 40; ++i) w.sim.tick();
    CHECK(plane.is_air_unit());
    CHECK(plane.has_unit_state("CannotFindPlaceToLand"));

    World w2;
    Unit& drone = *w2.make("drone", 32.0f, 32.0f);
    for (int i = 0; i < 100; ++i) w2.sim.tick();
    CHECK(drone.is_air_unit());
    CHECK_FALSE(drone.idle_landing().descending);
}

TEST_CASE("An aircraft under construction neither lands nor takes off", "[air_landing]") {
    World w;
    Unit& plane = *w.make("plane", 40.0f, 40.0f);
    plane.set_is_being_built(true);
    const osc::sim::Vector3 at = plane.position();
    for (int i = 0; i < 60; ++i) w.sim.tick();
    CHECK(plane.is_air_unit());
    CHECK_FALSE(plane.idle_landing().descending);
    CHECK(plane.position().x == at.x);
    CHECK(plane.position().z == at.z);
}

TEST_CASE("A landed aircraft given an order takes off, climbs to its height and flies it",
          "[air_landing]") {
    World w;
    Unit& plane = *w.make("plane", 40.0f, 40.0f);
    w.run_until([&] { return !plane.is_air_unit(); }, 300);
    REQUIRE(plane.layer() == "Land");
    World::move(plane, 90.0f, 40.0f);
    w.sim.tick();
    CHECK(plane.is_air_unit());
    CHECK(plane.has_unit_state("MovingUp"));
    const int up = w.run_until([&] { return !plane.has_unit_state("MovingUp"); }, 200);
    CHECK(up < 200);
    CHECK(plane.vert_event() == "Top");
    w.run_until([&] { return plane.command_queue().empty(); }, 400);
    CHECK(plane.position().x > 80.0f);
}

TEST_CASE("A game saved while an aircraft comes down loads and goes on as the original, its "
          "place still reserved",
          "[air_landing]") {
    World a;
    Unit& plane = *a.make("plane", 50.0f, 50.0f);
    a.sim.set_recording(true);
    a.run_until([&] { return plane.idle_landing().descending; }, 200);
    REQUIRE(plane.idle_landing().descending);
    a.sim.tick();
    const osc::sim::SavedGame save = osc::sim::save_game(a.sim, "landing");
    REQUIRE_FALSE(save.snapshot.empty());
    World b;
    b.make("plane", 50.0f, 50.0f);
    const std::string err = osc::sim::load_snapshot(b.sim, save.snapshot);
    INFO(err);
    REQUIRE(err.empty());
    CHECK(b.sim.occupancy().reserved_any(plane.idle_landing().reserved));
    for (int t = 0; t < 80; ++t) {
        a.sim.tick();
        b.sim.tick();
        INFO("tick " << t);
        REQUIRE(b.sim.compute_sync_checksum() == a.sim.compute_sync_checksum());
    }
    CHECK(plane.layer() == "Land");
}

TEST_CASE("A landed aircraft refuels by itself, at its FuelRechargeRate; flying burns fuel",
          "[air_landing]") {
    World w;
    Unit& plane = *w.make("plane", 40.0f, 40.0f);
    plane.set_fuel_use_time(100.0f);     // 1000 ticks a tank
    plane.set_fuel_recharge_rate(10.0f); // 10% a second, landed
    plane.set_fuel_ratio(0.5f);
    w.sim.tick();
    CHECK(plane.fuel_ratio() < 0.5f); // up, it burns
    w.run_until([&] { return !plane.is_air_unit(); }, 300);
    REQUIRE(plane.vert_event() == "Bottom");
    const f32 down = plane.fuel_ratio();
    for (int i = 0; i < 10; ++i) w.sim.tick();
    // A second on the ground: 10 / 100 x 0.1 a tick, ten ticks.
    CHECK(std::abs(plane.fuel_ratio() - std::min(down + 0.1f, 1.0f)) < 1e-4f);
    for (int i = 0; i < 100; ++i) w.sim.tick();
    CHECK(plane.fuel_ratio() == 1.0f);
}

TEST_CASE("An order while an aircraft comes down: it gives up its place, climbs back and flies "
          "the order",
          "[air_landing]") {
    World w;
    Unit& plane = *w.make("plane", 40.0f, 40.0f);
    w.run_until([&] { return plane.idle_landing().descending; }, 200);
    REQUIRE(plane.idle_landing().descending);
    w.sim.tick();
    const auto place = plane.idle_landing().reserved;
    REQUIRE(w.sim.occupancy().reserved_any(place));
    const f32 low = plane.current_altitude();
    World::move(plane, 100.0f, 40.0f);
    w.sim.tick();
    CHECK_FALSE(plane.idle_landing().descending);
    CHECK_FALSE(plane.has_unit_state("MovingDown"));
    CHECK_FALSE(w.sim.occupancy().reserved_any(place));
    CHECK(plane.is_air_unit());
    w.run_until([&] { return plane.command_queue().empty(); }, 400);
    CHECK(plane.position().x > 90.0f);
    CHECK(plane.current_altitude() > low);
}

TEST_CASE("An idle empty transport lands as any aircraft does", "[air_landing][transport]") {
    World w;
    Unit& ferry = *w.make("ferry", 40.0f, 40.0f);
    const int landed = w.run_until([&] { return !ferry.is_air_unit(); }, 300);
    CHECK(landed < 300);
    CHECK(ferry.layer() == "Land");
    CHECK(ferry.vert_event() == "Bottom");
}

TEST_CASE("Over deep water a TRANSPORTATION or CANLANDONWATER aircraft lands on the water; "
          "another finds no place",
          "[air_landing][transport]") {
    World w(64, 20.0f);
    Unit& ferry = *w.make("ferry", 20.0f, 20.0f);
    Unit& pod = *w.make("pod", 44.0f, 44.0f);
    Unit& plane = *w.make("plane", 20.0f, 44.0f);
    w.run_until([&] { return !ferry.is_air_unit() && !pod.is_air_unit(); }, 300);
    CHECK(ferry.layer() == "Water");
    CHECK(std::abs(ferry.position().x - 20.0f) < 1.0f);
    CHECK(std::abs(ferry.position().z - 20.0f) < 1.0f);
    CHECK(pod.layer() == "Water");
    CHECK(plane.is_air_unit());
    CHECK(plane.has_unit_state("CannotFindPlaceToLand"));
}

TEST_CASE("An idle transport with cargo comes down to its TransportHoverHeight and hovers there, "
          "refuelling",
          "[air_landing][transport]") {
    World w;
    Unit& ferry = *w.make("ferry", 40.0f, 40.0f);
    Unit& tank = *w.make("tank", 44.0f, 40.0f);
    tank.attach_to_transport(&ferry, w.sim.entity_registry(), w.state.raw());
    REQUIRE(ferry.cargo_ids().size() == 1);
    ferry.set_fuel_use_time(100.0f);
    ferry.set_fuel_recharge_rate(10.0f);
    ferry.set_fuel_ratio(0.5f);
    bool down_seen = false;
    const int hovering = w.run_until(
        [&] {
            down_seen =
                down_seen || (ferry.vert_event() == "Down" && ferry.has_unit_state("MovingDown"));
            return ferry.vert_event() == "Hover";
        },
        300);
    CHECK(hovering < 300);
    CHECK(down_seen);
    CHECK_FALSE(ferry.has_unit_state("MovingDown"));
    CHECK(ferry.is_air_unit());
    CHECK(std::abs(ferry.current_altitude() - 3.0f) < 1e-4f);
    const f32 low = ferry.fuel_ratio();
    for (int i = 0; i < 10; ++i) w.sim.tick();
    CHECK(ferry.fuel_ratio() > low);
    CHECK(ferry.vert_event() == "Hover");
}

TEST_CASE("A transport given an order out of its hover climbs away: Up, then Top",
          "[air_landing][transport]") {
    World w;
    Unit& ferry = *w.make("ferry", 40.0f, 40.0f);
    Unit& tank = *w.make("tank", 44.0f, 40.0f);
    tank.attach_to_transport(&ferry, w.sim.entity_registry(), w.state.raw());
    w.run_until([&] { return ferry.vert_event() == "Hover"; }, 300);
    REQUIRE(ferry.vert_event() == "Hover");
    World::move(ferry, 100.0f, 40.0f);
    bool up_seen = false;
    const int top = w.run_until(
        [&] {
            up_seen = up_seen || (ferry.vert_event() == "Up" && ferry.has_unit_state("MovingUp"));
            return ferry.vert_event() == "Top";
        },
        300);
    CHECK(top < 300);
    CHECK(up_seen);
    CHECK_FALSE(ferry.has_unit_state("MovingUp"));
}

TEST_CASE("A transport unloading comes down to its hover height as to a landing: Down, then Hover",
          "[air_landing][transport]") {
    World w;
    Unit& ferry = *w.make("ferry", 40.0f, 40.0f);
    Unit& tank = *w.make("tank", 44.0f, 40.0f);
    tank.attach_to_transport(&ferry, w.sim.entity_registry(), w.state.raw());
    osc::sim::UnitCommand drop;
    drop.type = osc::sim::CommandType::TransportUnload;
    drop.target_pos = ferry.position();
    ferry.push_command(drop, true);
    bool down_seen = false;
    std::string at_drop;
    const int dropped = w.run_until(
        [&] {
            down_seen = down_seen || ferry.vert_event() == "Down";
            if (ferry.cargo_ids().empty()) {
                at_drop = ferry.vert_event();
                return true;
            }
            return false;
        },
        300);
    CHECK(dropped < 300);
    CHECK(down_seen);
    CHECK(at_drop == "Hover");
}
