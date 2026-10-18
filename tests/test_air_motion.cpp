// An aircraft's level flight: Moho's CUnitMotion::CalcMoveAir and
// ComputeAirControl (faf-re CUnitMotion.cpp) under its blueprint's KMove and
// KMoveDamping.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "blueprints/blueprint_store.hpp"
#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "lua/sim_bindings.hpp"
#include "map/heightmap.hpp"
#include "map/terrain.hpp"
#include "sim/bone_data.hpp"
#include "sim/flight_math.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/unit_command.hpp"
#include "sim/weapon.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

using Catch::Approx;
using osc::f32;
using osc::sim::Unit;

namespace {

struct World {
    osc::lua::LuaState state;
    osc::blueprints::BlueprintStore store{state.raw()};
    osc::sim::SimState sim{state.raw(), &store};

    explicit World(bool rival = false) {
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
        if (rival) {
            sim.add_army("ARMY_2", "ARMY_2");
            sim.set_fog_of_war("none");
            sim.set_victory_condition("sandbox");
        }
        sim.set_game_setup(game);
        lua_State* L = state.raw();
        for (const char* bp : {
                 "{BlueprintId = 'gunship', Categories = {'AIR', 'MOBILE'},"
                 " Defense = {MaxHealth = 100}, SizeX = 2, SizeZ = 2,"
                 " Footprint = {SizeX = 2, SizeZ = 2},"
                 " Air = {CanFly = true, MaxAirspeed = 12, KMove = 0.8, KMoveDamping = 2,"
                 " StartTurnDistance = 5, AutoLandTime = 0},"
                 " Physics = {MotionType = 'RULEUMT_Air', Elevation = 10}}",
                 "{BlueprintId = 'gun1', Categories = {'AIR', 'MOBILE'},"
                 " Defense = {MaxHealth = 100}, SizeX = 1, SizeZ = 1,"
                 " Footprint = {SizeX = 1, SizeZ = 1},"
                 " Air = {CanFly = true, MaxAirspeed = 12, KMove = 0.8, KMoveDamping = 2,"
                 " StartTurnDistance = 5, AutoLandTime = 0},"
                 " Physics = {MotionType = 'RULEUMT_Air', Elevation = 10}}",
                 "{BlueprintId = 'fighter', Categories = {'AIR', 'MOBILE'},"
                 " Defense = {MaxHealth = 100}, SizeX = 2, SizeZ = 2,"
                 " Footprint = {SizeX = 2, SizeZ = 2},"
                 " Air = {CanFly = true, Winged = true, MaxAirspeed = 15, KMove = 1,"
                 " KMoveDamping = 1, StartTurnDistance = 5, TurnSpeed = 1.5, AutoLandTime = 1},"
                 " Physics = {MotionType = 'RULEUMT_Air', Elevation = 18}}",
                 "{BlueprintId = 'gunfwd', Categories = {'AIR', 'MOBILE'},"
                 " Defense = {MaxHealth = 100}, SizeX = 2, SizeZ = 2,"
                 " Footprint = {SizeX = 2, SizeZ = 2},"
                 " Air = {CanFly = true, MaxAirspeed = 12, KMove = 0.8, KMoveDamping = 2,"
                 " StartTurnDistance = 5, AutoLandTime = 0, BankForward = true},"
                 " Physics = {MotionType = 'RULEUMT_Air', Elevation = 10}}",
                 "{BlueprintId = 'level', Categories = {'AIR', 'MOBILE'},"
                 " Defense = {MaxHealth = 100}, SizeX = 1, SizeY = 0.2, SizeZ = 1,"
                 " Footprint = {SizeX = 1, SizeZ = 1},"
                 " Air = {CanFly = true, Winged = true, AutoLandTime = 1, BankFactor = 0,"
                 " KLift = 3, KLiftDamping = 2.5, KMove = 1, KMoveDamping = 1, KRoll = 2,"
                 " KRollDamping = 1, KTurn = 1, KTurnDamping = 1.5, LiftFactor = 7,"
                 " MaxAirspeed = 15, StartTurnDistance = 5, TurnSpeed = 1.5},"
                 " Physics = {MotionType = 'RULEUMT_Air', Elevation = 18}}",
                 "{BlueprintId = 'uea0102', Categories = {'AIR', 'MOBILE'},"
                 " Defense = {MaxHealth = 100}, SizeX = 1, SizeY = 0.2, SizeZ = 1,"
                 " Footprint = {SizeX = 1, SizeZ = 1},"
                 " Air = {CanFly = true, Winged = true, AutoLandTime = 1, BankFactor = 2,"
                 " BankForward = false, KLift = 3, KLiftDamping = 2.5, KMove = 1,"
                 " KMoveDamping = 1, KRoll = 2, KRollDamping = 1, KTurn = 1,"
                 " KTurnDamping = 1.5, LiftFactor = 7, MaxAirspeed = 15, MinAirspeed = 10,"
                 " StartTurnDistance = 5, TurnSpeed = 1.5},"
                 " Physics = {MotionType = 'RULEUMT_Air', Elevation = 18}}",
                 "{BlueprintId = 'bomber', Categories = {'AIR', 'MOBILE'},"
                 " Defense = {MaxHealth = 100}, SizeX = 1, SizeZ = 1,"
                 " Footprint = {SizeX = 1, SizeZ = 1},"
                 " Air = {CanFly = true, Winged = true, MaxAirspeed = 15, TurnSpeed = 0.7},"
                 " Physics = {MotionType = 'RULEUMT_Air', Elevation = 18}}",
                 "{BlueprintId = 'ferry', Categories = {'AIR', 'MOBILE', 'TRANSPORTATION'},"
                 " Defense = {MaxHealth = 100}, SizeX = 2, SizeZ = 2,"
                 " Footprint = {SizeX = 2, SizeZ = 2},"
                 " Air = {CanFly = true, MaxAirspeed = 10, AutoLandTime = 1, StartTurnDistance = 5,"
                 " TransportHoverHeight = 3},"
                 " Physics = {MotionType = 'RULEUMT_Air', Elevation = 10}}",
                 "{BlueprintId = 'pad', Categories = {'STRUCTURE', 'AIRSTAGINGPLATFORM'},"
                 " Defense = {MaxHealth = 100}, SizeX = 2, SizeZ = 2,"
                 " Footprint = {SizeX = 2, SizeZ = 2}, Physics = {MotionType = 'RULEUMT_None'}}",
                 "{BlueprintId = 'tank', Categories = {'LAND', 'MOBILE'},"
                 " Defense = {MaxHealth = 100}, SizeX = 1, SizeZ = 1,"
                 " Footprint = {SizeX = 1, SizeZ = 1}, Physics = {MotionType = 'RULEUMT_Land'}}",
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
        for (const char* id : {"gunship", "gun1", "fighter", "gunfwd", "level", "uea0102", "bomber",
                               "ferry", "pad", "tank"}) {
            lua_pushstring(L, id);
            lua_getglobal(L, "Plain");
            lua_rawset(L, -3);
        }
        lua_rawset(L, LUA_REGISTRYINDEX);
        lua_settop(L, 0);
    }

    Unit& make(const char* bp, f32 x, f32 z, int army = 1) {
        REQUIRE(state
                    .do_string("made = CreateUnit('" + std::string(bp) + "', " +
                               std::to_string(army) + ", " + std::to_string(x) + ", 10, " +
                               std::to_string(z) + ")")
                    .ok());
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

    static void move(Unit& u, f32 x, f32 z, bool clear = true) {
        osc::sim::UnitCommand cmd;
        cmd.type = osc::sim::CommandType::Move;
        cmd.target_pos = {x, 10.0f, z};
        u.push_command(cmd, clear);
    }

    static f32 flat_speed(const osc::sim::Vector3& a, const osc::sim::Vector3& b) {
        const f32 dx = b.x - a.x;
        const f32 dz = b.z - a.z;
        return std::sqrt(dx * dx + dz * dz) * 10.0f;
    }
};

} // namespace

TEST_CASE("CalcAirMovementDampingFactor: KMove at top speed, more toward KMoveDamping below it",
          "[air_motion]") {
    CHECK(osc::sim::air_move_damping(20.0f, 12.0f, 0.8f, 2.0f) == Approx(0.8f));
    CHECK(osc::sim::air_move_damping(8.0f, 12.0f, 0.8f, 2.0f) == Approx(1.5f));
    CHECK(osc::sim::air_move_damping(3.0f, 12.0f, 0.8f, 2.0f) == Approx(2.0f));
    CHECK(osc::sim::air_move_damping(0.2f, 12.0f, 0.8f, 2.0f) == Approx(2.0f));
}

TEST_CASE("ComputeAirControl's level step: KMove x the move against the damping", "[air_motion]") {
    const osc::sim::AirMoveStep step =
        osc::sim::air_move_step({12.0f, 0.0f, 0.0f}, {4.0f, 0.0f, 0.0f}, 0.8f, 2.0f, 0.1f);
    CHECK(step.velocity.x == Approx(9.92f));
    CHECK(step.move.x == Approx(1.096f));
    CHECK(step.velocity.z == Approx(0.0f));
}

TEST_CASE("An aircraft brakes onto the end of its move rather than stopping in one tick",
          "[air_motion]") {
    World w;
    Unit& gunship = w.make("gunship", 20.0f, 128.0f);
    World::move(gunship, 120.0f, 128.0f);
    f32 top = 0.0f;
    f32 sharpest = 0.0f;
    f32 last = 0.0f;
    int ticks = 0;
    for (; ticks < 300 && !gunship.command_queue().empty(); ++ticks) {
        const auto before = gunship.position();
        w.sim.tick();
        const f32 speed = World::flat_speed(before, gunship.position());
        top = std::max(top, speed);
        sharpest = std::max(sharpest, last - speed);
        last = speed;
    }
    REQUIRE(ticks < 300);
    CHECK(top > 11.0f);
    CHECK(sharpest < 2.0f);
    CHECK(last < 1.0f);
    CHECK(std::abs(gunship.position().x - 120.0f) <= 0.25f);
}

TEST_CASE("A gunship brakes onto the centre of its goal's cell as retail's does", "[air_motion]") {
    // A probe of the retail game (UEA0203 on SCMP_009): its x each tick from 11.98 a second.
    const f32 retail[] = {1.1824f, 2.3234f, 3.4034f,  4.4042f,  5.3098f,  6.1076f, 6.7964f,
                          7.3885f, 7.8980f, 8.3371f,  8.7161f,  9.0437f,  9.3274f, 9.5735f,
                          9.7873f, 9.9737f, 10.1363f, 10.2786f, 10.4034f, 10.5131f};
    constexpr int kRetailClose = 40;
    World w;
    Unit& gunship = w.make("gun1", 100.0f, 128.5f);
    gunship.set_position({108.9213f, 20.0f, 128.5f});
    gunship.set_current_altitude(10.0f);
    gunship.set_heading(1.5707964f);
    gunship.set_air_velocity({11.982f, 0.0f, 0.0f});
    World::move(gunship, 120.0f, 128.0f);
    int closed = -1;
    for (int t = 1; t <= 60 && closed < 0; ++t) {
        w.sim.tick();
        if (t <= 20) {
            CHECK(gunship.position().x - 108.9213f == Approx(retail[t - 1]).margin(0.01f));
        }
        if (gunship.command_queue().empty()) {
            closed = t;
        }
    }
    CHECK(std::abs(closed - kRetailClose) <= 1);
    CHECK(gunship.position().x == Approx(120.264f).margin(0.02f));
    CHECK(gunship.position().z == Approx(128.5f).margin(0.01f));
}

TEST_CASE("An aircraft with a move queued after its move flies through its goal at speed",
          "[air_motion]") {
    World w;
    Unit& gunship = w.make("gunship", 20.0f, 128.0f);
    World::move(gunship, 80.0f, 128.0f);
    World::move(gunship, 80.0f, 200.0f, false);
    f32 slowest = 1e9f;
    bool climbed = false;
    for (int t = 0; t < 400 && gunship.command_queue().size() > 1; ++t) {
        const auto before = gunship.position();
        w.sim.tick();
        const f32 speed = World::flat_speed(before, gunship.position());
        climbed = climbed || speed > 11.0f;
        if (climbed) {
            slowest = std::min(slowest, speed);
        }
    }
    REQUIRE(gunship.command_queue().size() == 1);
    CHECK(slowest > 10.0f);
}

TEST_CASE("An aircraft whose move ends at speed holds where it would be a second on, and lands "
          "there",
          "[air_motion]") {
    World w;
    Unit& fighter = w.make("fighter", 20.0f, 128.0f);
    fighter.set_heading(1.5707964f);
    fighter.set_orientation(osc::sim::euler_to_quat(1.5707964f, 0.0f, 0.0f));
    World::move(fighter, 60.0f, 128.0f);
    osc::sim::Vector3 held{};
    std::vector<f32> drops;
    for (int t = 0; t < 600 && fighter.layer() == "Air"; ++t) {
        const f32 y = fighter.position().y;
        const bool moving = !fighter.command_queue().empty();
        w.sim.tick();
        if (moving && fighter.command_queue().empty()) {
            held = fighter.position();
            held.x += fighter.air_velocity().x;
            held.z += fighter.air_velocity().z;
        }
        if (fighter.has_unit_state("MovingDown")) {
            drops.push_back(y - fighter.position().y);
        }
    }
    REQUIRE(fighter.layer() == "Land");
    REQUIRE(drops.size() > 2);
    const f32 steepest = *std::max_element(drops.begin(), drops.end());
    CHECK(held.x > 61.0f);
    CHECK(fighter.position().x == Approx(held.x).margin(1.0f));
    CHECK(fighter.position().z == Approx(held.z).margin(1.0f));
    CHECK(drops.front() < 0.1f);
    CHECK(steepest > 0.3f);
    CHECK(drops.back() < steepest / 3.0f);
    CHECK(fighter.position().y - 10.0f < 0.1f);
}


TEST_CASE("A winged aircraft's turn builds under KTurn toward KTurn x TurnSpeed / KTurnDamping",
          "[air_motion]") {
    World w;
    Unit& a = w.make("uea0102", 30.0f, 128.0f);
    World::move(a, 30.0f, 20.0f);
    f32 first = 0.0f;
    f32 fastest = 0.0f;
    f32 before = a.heading();
    for (int t = 0; t < 30; ++t) {
        w.sim.tick();
        f32 turn = std::abs(a.heading() - before);
        turn = std::min(turn, 6.2831855f - turn);
        if (t == 0) {
            first = turn;
        }
        fastest = std::max(fastest, turn);
        before = a.heading();
    }
    CHECK(first == Approx(0.0075f).margin(0.001f));
    CHECK(fastest > 0.09f);
    CHECK(fastest < 0.115f);
}

TEST_CASE("An aircraft without Air.TurnSpeed turns at Moho's default of 1 radian a second",
          "[air_motion]") {
    World w;
    CHECK(w.make("gunship", 20.0f, 128.0f).turn_rate_rad() == 1.0f);
    CHECK(w.make("bomber", 40.0f, 128.0f).turn_rate_rad() == 0.7f);
}

TEST_CASE("A winged aircraft banks into its turn by its BankFactor", "[air_motion]") {
    World w;
    Unit& banked = w.make("uea0102", 30.0f, 128.0f);
    Unit& level = w.make("level", 30.0f, 60.0f);
    World::move(banked, 120.0f, 128.0f);
    World::move(level, 120.0f, 60.0f);
    f32 deepest = 0.0f;
    f32 flattest = 0.0f;
    for (int t = 0; t < 20; ++t) {
        w.sim.tick();
        deepest = std::max(deepest, osc::sim::quat_rotate(banked.orientation(), {0, 1, 0}).x);
        flattest =
            std::max(flattest, std::abs(osc::sim::quat_rotate(level.orientation(), {0, 1, 0}).x));
    }
    CHECK(deepest > 0.3f);
    CHECK(flattest < 0.05f);
}

TEST_CASE("A hovering aircraft leans into its change of velocity, forward only with BankForward",
          "[air_motion]") {
    World w;
    Unit& plain = w.make("gunship", 30.0f, 60.0f);
    Unit& forward = w.make("gunfwd", 60.0f, 60.0f);
    World::move(plain, 30.0f, 200.0f);
    World::move(forward, 60.0f, 200.0f);
    f32 plain_lean = 0.0f;
    f32 forward_lean = 0.0f;
    for (int t = 0; t < 10; ++t) {
        w.sim.tick();
        plain_lean =
            std::max(plain_lean, std::abs(osc::sim::quat_rotate(plain.orientation(), {0, 1, 0}).z));
        forward_lean =
            std::max(forward_lean, osc::sim::quat_rotate(forward.orientation(), {0, 1, 0}).z);
    }
    CHECK(plain_lean < 0.01f);
    CHECK(forward_lean > 0.2f);

    const osc::sim::AirAxes side = osc::sim::hover_axes(osc::sim::Quaternion{}, {1.0f, 0.0f, 0.0f},
                                                        0.5f, false, 1.0f, {0.0f, 0.0f, 1.0f});
    CHECK(side.up.x == Approx(0.5f));
    CHECK(side.up.y == Approx(0.49f));
    const osc::sim::AirAxes ahead = osc::sim::hover_axes(osc::sim::Quaternion{}, {0.0f, 0.0f, 1.0f},
                                                         0.5f, false, 1.0f, {0.0f, 0.0f, 1.0f});
    CHECK(ahead.up.z == Approx(0.0f));
}

TEST_CASE("An aircraft stopped while turning holds where its turn would carry it in a second",
          "[air_motion]") {
    World w;
    Unit& a = w.make("uea0102", 30.0f, 128.0f);
    a.set_orientation(osc::sim::euler_to_quat(1.5707964f, 0.0f, 0.0f));
    a.set_air_velocity({10.0f, 0.0f, 0.0f});
    a.set_air_velocity({10.0f, 0.0f, 0.0f});
    a.set_air_turn_rate(1.0f);
    CHECK(a.air_turn_rate() == Approx(1.0f));
    const osc::sim::Vector3 at = a.air_stop_point();
    f32 x = 30.0f;
    f32 z = 128.0f;
    for (int step = 1; step <= 10; ++step) {
        x += std::cos(0.1f * static_cast<f32>(step));
        z -= std::sin(0.1f * static_cast<f32>(step));
    }
    CHECK(at.x == Approx(x).margin(0.001f));
    CHECK(at.z == Approx(z).margin(0.001f));
}

TEST_CASE("Near its goal a winged aircraft turns its nose to the way its order sent it",
          "[air_motion]") {
    World w;
    Unit& a = w.make("uea0102", 30.0f, 128.0f);
    a.set_air_velocity({0.0f, 0.0f, 15.0f});
    World::move(a, 60.0f, 128.0f);
    for (int t = 0; t < 600 && a.layer() == "Air"; ++t) {
        w.sim.tick();
    }
    REQUIRE(a.layer() == "Land");
    CHECK(a.heading() == Approx(1.5707964f).margin(0.05f));
}

TEST_CASE("An aircraft breaking off its patrol to attack flies on at its speed, not from rest",
          "[air_motion]") {
    World w(true);
    Unit& a = w.make("uea0102", 30.0f, 128.0f);
    auto gun = std::make_unique<osc::sim::Weapon>();
    gun->max_range = 25.0f;
    gun->damage = 10.0f;
    a.add_weapon(std::move(gun));
    w.make("gunship", 150.0f, 138.0f, 2);
    osc::sim::UnitCommand patrol;
    patrol.type = osc::sim::CommandType::Patrol;
    patrol.target_pos = {230.0f, 10.0f, 128.0f};
    a.push_command(patrol, true);
    f32 cruise = 0.0f;
    std::vector<f32> after;
    for (int t = 0; t < 300 && after.size() < 5; ++t) {
        a.weapons().front()->fire_clock = 1000000;
        const auto before = a.position();
        w.sim.tick();
        const f32 speed = World::flat_speed(before, a.position());
        if (!after.empty() || a.command_queue().front().from_patrol) {
            after.push_back(speed);
        } else {
            cruise = speed;
        }
    }
    REQUIRE(after.size() == 5);
    CHECK(cruise > 14.0f);
    CHECK(*std::min_element(after.begin(), after.end()) > cruise - 3.0f);
}

TEST_CASE("An aircraft whose ground attack goes round the queue flies on through that tick",
          "[air_motion]") {
    for (const char* id : {"bomber", "gunship"}) {
        INFO(id);
        World w;
        Unit& a = w.make(id, 30.0f, 128.0f);
        auto gun = std::make_unique<osc::sim::Weapon>();
        gun->max_range = 20.0f;
        gun->damage = 10.0f;
        a.add_weapon(std::move(gun));
        osc::sim::UnitCommand attack;
        attack.type = osc::sim::CommandType::Attack;
        attack.target_pos = {120.0f, 10.0f, 128.0f};
        a.push_command(attack, true);
        World::move(a, 200.0f, 220.0f, false);
        osc::sim::Weapon& gun_ref = *a.weapons().front();
        f32 last = 0.0f;
        f32 at_turn = 0.0f;
        f32 sharpest = 0.0f;
        int after = -1;
        for (int t = 0; t < 600 && after < 5; ++t) {
            gun_ref.fire_clock = 1000000;
            if (a.command_queue().front().engaged) {
                gun_ref.shots_at_target = gun_ref.attack_ground_tries;
            }
            const auto before = a.position();
            w.sim.tick();
            const f32 speed = World::flat_speed(before, a.position());
            if (after < 0 && a.command_queue().front().type == osc::sim::CommandType::Move) {
                after = 0;
                at_turn = last;
            }
            if (after >= 0) {
                sharpest = std::max(sharpest, last - speed);
                ++after;
            }
            last = speed;
        }
        REQUIRE(after == 5);
        CHECK(at_turn > 5.0f);
        CHECK(sharpest < 4.0f);
    }
}

TEST_CASE("An aircraft the engine held docked or hovering flies again from rest", "[air_motion]") {
    World w;
    lua_State* L = w.state.raw();
    SECTION("docked on a platform") {
        Unit& a = w.make("gunship", 30.0f, 128.0f);
        Unit& pad = w.make("pad", 60.0f, 60.0f);
        World::move(a, 200.0f, 128.0f);
        for (int t = 0; t < 40; ++t) {
            w.sim.tick();
        }
        REQUIRE(std::abs(a.air_velocity().x) > 8.0f);
        a.attach_to_transport(&pad, w.sim.entity_registry(), L);
        REQUIRE(a.transport_id() == pad.entity_id());
        for (int t = 0; t < 5; ++t) {
            w.sim.tick();
        }
        pad.detach_cargo({a.entity_id()}, w.sim.entity_registry(), L, w.sim.terrain());
        const auto before = a.position();
        w.sim.tick();
        CHECK(World::flat_speed(before, a.position()) < 2.0f);
    }
    SECTION("hovering low with its cargo") {
        Unit& ferry = w.make("ferry", 40.0f, 40.0f);
        Unit& tank = w.make("tank", 44.0f, 40.0f);
        tank.attach_to_transport(&ferry, w.sim.entity_registry(), L);
        for (int t = 0; t < 300 && ferry.vert_event() != "Hover"; ++t) {
            if (ferry.has_unit_state("MovingDown")) {
                ferry.set_air_spin({0.0f, 3.0f, 0.0f});
            }
            w.sim.tick();
        }
        REQUIRE(ferry.vert_event() == "Hover");
        World::move(ferry, 100.0f, 40.0f);
        const auto before = ferry.orientation();
        w.sim.tick();
        const auto after = ferry.orientation();
        const f32 dot = std::abs(before.x * after.x + before.y * after.y + before.z * after.z +
                                 before.w * after.w);
        CHECK(2.0f * std::acos(std::min(dot, 1.0f)) < 0.05f);
    }
}

TEST_CASE("An aircraft's weapon takes its ordered target only once the attack engages",
          "[air_motion]") {
    World w(true);
    Unit& a = w.make("uea0102", 30.0f, 128.0f);
    auto gun = std::make_unique<osc::sim::Weapon>();
    gun->max_range = 25.0f;
    gun->damage = 10.0f;
    a.add_weapon(std::move(gun));
    const Unit& tank = w.make("tank", 150.0f, 128.0f, 2);
    osc::sim::UnitCommand attack;
    attack.type = osc::sim::CommandType::Attack;
    attack.target_id = tank.entity_id();
    a.push_command(attack, true);
    const osc::sim::Weapon& weapon = *a.weapons().front();
    bool early = false;
    for (int t = 0; t < 300 && !a.command_queue().front().engaged; ++t) {
        a.weapons().front()->fire_clock = 1000000;
        w.sim.tick();
        early = early ||
                (!a.command_queue().front().engaged && weapon.target_entity_id == tank.entity_id());
    }
    REQUIRE(a.command_queue().front().engaged);
    CHECK_FALSE(early);
    CHECK(weapon.target_entity_id == 0);
    a.weapons().front()->fire_clock = 1000000;
    w.sim.tick();
    CHECK(weapon.target_entity_id == tank.entity_id());
}

TEST_CASE("An idle aircraft whose weapon picks a target flies its attack run that tick",
          "[air_motion]") {
    World w(true);
    Unit& a = w.make("uea0102", 30.0f, 128.0f);
    auto gun = std::make_unique<osc::sim::Weapon>();
    gun->max_range = 25.0f;
    gun->damage = 10.0f;
    gun->target_check_period = 1;
    gun->auto_initiate_attack_command = true;
    a.add_weapon(std::move(gun));
    for (int t = 0; t < 5; ++t) {
        a.weapons().front()->fire_clock = 1000000;
        w.sim.tick();
    }
    REQUIRE(a.command_queue().empty());
    REQUIRE_FALSE(a.air_combat().flying);
    w.make("tank", 45.0f, 128.0f, 2);
    a.weapons().front()->fire_clock = 1000000;
    w.sim.tick();
    REQUIRE_FALSE(a.command_queue().empty());
    CHECK(a.command_queue().front().type == osc::sim::CommandType::Attack);
    CHECK(a.air_combat().flying);
}

TEST_CASE("An attack that engages makes the weapons drop their target, then take the order's",
          "[air_motion]") {
    World w(true);
    lua_State* L = w.state.raw();
    Unit& a = w.make("uea0102", 30.0f, 128.0f);
    auto gun = std::make_unique<osc::sim::Weapon>();
    gun->max_range = 25.0f;
    gun->damage = 10.0f;
    REQUIRE(w.state
                .do_string("heard = {} gun = {"
                           " OnGotTarget = function(self) table.insert(heard, 'got') end,"
                           " OnLostTarget = function(self) table.insert(heard, 'lost') end}")
                .ok());
    lua_getglobal(L, "gun");
    gun->lua_table_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    gun->script_class = true;
    a.add_weapon(std::move(gun));
    const Unit& tank = w.make("tank", 45.0f, 128.0f, 2);
    const osc::sim::Weapon& weapon = *a.weapons().front();
    for (int t = 0; t < 20 && weapon.target_entity_id != tank.entity_id(); ++t) {
        a.weapons().front()->fire_clock = 1000000;
        w.sim.tick();
    }
    REQUIRE(weapon.target_entity_id == tank.entity_id());
    REQUIRE(w.state.do_string("heard = {}").ok());
    osc::sim::UnitCommand attack;
    attack.type = osc::sim::CommandType::Attack;
    attack.target_id = tank.entity_id();
    a.push_command(attack, true);
    for (int t = 0; t < 100 && !a.command_queue().front().engaged; ++t) {
        a.weapons().front()->fire_clock = 1000000;
        w.sim.tick();
    }
    REQUIRE(a.command_queue().front().engaged);
    REQUIRE(w.state.do_string("assert(table.concat(heard, ' ') == 'lost')").ok());
    a.weapons().front()->fire_clock = 1000000;
    w.sim.tick();
    REQUIRE(w.state.do_string("assert(table.concat(heard, ' ') == 'lost got')").ok());
    CHECK(weapon.target_entity_id == tank.entity_id());
}

TEST_CASE("An aircraft whose move hands over to its next order as it runs dry flies that tick "
          "one step, the order's, slowed",
          "[air_motion]") {
    const char* id = "uea0102";
    f32 top = 15.0f;
    osc::sim::UnitCommand next;
    next.type = osc::sim::CommandType::Attack;
    SECTION("a winged aircraft's attack run") {}
    SECTION("a gunship circling its attack's target") {
        id = "gunship";
        top = 12.0f;
    }
    SECTION("a gunship flying its guard winged") {
        id = "gunship";
        top = 12.0f;
        next.type = osc::sim::CommandType::Guard;
        next.target_pos = {140.0f, 10.0f, 200.0f};
    }
    const auto start = [&](World& w) -> Unit& {
        Unit& a = w.make(id, 30.0f, 128.0f);
        auto gun = std::make_unique<osc::sim::Weapon>();
        gun->max_range = 25.0f;
        gun->damage = 10.0f;
        a.add_weapon(std::move(gun));
        World::move(a, 100.0f, 128.0f);
        osc::sim::UnitCommand order = next;
        if (order.type == osc::sim::CommandType::Attack) {
            order.target_id = w.make("tank", 105.0f, 128.0f, 2).entity_id();
        }
        a.push_command(order, false);
        return a;
    };
    World handed(true);
    World alone(true);
    REQUIRE(
        handed.state.do_string("Plain.OnRunOutOfFuel = function(self) self:SetSpeedMult(0.25) end")
            .ok());
    Unit& plane = start(handed);
    Unit& reference = start(alone);
    int ticks = 0;
    for (; ticks < 300 && plane.command_queue().size() == 2; ++ticks) {
        plane.weapons().front()->fire_clock = 1000000;
        reference.weapons().front()->fire_clock = 1000000;
        if (plane.navigator().ended()) {
            for (Unit* u : {&plane, &reference}) {
                u->set_fuel_use_time(1.0f);
                u->set_fuel_ratio(0.05f);
            }
            reference.set_speed_mult(0.25f);
            const osc::sim::UnitCommand order = reference.command_queue().back();
            reference.push_command(order, true);
        }
        const auto before = plane.position();
        handed.sim.tick();
        alone.sim.tick();
        if (plane.command_queue().size() == 1) {
            REQUIRE(plane.speed_mult() == 0.25f);
            CHECK(plane.air_combat().flying);
            CHECK(World::flat_speed(before, plane.position()) <= top + 1e-3f);
        }
        CHECK(plane.position().x == reference.position().x);
        CHECK(plane.position().y == reference.position().y);
        CHECK(plane.position().z == reference.position().z);
    }
    CHECK(ticks < 300);
}

TEST_CASE("An aircraft docking at a staging platform is taken aboard on the tick after the step "
          "that settles it over its slot",
          "[air_motion]") {
    osc::sim::BoneData skeleton;
    skeleton.bones.push_back({"root", {}, {}, -1, {}, {}});
    osc::sim::BoneInfo point;
    point.name = "Attachpoint01";
    point.parent_index = 0;
    point.world_position = {0.0f, 1.0f, 0.0f};
    skeleton.bones.push_back(point);
    World w;
    Unit& pad = w.make("pad", 80.0f, 128.0f);
    pad.set_bone_data(&skeleton);
    Unit& plane = w.make("gunship", 30.0f, 128.0f);
    plane.set_air_class(true);
    osc::sim::UnitCommand dock;
    dock.type = osc::sim::CommandType::Dock;
    dock.target_id = pad.entity_id();
    dock.target_pos = pad.position();
    plane.push_command(dock, true);
    int settled = -1;
    int aboard = -1;
    for (int t = 0; t < 400; ++t) {
        w.sim.tick();
        if (plane.transport_id() == pad.entity_id()) {
            aboard = t;
            break;
        }
        const osc::sim::TransportSlots* slots = pad.built_transport_slots();
        const auto* slot = slots ? slots->slot_of(plane.entity_id()) : nullptr;
        if (settled < 0 && slot) {
            const auto at = pad.bone_world_position(slot->bone);
            const auto& p = plane.position();
            const f32 floor = plane.air_floor(w.sim.terrain(), p.x, p.z);
            if (std::hypot(at.x - p.x, at.z - p.z) <= 0.5f &&
                plane.current_altitude() == at.y - floor && std::cos(plane.heading()) > 0.95f) {
                settled = t;
            }
        }
    }
    REQUIRE(aboard >= 0);
    CHECK(aboard == settled + 1);
}
