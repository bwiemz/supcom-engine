// An attack order on a unit, as Moho's CUnitAttackTargetTask carries it out
// (faf-re, checked in the Steam exe): the weapon it attacks with is the first
// of the unit's weapons that can attack the target (CAiAttackerImpl::
// GetTargetWeapon), and the unit closes until the target is within that
// weapon's range (TaskTick's TargetIsWithinWeaponAttackRange). A longer
// weapon that can't hit the target, or comes later, doesn't stop it short.

#include <catch2/catch_test_macros.hpp>

#include "blueprints/footprint.hpp"
#include "map/heightmap.hpp"
#include "map/terrain.hpp"
#include "sim/manipulator.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/unit_command.hpp"
#include "sim/weapon.hpp"
#include "support/tick_until.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

using osc::f32;
using osc::sim::CommandType;
using osc::sim::SimState;
using osc::sim::Unit;
using osc::sim::UnitCommand;
using osc::sim::Weapon;
using osc::test::tick_until;

namespace {

struct LuaGuard {
    lua_State* L = lua_open();
    ~LuaGuard() { lua_close(L); }
};

void land(SimState& sim) {
    std::vector<osc::u16> heights(129 * 129, 1000);
    osc::map::Heightmap hm(128, 128, 1.0f / 128.0f, std::move(heights));
    sim.set_terrain(std::make_unique<osc::map::Terrain>(std::move(hm), 0.0f, false));
    sim.build_pathfinding_grid();
    sim.add_army("ARMY_1", "ARMY_1");
    sim.add_army("ARMY_2", "ARMY_2");
    sim.set_fog_of_war("none");
    sim.set_victory_condition("sandbox");
}

struct Arm {
    f32 range;
    osc::u8 layers; ///< FireTargetLayerCaps
};

/// A land unit with these weapons, in this order.
Unit* attacker(SimState& sim, f32 x, f32 z, std::initializer_list<Arm> arms) {
    auto u = std::make_unique<Unit>();
    u->set_army(0);
    u->set_max_speed(5.0f);
    u->set_motion_type("RULEUMT_Land");
    u->set_position({x, 7.8f, z});
    u->set_max_health(100.0f);
    u->set_health(100.0f);
    Unit::Drive drive;
    drive.max_accel = 50.0f;
    u->set_drive(drive);
    for (const Arm& a : arms) {
        auto w = std::make_unique<Weapon>();
        w->target_priorities = {osc::sim::parse_category_list("ALLUNITS")};
        w->max_range = a.range;
        w->fire_target_layer_caps = a.layers;
        w->damage = 10.0f;
        w->weapon_index = static_cast<osc::i32>(u->weapons().size());
        u->add_weapon(std::move(w));
    }
    auto* raw = u.get();
    sim.entity_registry().register_entity(std::move(u));
    return raw;
}

/// An enemy on the land, standing still.
Unit* foe(SimState& sim, f32 x, f32 z) {
    auto u = std::make_unique<Unit>();
    u->set_army(1);
    u->set_position({x, 7.8f, z});
    u->set_max_health(100.0f);
    u->set_health(100.0f);
    auto* raw = u.get();
    sim.entity_registry().register_entity(std::move(u));
    return raw;
}

void attack(Unit& u, const Unit& target) {
    UnitCommand c;
    c.type = CommandType::Attack;
    c.target_id = target.entity_id();
    c.target_pos = target.position();
    u.push_command(c, true);
}

f32 apart(const Unit& a, const Unit& b) {
    return std::hypot(a.position().x - b.position().x, a.position().z - b.position().z);
}

/// One tick, its weapons held: the tests read their targets, not their shots.
void tick(SimState& sim, Unit& u) {
    for (const auto& w : u.weapons()) {
        w->fire_clock = 1000000;
    }
    sim.tick();
}

/// It has stopped, its attack in reach, weapon `i` on the target.
bool at_rest_on(const Unit& u, size_t i, const Unit& target) {
    return !u.command_queue().empty() && u.command_queue().front().engaged &&
           u.ground_speed() == 0.0f && u.weapons()[i]->target_entity_id == target.entity_id();
}

/// Gives `w` a script that notes in the global `heard` what it hears of its target.
void listen(lua_State* L, Weapon& w) {
    REQUIRE(lua_dostring(L, "heard = ''"
                            " gun = {OnGotTarget = function() heard = heard .. 'got ' end,"
                            " OnLostTarget = function() heard = heard .. 'lost ' end}") == 0);
    lua_pushstring(L, "gun");
    lua_gettable(L, LUA_GLOBALSINDEX);
    w.lua_table_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    w.script_class = true;
}

std::string heard(lua_State* L) {
    lua_pushstring(L, "heard");
    lua_gettable(L, LUA_GLOBALSINDEX);
    std::string text = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
    lua_pop(L, 1);
    return text;
}

// Layer caps, as layer_to_bit's: Land | Water | Seabed, and Air.
constexpr osc::u8 kSurface = 0x01 | 0x02 | 0x04;
constexpr osc::u8 kAir = 0x10;
constexpr f32 kDeg = 3.14159265358979f / 180.0f;

} // namespace

TEST_CASE("An attack closes to its first weapon that can hit the target, past a longer one",
          "[attack-reach]") {
    // A destroyer's order against a sub, or a frigate's against a ship: its
    // anti-air reaches 30, its gun 10. Before, it stopped at 30, where
    // nothing it had could hit the target.
    LuaGuard g;
    SimState sim(g.L, nullptr);
    land(sim);
    Unit* u = attacker(sim, 20.0f, 64.0f, {{30.0f, kAir}, {10.0f, kSurface}});
    Unit* target = foe(sim, 70.0f, 64.0f);
    attack(*u, *target);
    tick_until([&] { tick(sim, *u); }, "it comes to rest with its gun on the target", 600,
               [&] { return at_rest_on(*u, 1, *target); });
    CHECK(apart(*u, *target) <= 10.0f);
    CHECK(apart(*u, *target) > 5.0f);
    CHECK_FALSE(u->weapons()[0]->target_entity_id);
}

TEST_CASE("The first weapon that can hit the target decides, though a later one reaches further",
          "[attack-reach]") {
    // GetTargetWeapon takes the first, in blueprint order, not the longest.
    LuaGuard g;
    SimState sim(g.L, nullptr);
    land(sim);
    Unit* u = attacker(sim, 20.0f, 64.0f, {{10.0f, kSurface}, {25.0f, kSurface}});
    Unit* target = foe(sim, 70.0f, 64.0f);
    attack(*u, *target);
    tick_until([&] { tick(sim, *u); }, "it comes to rest with its first gun on the target", 600,
               [&] { return at_rest_on(*u, 0, *target); });
    CHECK(apart(*u, *target) <= 10.0f);
    CHECK(apart(*u, *target) > 5.0f);
}

TEST_CASE("A target in range but out of its weapon's arc is out of reach: the attack engages once "
          "its unit has turned the arc onto it",
          "[attack-reach]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    land(sim);
    Unit* u = attacker(sim, 20.0f, 64.0f, {{30.0f, kSurface}});
    Unit::Drive drive = u->drive();
    drive.turn_rate = 90.0f * kDeg;
    u->set_drive(drive);
    u->weapons()[0]->heading_arc_range = 30.0f;
    Unit* target = foe(sim, 40.0f, 64.0f);
    attack(*u, *target);
    tick_until([&] { tick(sim, *u); }, "its attack engages", 60,
               [&] { return u->command_queue().front().engaged; });
    CHECK(u->weapons()[0]->in_heading_arc(*u, target->position()));
}

TEST_CASE("Inside its weapon's minimum range, a unit backs off from its target and looks again 9 "
          "ticks on",
          "[attack-reach]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    land(sim);
    Unit* u = attacker(sim, 20.0f, 64.0f, {{30.0f, kSurface}});
    u->weapons()[0]->min_range = 6.0f;
    Unit* target = foe(sim, 20.0f, 66.0f);
    attack(*u, *target);
    int ticks = 0;
    tick_until(
        [&] {
            tick(sim, *u);
            ++ticks;
        },
        "its attack engages", 200, [&] { return u->command_queue().front().engaged; });
    CHECK(apart(*u, *target) > 6.0f);
    CHECK(ticks >= 11);
}

TEST_CASE("Closing in, a unit keeps its goal till a MOBILE target is more than 10 off where it was "
          "when the goal was set",
          "[attack-reach]") {
    for (const bool mobile : {true, false}) {
        CAPTURE(mobile);
        LuaGuard g;
        SimState sim(g.L, nullptr);
        land(sim);
        Unit* u = attacker(sim, 20.0f, 64.0f, {{10.0f, kSurface}});
        osc::blueprints::UnitFootprints footprints;
        footprints.main.size_x = 1;
        footprints.main.size_z = 1;
        footprints.main.caps = osc::blueprints::occupancy::kLand;
        u->set_footprints(footprints);
        Unit* target = foe(sim, 100.0f, 64.0f);
        if (mobile) {
            target->add_category("MOBILE");
        }
        attack(*u, *target);
        tick(sim, *u);
        REQUIRE(u->navigator().busy());
        const osc::sim::Vector3 first = u->navigator().goal();
        CHECK(first.x == 95.0f);
        CHECK(first.z == 64.0f);
        target->set_position({100.0f, 7.8f, 70.0f});
        target->place_now();
        for (int t = 0; t < 3; ++t) {
            tick(sim, *u);
        }
        CHECK(u->navigator().goal().x == first.x);
        CHECK(u->navigator().goal().z == first.z);
        target->set_position({100.0f, 7.8f, 76.0f});
        target->place_now();
        tick(sim, *u);
        CHECK(u->navigator().goal().x == (mobile ? 100.0f : first.x));
        CHECK(u->navigator().goal().z == (mobile ? 76.0f : first.z));
    }
}

TEST_CASE("A ManualFire weapon with the target inside its minimum range has its unit back off "
          "while no weapon has a solution",
          "[attack-reach]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    land(sim);
    Unit* u = attacker(sim, 20.0f, 64.0f, {{30.0f, kSurface}, {30.0f, kSurface}});
    u->weapons()[0]->heading_arc_range = 30.0f;
    u->weapons()[1]->manual_fire = true;
    u->weapons()[1]->min_range = 6.0f;
    Unit* target = foe(sim, 23.0f, 64.0f);
    attack(*u, *target);
    f32 farthest = 0.0f;
    for (int t = 0; t < 30; ++t) {
        tick(sim, *u);
        farthest = std::max(farthest, apart(*u, *target));
    }
    CHECK(farthest > 4.0f);
}

TEST_CASE("An attack keeps when only a ManualFire weapon can hit the target", "[attack-reach]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    land(sim);
    Unit* u = attacker(sim, 20.0f, 64.0f, {{30.0f, kAir}, {30.0f, kSurface}});
    u->weapons()[1]->manual_fire = true;
    Unit* target = foe(sim, 60.0f, 64.0f);
    attack(*u, *target);
    for (int t = 0; t < 5; ++t) {
        tick(sim, *u);
    }
    CHECK_FALSE(u->command_queue().empty());
}

TEST_CASE("An attack keeps while its unit's only weapon is switched off, and engages once it is on",
          "[attack-reach]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    land(sim);
    Unit* u = attacker(sim, 20.0f, 64.0f, {{30.0f, kSurface}});
    u->weapons()[0]->enabled = false;
    Unit* target = foe(sim, 30.0f, 64.0f);
    attack(*u, *target);
    for (int t = 0; t < 20; ++t) {
        tick(sim, *u);
    }
    REQUIRE_FALSE(u->command_queue().empty());
    CHECK_FALSE(u->command_queue().front().engaged);
    u->weapons()[0]->enabled = true;
    tick_until([&] { tick(sim, *u); }, "its attack engages", 3,
               [&] { return u->command_queue().front().engaged; });
}

TEST_CASE("With no weapon that can hit its target, a DIRECTFIRE unit's attack never engages: it "
          "closes on the target",
          "[attack-reach]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    land(sim);
    Unit* u = attacker(sim, 20.0f, 64.0f, {{30.0f, kAir}});
    u->add_category("DIRECTFIRE");
    Unit* target = foe(sim, 60.0f, 64.0f);
    attack(*u, *target);
    for (int t = 0; t < 120; ++t) {
        tick(sim, *u);
    }
    REQUIRE_FALSE(u->command_queue().empty());
    CHECK_FALSE(u->command_queue().front().engaged);
    CHECK(apart(*u, *target) < 10.0f);
}

TEST_CASE("While its unit attacks a target it can't hit, an AutoInitiateAttackCommand weapon "
          "takes no other until the target is dying, another weapon does",
          "[attack-reach]") {
    for (const bool auto_initiate : {false, true}) {
        CAPTURE(auto_initiate);
        LuaGuard g;
        SimState sim(g.L, nullptr);
        land(sim);
        sim.con_vars().weapon_terrain_blockage_test = false;
        Unit* u = attacker(sim, 20.0f, 64.0f, {{30.0f, kAir}, {10.0f, kSurface}});
        Weapon& aa = *u->weapons()[0];
        aa.auto_initiate_attack_command = auto_initiate;
        aa.target_check_period = 5;
        Unit* target = foe(sim, 25.0f, 64.0f);
        Unit* plane = foe(sim, 100.0f, 100.0f);
        plane->set_layer("Air");
        attack(*u, *target);
        tick_until([&] { tick(sim, *u); }, "its gun takes the target", 10,
                   [&] { return u->weapons()[1]->target_entity_id == target->entity_id(); });
        plane->set_position({20.0f, 17.8f, 74.0f});
        plane->place_now();
        for (int t = 0; t < 10; ++t) {
            tick(sim, *u);
        }
        CHECK((aa.target_entity_id == plane->entity_id()) == !auto_initiate);
        target->begin_dying();
        tick_until([&] { tick(sim, *u); }, "its anti-air takes the plane", 2,
                   [&] { return aa.target_entity_id == plane->entity_id(); });
    }
}

TEST_CASE("A weapon on its attack's target looks again the tick the target leaves its reach",
          "[attack-reach]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    land(sim);
    sim.con_vars().weapon_terrain_blockage_test = false;
    Unit* u = attacker(sim, 20.0f, 64.0f, {{10.0f, kSurface}});
    Weapon& gun = *u->weapons()[0];
    gun.target_check_period = 30;
    Unit* target = foe(sim, 25.0f, 64.0f);
    Unit* other = foe(sim, 100.0f, 100.0f);
    tick(sim, *u);
    REQUIRE(gun.target_entity_id == target->entity_id());
    attack(*u, *target);
    tick_until([&] { tick(sim, *u); }, "the attack engages", 10,
               [&] { return u->command_queue().front().engaged; });
    tick(sim, *u);
    target->set_position({100.0f, 7.8f, 20.0f});
    target->place_now();
    other->set_position({20.0f, 7.8f, 70.0f});
    other->place_now();
    tick_until([&] { tick(sim, *u); }, "its gun takes the other enemy", 1,
               [&] { return gun.target_entity_id == other->entity_id(); });
}

TEST_CASE("A weapon keeps its attack's target out of its reach until it finds another enemy",
          "[attack-reach]") {
    for (const bool ground : {false, true}) {
        CAPTURE(ground);
        LuaGuard g;
        SimState sim(g.L, nullptr);
        land(sim);
        sim.con_vars().weapon_terrain_blockage_test = false;
        Unit* u = attacker(sim, 20.0f, 64.0f, {{10.0f, kSurface}});
        Weapon& gun = *u->weapons()[0];
        gun.target_check_period = 5;
        Unit* target = foe(sim, 25.0f, 64.0f);
        Unit* other = foe(sim, 100.0f, 100.0f);
        UnitCommand c;
        c.type = CommandType::Attack;
        c.target_id = ground ? osc::sim::EntityId() : target->entity_id();
        c.target_pos = target->position();
        u->push_command(c, true);
        const auto on_it = [&] {
            return ground ? gun.has_ground_target && gun.ground_target.x == c.target_pos.x
                          : gun.target_entity_id == target->entity_id();
        };
        tick_until([&] { tick(sim, *u); }, "its gun takes the attack's target", 10, on_it);
        u->set_position({0.0f, 7.8f, 64.0f});
        u->place_now();
        for (int t = 0; t < 12; ++t) {
            tick(sim, *u);
            CHECK(on_it());
        }
        if (!ground) {
            other->set_position({u->position().x, 7.8f, 70.0f});
            other->place_now();
            tick_until([&] { tick(sim, *u); }, "its gun takes the other enemy", 5,
                       [&] { return gun.target_entity_id == other->entity_id(); });
        }
    }
}

TEST_CASE(
    "A structure's attack ends once its weapon has no solution for the target, from the start "
    "or as the target leaves its reach",
    "[attack-reach]") {
    for (const bool ground : {false, true}) {
        for (const bool from_start : {false, true}) {
            CAPTURE(ground, from_start);
            LuaGuard g;
            SimState sim(g.L, nullptr);
            land(sim);
            sim.con_vars().weapon_terrain_blockage_test = false;
            Unit* u = attacker(sim, 20.0f, 64.0f, {{10.0f, kSurface}});
            u->set_motion_type("RULEUMT_None");
            Unit* target = foe(sim, from_start ? 50.0f : 25.0f, 64.0f);
            UnitCommand c;
            c.type = CommandType::Attack;
            c.target_id = ground ? osc::sim::EntityId() : target->entity_id();
            c.target_pos = target->position();
            u->push_command(c, true);
            const auto attacking = [&] { return !u->command_queue().empty(); };
            if (!from_start) {
                for (int t = 0; t < 5; ++t) {
                    tick(sim, *u);
                }
                REQUIRE(attacking());
                u->set_position({0.0f, 7.8f, 64.0f});
                u->place_now();
            }
            tick_until([&] { tick(sim, *u); }, "the attack ends", 2, [&] { return !attacking(); });
            CHECK_FALSE(u->weapons()[0]->has_target());
        }
    }
}

TEST_CASE("An attack's start drops its unit's weapons' targets, their scripts told, and they look "
          "again at once",
          "[attack-reach]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    land(sim);
    sim.con_vars().weapon_terrain_blockage_test = false;
    Unit* u = attacker(sim, 20.0f, 64.0f, {{10.0f, kSurface}});
    Weapon& gun = *u->weapons()[0];
    listen(g.L, gun);
    Unit* first = foe(sim, 25.0f, 64.0f);
    Unit* second = foe(sim, 20.0f, 70.0f);
    tick_until([&] { tick(sim, *u); }, "its gun takes the nearer enemy", 2,
               [&] { return gun.target_entity_id == first->entity_id(); });
    REQUIRE(heard(g.L) == "got ");
    attack(*u, *second);
    tick_until([&] { tick(sim, *u); }, "its gun takes the attack's target", 3,
               [&] { return gun.target_entity_id == second->entity_id(); });
    CHECK(heard(g.L) == "got lost got ");
}

TEST_CASE("A structure's gun takes its attack's target out of reach at once, and drops it as the "
          "attack ends",
          "[attack-reach]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    land(sim);
    sim.con_vars().weapon_terrain_blockage_test = false;
    Unit* u = attacker(sim, 20.0f, 64.0f, {{10.0f, kSurface}});
    u->set_motion_type("RULEUMT_None");
    listen(g.L, *u->weapons()[0]);
    Unit* target = foe(sim, 50.0f, 64.0f);
    attack(*u, *target);
    tick_until([&] { tick(sim, *u); }, "the attack ends", 2,
               [&] { return u->command_queue().empty(); });
    CHECK(heard(g.L) == "got lost ");
}
