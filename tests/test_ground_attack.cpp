// An attack order at a point on the ground, as Moho's CUnitAttackTargetTask
// carries out an AITARGET_Ground target (faf-re): the unit comes within its
// weapon's range and its weapons take the point; the order lasts until
// another follows and AttackGroundTries shots have gone at it, then goes to
// the back of the queue (IAiCommandDispatchImpl). UnitWeapon::CanAttackTarget
// decides which weapons can hit the ground there.

#include <catch2/catch_test_macros.hpp>

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
#include <vector>

using osc::f32;
using osc::sim::CommandType;
using osc::sim::SimState;
using osc::sim::Unit;
using osc::sim::UnitCommand;
using osc::sim::Vector3;
using osc::sim::Weapon;

namespace {

struct LuaGuard {
    lua_State* L = lua_open();
    ~LuaGuard() { lua_close(L); }
};

/// 128 x 128: land (height 7.8) east of x = 64, a seabed (height 0) west of
/// it, under water at `water` when there is any.
std::unique_ptr<osc::map::Terrain> terrain(bool with_water, f32 water = 5.0f) {
    std::vector<osc::u16> heights(129 * 129, 1000);
    for (size_t z = 0; z <= 128; ++z)
        for (size_t x = 0; x < 64; ++x) heights[z * 129 + x] = 0;
    osc::map::Heightmap hm(128, 128, 1.0f / 128.0f, std::move(heights));
    return std::make_unique<osc::map::Terrain>(std::move(hm), water, with_water);
}

void land(SimState& sim) {
    sim.set_terrain(terrain(false));
    sim.build_pathfinding_grid();
    sim.add_army("ARMY_1", "ARMY_1");
    sim.add_army("ARMY_2", "ARMY_2");
    sim.set_fog_of_war("none");
    sim.set_victory_condition("sandbox");
}

/// A land unit with one gun of range `range`, which never fires by itself
/// (its fire clock is held): the tests count its shots.
Unit* tank(SimState& sim, f32 x, f32 z, f32 range = 10.0f, f32 speed = 5.0f) {
    auto u = std::make_unique<Unit>();
    u->set_army(0);
    u->set_max_speed(speed);
    u->set_motion_type(speed > 0 ? "RULEUMT_Land" : "RULEUMT_None");
    u->set_position({x, 7.8f, z});
    u->set_max_health(100.0f);
    u->set_health(100.0f);
    Unit::Drive drive;
    drive.max_accel = 50.0f;
    u->set_drive(drive);
    auto gun = std::make_unique<Weapon>();
    gun->max_range = range;
    gun->damage = 10.0f;
    u->add_weapon(std::move(gun));
    auto* raw = u.get();
    sim.entity_registry().register_entity(std::move(u));
    return raw;
}

Weapon& gun(Unit& u) {
    return *u.weapons().front();
}

/// Hold the gun's fire: the tests count shots themselves.
void hold(Unit& u) {
    gun(u).fire_clock = 1000000;
}

UnitCommand ground_attack(Vector3 at) {
    UnitCommand c;
    c.type = CommandType::Attack;
    c.target_pos = at;
    return c;
}

UnitCommand move_to(Vector3 at) {
    UnitCommand c;
    c.type = CommandType::Move;
    c.target_pos = at;
    return c;
}

f32 dist_to(const Unit& u, Vector3 at) {
    return std::hypot(u.position().x - at.x, u.position().z - at.z);
}

void ticks(SimState& sim, Unit& u, int n) {
    for (int t = 0; t < n; ++t) {
        hold(u);
        sim.tick();
    }
}

} // namespace

TEST_CASE("A ground attack brings its unit within range, and its weapon takes the point",
          "[ground-attack]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    land(sim);
    Unit* u = tank(sim, 70.0f, 64.0f);
    const Vector3 at{110.0f, 7.8f, 64.0f};
    u->push_command(ground_attack(at), true);
    ticks(sim, *u, 150);

    REQUIRE(u->command_queue().size() == 1);
    CHECK(u->command_queue().front().type == CommandType::Attack);
    CHECK(u->command_queue().front().engaged);
    // It stops once in range, not at the point.
    CHECK(dist_to(*u, at) <= 10.0f);
    CHECK(dist_to(*u, at) > 5.0f);
    CHECK(gun(*u).has_ground_target);
    CHECK(gun(*u).ground_from_order);
    CHECK(gun(*u).ground_target.x == at.x);
    CHECK(gun(*u).ground_target.z == at.z);
}

TEST_CASE("A ground attack with no order after it never ends", "[ground-attack]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    land(sim);
    Unit* u = tank(sim, 100.0f, 64.0f);
    const Vector3 at{105.0f, 7.8f, 64.0f};
    u->push_command(ground_attack(at), true);
    ticks(sim, *u, 5);
    REQUIRE(gun(*u).ground_from_order);
    gun(*u).shots_at_target = 50;
    ticks(sim, *u, 20);
    REQUIRE(u->command_queue().size() == 1);
    CHECK(u->command_queue().front().type == CommandType::Attack);
    CHECK(gun(*u).has_ground_target);
}

TEST_CASE("After AttackGroundTries shots, a ground attack goes to the back of the queue",
          "[ground-attack]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    land(sim);
    Unit* u = tank(sim, 100.0f, 64.0f);
    const Vector3 at{105.0f, 7.8f, 64.0f};
    const Vector3 next{100.0f, 7.8f, 90.0f};
    u->push_command(ground_attack(at), true);
    u->push_command(move_to(next), false);
    ticks(sim, *u, 5);
    REQUIRE(gun(*u).ground_from_order);

    // Fewer shots than its tries (3, Moho's default): it stays.
    gun(*u).shots_at_target = 2;
    ticks(sim, *u, 3);
    REQUIRE(u->command_queue().size() == 2);
    CHECK(u->command_queue().front().type == CommandType::Attack);

    gun(*u).shots_at_target = 3;
    ticks(sim, *u, 1);
    REQUIRE(u->command_queue().size() == 2);
    CHECK(u->command_queue().front().type == CommandType::Move);
    CHECK(u->command_queue().back().type == CommandType::Attack);
    CHECK_FALSE(u->command_queue().back().engaged);
    // Its weapon lets the point go with the order.
    ticks(sim, *u, 1);
    CHECK_FALSE(gun(*u).has_ground_target);
    CHECK(gun(*u).shots_at_target == 0);

    // The move done, the attack is its only order, and runs for good.
    ticks(sim, *u, 100);
    REQUIRE(u->command_queue().size() == 1);
    CHECK(u->command_queue().front().type == CommandType::Attack);
}

TEST_CASE("A weapon's own AttackGroundTries counts", "[ground-attack]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    land(sim);
    Unit* u = tank(sim, 100.0f, 64.0f);
    gun(*u).attack_ground_tries = 6;
    u->push_command(ground_attack({105.0f, 7.8f, 64.0f}), true);
    u->push_command(move_to({100.0f, 7.8f, 90.0f}), false);
    ticks(sim, *u, 5);
    gun(*u).shots_at_target = 5;
    ticks(sim, *u, 2);
    CHECK(u->command_queue().front().type == CommandType::Attack);
    gun(*u).shots_at_target = 6;
    ticks(sim, *u, 1);
    CHECK(u->command_queue().front().type == CommandType::Move);
}

TEST_CASE("A weapon that cannot attack the ground never takes the point", "[ground-attack]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    land(sim);
    Unit* u = tank(sim, 70.0f, 64.0f);
    gun(*u).cannot_attack_ground = true;
    const Vector3 at{100.0f, 7.8f, 64.0f};
    u->push_command(ground_attack(at), true);
    ticks(sim, *u, 150);
    // With no weapon for it, the unit goes to the point and stays.
    REQUIRE(u->command_queue().size() == 1);
    CHECK(u->command_queue().front().type == CommandType::Attack);
    CHECK(dist_to(*u, at) <= 1.5f);
    CHECK_FALSE(gun(*u).has_ground_target);
}

TEST_CASE("A structure takes the point at once, and fires only within its range",
          "[ground-attack]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    land(sim);
    Unit* pd = tank(sim, 100.0f, 64.0f, 10.0f, 0.0f);
    pd->push_command(ground_attack({120.0f, 7.8f, 64.0f}), true);
    ticks(sim, *pd, 5);
    REQUIRE(pd->command_queue().size() == 1);
    CHECK(pd->command_queue().front().engaged);
    CHECK(pd->position().x == 100.0f);
    CHECK_FALSE(gun(*pd).has_ground_target); // 20 away, its range 10

    pd->push_command(ground_attack({106.0f, 7.8f, 64.0f}), true);
    ticks(sim, *pd, 2);
    CHECK(gun(*pd).has_ground_target);
}

TEST_CASE("The order gone, its point goes; a script's ground target stays", "[ground-attack]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    land(sim);
    Unit* u = tank(sim, 100.0f, 64.0f);
    u->push_command(ground_attack({105.0f, 7.8f, 64.0f}), true);
    ticks(sim, *u, 5);
    REQUIRE(gun(*u).ground_from_order);
    u->push_command(move_to({100.0f, 7.8f, 70.0f}), true); // a fresh order
    ticks(sim, *u, 1);
    CHECK_FALSE(gun(*u).has_ground_target);

    // SetTargetGround from a script: no order, and it stays.
    gun(*u).set_target_ground({104.0f, 7.8f, 70.0f});
    ticks(sim, *u, 20);
    CHECK(gun(*u).has_ground_target);
    CHECK_FALSE(gun(*u).ground_from_order);
}

TEST_CASE("A weapon's shot count starts again with a new target", "[ground-attack]") {
    Weapon w;
    w.set_target_ground({1.0f, 0.0f, 1.0f});
    w.shots_at_target = 4;
    w.set_target_ground({1.0f, 0.0f, 1.0f}); // the same point
    CHECK(w.shots_at_target == 4);
    w.set_target_ground({2.0f, 0.0f, 1.0f});
    CHECK(w.shots_at_target == 0);
    w.shots_at_target = 2;
    w.set_target_entity(7);
    CHECK(w.shots_at_target == 0);
    w.shots_at_target = 2;
    w.set_target_entity(7);
    CHECK(w.shots_at_target == 2);
}

TEST_CASE("Which weapons can attack the ground: CannotAttackGround and the layer there",
          "[ground-attack]") {
    Weapon w;
    const auto with_water = terrain(true, 5.0f);
    const Vector3 on_land{100.0f, 0.0f, 64.0f};
    const Vector3 on_water{20.0f, 0.0f, 64.0f};
    CHECK(w.can_attack_ground(on_land, with_water.get()));
    CHECK(w.can_attack_ground(on_water, with_water.get()));
    CHECK(w.can_attack_ground(on_land, nullptr)); // no terrain: land

    w.fire_target_layer_caps = osc::sim::layer_to_bit("Land");
    CHECK(w.can_attack_ground(on_land, with_water.get()));
    CHECK_FALSE(w.can_attack_ground(on_water, with_water.get()));
    w.fire_target_layer_caps = osc::sim::layer_to_bit("Water") | osc::sim::layer_to_bit("Sub");
    CHECK_FALSE(w.can_attack_ground(on_land, with_water.get()));
    CHECK(w.can_attack_ground(on_water, with_water.get()));

    // A map without water: the low ground is land.
    const auto dry = terrain(false, 5.0f);
    w.fire_target_layer_caps = osc::sim::layer_to_bit("Land");
    CHECK(w.can_attack_ground(on_water, dry.get()));
    // Where the ground meets the water exactly, neither layer: no weapon.
    const auto level = terrain(true, 0.0f);
    w.fire_target_layer_caps = 0xFF;
    CHECK_FALSE(w.can_attack_ground(on_water, level.get()));

    w.cannot_attack_ground = true;
    CHECK_FALSE(w.can_attack_ground(on_land, with_water.get()));
    // Aimed at the ground anyway (a script's SetTargetGround), it holds its
    // fire; at a unit it fires.
    CHECK_FALSE(w.ground_fire_barred());
    w.set_target_ground(on_land);
    CHECK(w.ground_fire_barred());
    w.set_target_entity(5);
    CHECK_FALSE(w.ground_fire_barred());
}
