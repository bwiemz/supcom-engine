// Idle aircraft landing (roadmap item 6): Moho's CUnitMotion landing phase.
// AutoLandTime after its orders run out, near its place, an aircraft finds a
// place (Unit::PrepareMove), reserves it, comes down and lands; an order
// takes it back up.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

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

#include <algorithm>
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
/// orders run out ('plane', winged 'fighter'), and one that never does
/// ('drone', winged 'flier').
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
                 "{BlueprintId = 'giant', Categories = {'AIR', 'MOBILE', 'EXPERIMENTAL'},"
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
                 "{BlueprintId = 'fighter', Categories = {'AIR', 'MOBILE'},"
                 " Defense = {MaxHealth = 100}, SizeX = 2, SizeZ = 2,"
                 " Footprint = {SizeX = 2, SizeZ = 2},"
                 " Air = {CanFly = true, Winged = true, MaxAirspeed = 15, KMove = 1,"
                 " KMoveDamping = 1, StartTurnDistance = 5, TurnSpeed = 1.5, AutoLandTime = 1},"
                 " Physics = {MotionType = 'RULEUMT_Air', Elevation = 18}}",
                 "{BlueprintId = 'flier', Categories = {'AIR', 'MOBILE'},"
                 " Defense = {MaxHealth = 100}, SizeX = 2, SizeZ = 2,"
                 " Footprint = {SizeX = 2, SizeZ = 2},"
                 " Air = {CanFly = true, Winged = true, MaxAirspeed = 15, KMove = 1,"
                 " KMoveDamping = 1, StartTurnDistance = 5, TurnSpeed = 1.5, AutoLandTime = 0},"
                 " Physics = {MotionType = 'RULEUMT_Air', Elevation = 18}}",
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
        for (const char* id :
             {"plane", "giant", "drone", "ferry", "pod", "fighter", "flier", "tank"}) {
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

    Unit* build(const char* bp, f32 x, f32 z) {
        lua_State* L = state.raw();
        lua_pushstring(L, "__osc_create_building_unit");
        lua_rawget(L, LUA_REGISTRYINDEX);
        lua_pushstring(L, bp);
        lua_pushnumber(L, 1);
        lua_pushnumber(L, x);
        lua_pushnil(L);
        lua_pushnumber(L, z);
        REQUIRE(lua_pcall(L, 5, 2, 0) == 0);
        auto* u =
            static_cast<Unit*>(sim.entity_registry().find(static_cast<u32>(lua_tonumber(L, -2))));
        lua_settop(L, 0);
        REQUIRE(u);
        return u;
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

TEST_CASE("Aircraft sent to one point all finish the move there and land", "[air_landing]") {
    World w;
    std::vector<Unit*> planes;
    for (int i = 0; i < 5; ++i) {
        planes.push_back(w.make("plane", 30.0f + 3.0f * static_cast<f32>(i), 30.0f));
        World::move(*planes.back(), 64.0f, 64.0f);
    }
    std::vector<int> finished(planes.size(), -1);
    int tick = 0;
    const int t = w.run_until(
        [&] {
            for (size_t i = 0; i < planes.size(); ++i) {
                if (finished[i] < 0 && planes[i]->command_queue().empty()) {
                    finished[i] = tick;
                }
            }
            ++tick;
            return std::all_of(planes.begin(), planes.end(),
                               [](const Unit* p) { return !p->is_air_unit(); });
        },
        800);
    INFO("ticks " << t);
    for (size_t i = 0; i < planes.size(); ++i) {
        INFO("plane " << i << " finished its move at tick " << finished[i]);
        CHECK(finished[i] >= 0);
        CHECK_FALSE(planes[i]->is_air_unit());
    }
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

namespace {

struct Flight {
    f32 slowest = 1e9f;
    f32 farthest = 0.0f;
    int landing_tries = 0;
};

Flight watch(World& w, const Unit& u, const osc::sim::Vector3& from, int after, int ticks) {
    for (int t = 0; t < after; ++t) {
        w.sim.tick();
    }
    Flight seen;
    for (int t = 0; t < ticks; ++t) {
        const osc::sim::Vector3 was = u.position();
        const bool failed = u.has_unit_state("CannotFindPlaceToLand");
        w.sim.tick();
        if (!failed && u.has_unit_state("CannotFindPlaceToLand")) {
            ++seen.landing_tries;
        }
        const osc::sim::Vector3 at = u.position();
        seen.slowest = std::min(seen.slowest, std::hypot(at.x - was.x, at.z - was.z) * 10.0f);
        seen.farthest = std::max(seen.farthest, std::hypot(at.x - from.x, at.z - from.z));
    }
    return seen;
}

} // namespace

TEST_CASE("An idle aircraft that can't land, or is told to speed through, flies on round its "
          "place at speed; one that isn't comes to rest",
          "[air_landing]") {
    const osc::sim::Vector3 place{64.0f, 0.0f, 64.0f};
    SECTION("CannotFindPlaceToLand") {
        World w(128, 20.0f);
        Unit& fighter = *w.make("fighter", 30.0f, 64.0f);
        World::move(fighter, place.x, place.z);
        w.run_until([&] { return fighter.has_unit_state("CannotFindPlaceToLand"); }, 400);
        REQUIRE(fighter.has_unit_state("CannotFindPlaceToLand"));
        REQUIRE(fighter.command_queue().empty());
        const Flight seen = watch(w, fighter, place, 20, 300);
        CHECK(seen.slowest > 2.0f);
        CHECK(seen.farthest < 30.0f);
        CHECK(seen.landing_tries >= 2);
    }
    SECTION("ForceSpeedThrough") {
        World w;
        Unit& flier = *w.make("flier", 30.0f, 64.0f);
        World::move(flier, place.x, place.z);
        w.run_until([&] { return flier.command_queue().empty(); }, 400);
        flier.set_unit_state("ForceSpeedThrough", true);
        const Flight seen = watch(w, flier, place, 20, 300);
        CHECK(seen.slowest > 2.0f);
        CHECK(seen.farthest < 30.0f);
    }
    SECTION("neither") {
        World w;
        Unit& flier = *w.make("flier", 30.0f, 64.0f);
        World::move(flier, place.x, place.z);
        w.run_until([&] { return flier.command_queue().empty(); }, 400);
        flier.navigator().set_speed_through_goal(true);
        CHECK(watch(w, flier, place, 100, 100).slowest < 0.1f);
    }
}

TEST_CASE("A winged aircraft guarding a unit that stands still flies round it, never hanging "
          "over it",
          "[air_landing]") {
    World w;
    const Unit& tank = *w.make("tank", 64.0f, 64.0f);
    Unit& fighter = *w.make("fighter", 30.0f, 64.0f);
    osc::sim::UnitCommand guard;
    guard.type = osc::sim::CommandType::Guard;
    guard.target_id = tank.entity_id();
    guard.target_pos = tank.position();
    fighter.push_command(guard, true);
    const Flight seen = watch(w, fighter, tank.position(), 200, 300);
    CHECK(seen.slowest > 2.0f);
    CHECK(seen.farthest < 30.0f);
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

TEST_CASE("An EXPERIMENTAL aircraft is built on the ground and takes off once finished; another "
          "is built in the Air",
          "[air_landing]") {
    const bool experimental = GENERATE(false, true);
    World w;
    Unit& u = *w.build(experimental ? "giant" : "plane", 40.0f, 40.0f);
    if (experimental) {
        CHECK(u.layer() == "Land");
        CHECK(u.position().y == 10.0f);
    } else {
        CHECK(u.layer() == "Air");
    }
    const std::string layer = u.layer();
    for (int i = 0; i < 10; ++i) {
        w.sim.tick();
    }
    CHECK(u.layer() == layer);
    u.set_is_being_built(false);
    w.sim.tick();
    CHECK(u.is_air_unit());
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

TEST_CASE("A landed aircraft refuels by itself, FuelRechargeRate / FuelUseTime / 100 a tick; "
          "flying burns fuel",
          "[air_landing]") {
    World w;
    Unit& plane = *w.make("plane", 40.0f, 40.0f);
    plane.set_fuel_use_time(100.0f);
    plane.set_fuel_recharge_rate(5.0f);
    plane.set_fuel_ratio(0.2f);
    w.sim.tick();
    CHECK(plane.fuel_ratio() < 0.2f);
    w.run_until([&] { return !plane.is_air_unit(); }, 300);
    REQUIRE(plane.vert_event() == "Bottom");
    const f32 down = plane.fuel_ratio();
    for (int i = 0; i < 100; ++i) w.sim.tick();
    CHECK(std::abs(plane.fuel_ratio() - (down + 0.05f)) < 1e-4f);
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
                return true;
            }
            at_drop = ferry.vert_event();
            return false;
        },
        300);
    CHECK(dropped < 300);
    CHECK(down_seen);
    CHECK(at_drop == "Hover");
}

TEST_CASE("An aircraft flies one step a tick as it passes from one move to the next",
          "[air_landing]") {
    World w;
    Unit& plane = *w.make("drone", 30.0f, 30.0f);
    World::move(plane, 60.0f, 30.0f);
    osc::sim::UnitCommand next;
    next.type = osc::sim::CommandType::Move;
    next.target_pos = {60.0f, 20.0f, 80.0f};
    plane.push_command(next, false);
    f32 longest = 0.0f;
    int flew = 0;
    for (; flew < 400 && !plane.command_queue().empty(); ++flew) {
        const auto before = plane.position();
        w.sim.tick();
        const f32 dx = plane.position().x - before.x;
        const f32 dz = plane.position().z - before.z;
        longest = std::max(longest, std::sqrt(dx * dx + dz * dz));
    }
    CHECK(flew < 400);
    CHECK(plane.position().z > 70.0f);
    CHECK(longest <= 10.0f * 0.1f + 1e-3f);
}

TEST_CASE("An aircraft flies one step when a move hands over to a guard", "[air_landing]") {
    World queued;
    World plain;
    Unit& plane = *queued.make("drone", 30.0f, 30.0f);
    Unit& reference = *plain.make("drone", 30.0f, 30.0f);
    World::move(plane, 60.0f, 30.0f);
    World::move(reference, 60.0f, 30.0f);
    osc::sim::UnitCommand guard;
    guard.type = osc::sim::CommandType::Guard;
    guard.target_pos = {60.0f, 20.0f, 80.0f};
    SECTION("a point") {}
    SECTION("a unit") {
        guard.target_id = queued.make("tank", 60.0f, 80.0f)->entity_id();
    }
    plane.push_command(guard, false);
    // Both queues fly through the first goal; the reference's next Move
    // already obeys the once-per-tick gate.
    osc::sim::UnitCommand next = guard;
    next.type = osc::sim::CommandType::Move;
    next.target_id = 0;
    reference.push_command(next, false);
    bool handed_over = false;
    for (int tick = 0; tick < 400 && !handed_over; ++tick) {
        queued.sim.tick();
        plain.sim.tick();
        handed_over = plane.command_queue().front().type == osc::sim::CommandType::Guard;
        INFO("tick " << tick);
        CHECK(std::abs(plane.position().x - reference.position().x) <= 1e-5f);
        CHECK(std::abs(plane.position().y - reference.position().y) <= 1e-5f);
        CHECK(std::abs(plane.position().z - reference.position().z) <= 1e-5f);
    }
    REQUIRE(handed_over);
}
