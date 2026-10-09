// A stopped surface unit turns its hull to its weapons' work, as Moho's
// CUnitMotion::CalcMoveCommon does (faf-re): toward its first SlavedToBody
// weapon's target with hysteresis (SlavedToBodyArcRange), or AttackAngle off
// it; a parked attack's AttackAngle facing without one. A mobile unit's
// slaved weapon may hold a target outside its heading arc, and fires only
// once it is in it. See ~/.cache/osc-design-drafts/2026-10-05-hull-facing-design.md.

#include <catch2/catch_test_macros.hpp>

#include "core/dmath.hpp"
#include "map/heightmap.hpp"
#include "map/terrain.hpp"
#include "sim/manipulator.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/unit_command.hpp"
#include "sim/weapon.hpp"

extern "C" {
#include <lua.h>
}

#include <cmath>
#include <memory>
#include <string>
#include <vector>

using osc::f32;
using osc::sim::CommandType;
using osc::sim::SimState;
using osc::sim::Unit;
using osc::sim::UnitCommand;
using osc::sim::Vector3;
using osc::sim::Weapon;

namespace {

constexpr f32 kPi = 3.14159265358979f;
constexpr f32 kDeg = kPi / 180.0f;

struct LuaGuard {
    lua_State* L = lua_open();
    ~LuaGuard() { lua_close(L); }
};

void flat(SimState& sim) {
    std::vector<osc::u16> heights(129 * 129, 1000);
    osc::map::Heightmap hm(128, 128, 1.0f / 128.0f, std::move(heights));
    sim.set_terrain(std::make_unique<osc::map::Terrain>(std::move(hm), 0.0f, false));
    sim.build_pathfinding_grid();
    sim.add_army("ARMY_1", "ARMY_1");
    sim.add_army("ARMY_2", "ARMY_2");
    sim.set_fog_of_war("none");
    sim.set_victory_condition("sandbox");
}

/// A land unit facing +Z (heading 0) that turns 90 degrees a second (9 a
/// tick), with one gun of range 40 that never fires (no damage: its target
/// stays as set).
Unit* hull(SimState& sim, f32 x, f32 z, const std::string& motion = "RULEUMT_Land") {
    auto u = std::make_unique<Unit>();
    u->set_army(0);
    u->set_max_speed(motion == "RULEUMT_None" ? 0.0f : 5.0f);
    u->set_motion_type(motion);
    u->set_position({x, 7.8f, z});
    u->set_max_health(100.0f);
    u->set_health(100.0f);
    Unit::Drive drive;
    drive.max_accel = 50.0f;
    drive.max_brake = 50.0f;
    drive.turn_rate = 90.0f * kDeg;
    u->set_drive(drive);
    auto gun = std::make_unique<Weapon>();
    gun->max_range = 40.0f;
    u->add_weapon(std::move(gun));
    auto* raw = u.get();
    sim.entity_registry().register_entity(std::move(u));
    return raw;
}

Unit* dummy(SimState& sim, f32 x, f32 z) {
    auto u = std::make_unique<Unit>();
    u->set_army(1);
    u->set_position({x, 7.8f, z});
    u->set_max_health(100.0f);
    u->set_health(100.0f);
    auto* raw = u.get();
    sim.entity_registry().register_entity(std::move(u));
    return raw;
}

Weapon& gun(Unit& u, size_t i = 0) {
    return *u.weapons()[i];
}

/// The point `dist` away from `u` on the bearing `deg` (0: +Z, 90: +X).
Vector3 at_bearing(const Unit& u, f32 deg, f32 dist) {
    return {u.position().x + dist * osc::dmath::sin(deg * kDeg), 7.8f,
            u.position().z + dist * osc::dmath::cos(deg * kDeg)};
}

f32 heading_deg(const Unit& u) {
    return osc::sim::quat_yaw(u.orientation()) / kDeg;
}

void ticks(SimState& sim, int n) {
    for (int t = 0; t < n; ++t) sim.tick();
}

/// `deg` brought into (-180, 180].
f32 wrap_deg(f32 deg) {
    while (deg > 180.0f) deg -= 360.0f;
    while (deg <= -180.0f) deg += 360.0f;
    return deg;
}

} // namespace

TEST_CASE("A slaved weapon's target turns the hull past its arc, until under half of it",
          "[hull-facing]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    flat(sim);
    Unit* u = hull(sim, 64.0f, 40.0f);
    gun(*u).slaved_to_body = true;
    gun(*u).slaved_arc_range = 30.0f;
    Unit* foe = dummy(sim, 0, 0);
    foe->set_position(at_bearing(*u, 20.0f, 20.0f));
    gun(*u).set_target_entity(foe->entity_id());
    ticks(sim, 50);
    CHECK(std::abs(heading_deg(*u)) < 0.01f); // 20 off: inside its 30

    foe->set_position(at_bearing(*u, 31.0f, 20.0f));
    ticks(sim, 10);
    // 9 a tick: 9, 18; then 13 off, under 15, and it stops.
    CHECK(std::abs(heading_deg(*u) - 18.0f) < 0.05f);
}

TEST_CASE("The default arc is 1 degree; a weapon not slaved, or an arc of 180, turns nothing",
          "[hull-facing]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    flat(sim);
    Unit* by_default = hull(sim, 30.0f, 30.0f);
    Unit* unslaved = hull(sim, 60.0f, 30.0f);
    Unit* wide = hull(sim, 90.0f, 30.0f);
    gun(*by_default).slaved_to_body = true;
    gun(*wide).slaved_to_body = true;
    gun(*wide).slaved_arc_range = 180.0f;
    for (Unit* u : {by_default, unslaved, wide}) {
        Unit* foe = dummy(sim, 0, 0);
        foe->set_position(at_bearing(*u, 2.0f, 20.0f));
        gun(*u).set_target_entity(foe->entity_id());
    }
    ticks(sim, 5);
    CHECK(std::abs(heading_deg(*by_default) - 2.0f) < 0.5f);
    CHECK(std::abs(heading_deg(*unslaved)) < 0.01f);
    CHECK(std::abs(heading_deg(*wide)) < 0.01f);
}

TEST_CASE("The first slaved weapon with a target decides", "[hull-facing]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    flat(sim);
    Unit* u = hull(sim, 64.0f, 40.0f);
    auto second = std::make_unique<Weapon>();
    second->max_range = 40.0f;
    second->slaved_to_body = true;
    u->add_weapon(std::move(second));
    gun(*u).slaved_to_body = true;
    Unit* left = dummy(sim, 0, 0);
    Unit* right = dummy(sim, 0, 0);
    left->set_position(at_bearing(*u, -40.0f, 20.0f));
    right->set_position(at_bearing(*u, 40.0f, 20.0f));
    gun(*u, 1).set_target_entity(right->entity_id());
    ticks(sim, 10);
    CHECK(std::abs(heading_deg(*u) - 40.0f) < 0.5f); // only the second has one
    gun(*u, 0).set_target_entity(left->entity_id());
    ticks(sim, 20);
    CHECK(std::abs(heading_deg(*u) + 40.0f) < 0.5f); // the first, in blueprint order
}

TEST_CASE("Moving, the hull faces its path; arrived, it turns to the target", "[hull-facing]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    flat(sim);
    Unit* u = hull(sim, 30.0f, 64.0f);
    gun(*u).slaved_to_body = true;
    Unit* foe = dummy(sim, 50.0f, 100.0f); // ahead of its path's end, to its left
    gun(*u).set_target_entity(foe->entity_id());
    UnitCommand move;
    move.type = CommandType::Move;
    move.target_pos = {50.0f, 7.8f, 64.0f}; // along +X: heading 90
    u->push_command(move, true);
    // While it drives, its hull is along its travel, not turned to the target.
    bool faced_path = false;
    for (int t = 0; t < 60 && !u->command_queue().empty(); ++t) {
        sim.tick();
        const Vector3& v = u->velocity();
        if (u->ground_speed() < 4.0f) continue;
        const f32 travel = osc::dmath::atan2(v.x, v.z) / kDeg;
        const f32 to_foe = osc::dmath::atan2(foe->position().x - u->position().x,
                                             foe->position().z - u->position().z) /
                           kDeg;
        faced_path = std::abs(wrap_deg(heading_deg(*u) - travel)) < 1.0f &&
                     std::abs(wrap_deg(heading_deg(*u) - to_foe)) > 30.0f;
    }
    CHECK(faced_path);
    ticks(sim, 30);
    const f32 bearing = osc::dmath::atan2(foe->position().x - u->position().x,
                                          foe->position().z - u->position().z) /
                        kDeg;
    CHECK(std::abs(wrap_deg(heading_deg(*u) - bearing)) < 0.5f);
}

TEST_CASE("AttackAngle puts the target that far off the bow, on its own side", "[hull-facing]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    flat(sim);
    const struct {
        f32 target;
        f32 heading;
    } cases[] = {{30.0f, -30.0f}, {-30.0f, 30.0f}, {0.0f, -60.0f}};
    f32 x = 20.0f;
    std::vector<Unit*> ships;
    for (const auto& c : cases) {
        Unit* u = hull(sim, x, 40.0f, "RULEUMT_Water");
        u->set_attack_angle(60.0f);
        gun(*u).slaved_to_body = true;
        Unit* foe = dummy(sim, 0, 0);
        foe->set_position(at_bearing(*u, c.target, 30.0f)); // in range or not: no matter
        gun(*u).set_target_entity(foe->entity_id());
        ships.push_back(u);
        x += 30.0f;
    }
    ticks(sim, 20);
    for (size_t i = 0; i < ships.size(); ++i) {
        INFO("case " << i);
        CHECK(std::abs(wrap_deg(heading_deg(*ships[i]) - cases[i].heading)) < 0.5f);
    }
}

TEST_CASE("A parked attack's AttackAngle faces the target off the bow, every 9 ticks",
          "[hull-facing]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    flat(sim);
    Unit* fatboy = hull(sim, 64.0f, 40.0f);
    fatboy->set_attack_angle(10.0f); // no slaved weapon
    Unit* foe = dummy(sim, 0, 0);
    foe->set_position(at_bearing(*fatboy, 50.0f, 20.0f)); // within its 40
    UnitCommand attack;
    attack.type = CommandType::Attack;
    attack.target_id = foe->entity_id();
    attack.target_pos = foe->position();
    fatboy->push_command(attack, true);
    ticks(sim, 12);
    CHECK(std::abs(wrap_deg(heading_deg(*fatboy) - 40.0f)) < 0.5f);
    // The target moves to 90: the facing is asked again only on the cadence.
    foe->set_position(at_bearing(*fatboy, 90.0f, 20.0f));
    ticks(sim, 12);
    CHECK(std::abs(wrap_deg(heading_deg(*fatboy) - 80.0f)) < 0.5f);
    // A move clears it.
    UnitCommand move;
    move.type = CommandType::Move;
    move.target_pos = {64.0f, 7.8f, 60.0f};
    fatboy->push_command(move, true);
    ticks(sim, 3);
    CHECK(fatboy->attack_facing().x == 0.0f);
    CHECK(fatboy->attack_facing().z == 0.0f);
}

TEST_CASE("A mobile unit's slaved weapon holds a target out of its arc and fires only in it",
          "[hull-facing]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    flat(sim);
    Unit* tank = hull(sim, 40.0f, 40.0f);
    Unit* tower = hull(sim, 90.0f, 40.0f, "RULEUMT_None");
    Unit* plain = hull(sim, 40.0f, 90.0f);
    for (Unit* u : {tank, tower, plain}) gun(*u).heading_arc_range = 60.0f;
    gun(*tank).slaved_to_body = true;
    gun(*tower).slaved_to_body = true;
    for (Unit* u : {tank, tower, plain}) {
        Unit* behind = dummy(sim, 0, 0);
        behind->set_position(at_bearing(*u, 180.0f, 20.0f));
        INFO("unit at " << u->position().x << ", " << u->position().z);
        const bool holds = gun(*u).can_target(*u, *behind, &sim);
        CHECK(holds == (u == tank));
        gun(*u).set_target_entity(behind->entity_id());
        CHECK_FALSE(gun(*u).can_fire(*u, sim.entity_registry()));
        CHECK_FALSE(gun(*u).in_heading_arc(*u, behind->position()));
    }
    // The slaved tank turns it into the arc, and can fire then.
    ticks(sim, 25);
    CHECK(gun(*tank).has_target());
    CHECK(gun(*tank).can_fire(*tank, sim.entity_registry()));
}

TEST_CASE("Held still or stunned it turns; an aircraft doesn't", "[hull-facing]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    flat(sim);
    Unit* held = hull(sim, 30.0f, 30.0f);
    held->set_immobile(true);
    Unit* plane = hull(sim, 80.0f, 30.0f, "RULEUMT_Air");
    plane->set_layer("Air");
    for (Unit* u : {held, plane}) {
        gun(*u).slaved_to_body = true;
        Unit* foe = dummy(sim, 0, 0);
        foe->set_position(at_bearing(*u, 45.0f, 20.0f));
        gun(*u).set_target_entity(foe->entity_id());
    }
    ticks(sim, 10);
    CHECK(std::abs(heading_deg(*held) - 45.0f) < 0.5f);
    CHECK(std::abs(heading_deg(*plane)) < 0.01f);
}

TEST_CASE("A landed aircraft doesn't turn to its slaved weapon's target", "[hull-facing]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    flat(sim);
    Unit* plane = hull(sim, 64.0f, 40.0f, "RULEUMT_Air");
    plane->set_layer("Land");
    REQUIRE_FALSE(plane->is_air_unit());
    gun(*plane).slaved_to_body = true;
    gun(*plane).slaved_arc_range = 50.0f;
    Unit* foe = dummy(sim, 0, 0);
    foe->set_position(at_bearing(*plane, 120.0f, 20.0f));
    gun(*plane).set_target_entity(foe->entity_id());
    ticks(sim, 20);
    CHECK(std::abs(heading_deg(*plane)) < 0.01f);
}

TEST_CASE("A turn in place is a move: Cruise, then Stopping, then Stopped", "[hull-facing]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    flat(sim);
    Unit* u = hull(sim, 64.0f, 40.0f);
    gun(*u).slaved_to_body = true;
    Unit* foe = dummy(sim, 0, 0);
    foe->set_position(at_bearing(*u, 90.0f, 20.0f));
    gun(*u).set_target_entity(foe->entity_id());
    std::vector<Unit::MotionHorz> seen;
    for (int t = 0; t < 20; ++t) {
        sim.tick();
        if (seen.empty() || seen.back() != u->motion_horz()) seen.push_back(u->motion_horz());
    }
    REQUIRE(seen.size() == 3);
    CHECK(seen[0] == Unit::MotionHorz::Cruise);
    CHECK(seen[1] == Unit::MotionHorz::Stopping);
    CHECK(seen[2] == Unit::MotionHorz::Stopped);
}

TEST_CASE("attack_angle_heading: Moho's side rule", "[hull-facing]") {
    const auto deg = [](f32 r) { return r / kDeg; };
    CHECK(std::abs(deg(osc::sim::attack_angle_heading(30 * kDeg, 0, 60 * kDeg)) + 30.0f) < 1e-3f);
    CHECK(std::abs(deg(osc::sim::attack_angle_heading(-30 * kDeg, 0, 60 * kDeg)) - 30.0f) < 1e-3f);
    CHECK(std::abs(deg(osc::sim::attack_angle_heading(0, 0, 60 * kDeg)) + 60.0f) < 1e-3f);
    // Across the wrap: heading 170, target at -170 (20 to the right).
    CHECK(
        std::abs(wrap_deg(deg(osc::sim::attack_angle_heading(-170 * kDeg, 170 * kDeg, 60 * kDeg))) -
                 130.0f) < 1e-3f);
}

TEST_CASE("A unit that stops to fire brakes straight on while its hull turns", "[hull-facing]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    flat(sim);
    // Slow to brake (5 a second, at 5 a second: a second's coast), it turns
    // 90 a second to a slaved target abeam as it stops for its attack.
    Unit* u = hull(sim, 20.0f, 64.0f);
    Unit::Drive drive = u->drive();
    drive.max_brake = 5.0f;
    u->set_drive(drive);
    gun(*u).slaved_to_body = true;
    UnitCommand move;
    move.type = CommandType::Move;
    move.target_pos = {120.0f, 7.8f, 64.0f}; // along +X
    u->push_command(move, true);
    for (int t = 0; t < 40; ++t) sim.tick();
    REQUIRE(u->ground_speed() > 4.0f);
    const f32 z = u->position().z;
    Unit* foe = dummy(sim, 0, 0);
    foe->set_position({u->position().x, 7.8f, u->position().z + 30.0f}); // abeam, in range 40
    gun(*u).set_target_entity(foe->entity_id());
    UnitCommand attack;
    attack.type = CommandType::Attack;
    attack.target_id = foe->entity_id();
    attack.target_pos = foe->position();
    u->push_command(attack, true);
    for (int t = 0; t < 15; ++t) sim.tick();
    CHECK(u->ground_speed() == 0.0f);
    CHECK(std::abs(u->position().z - z) < 0.05f); // on its line
    // ...and has turned to the target, behind it now as it coasted on
    const f32 bearing = osc::dmath::atan2(foe->position().x - u->position().x,
                                          foe->position().z - u->position().z) /
                        kDeg;
    CHECK(std::abs(wrap_deg(heading_deg(*u) - bearing)) < 1.0f);
}
