// ReTargetOnMiss, as Moho's CAcquireTargetTask listens to its weapon's shots
// and beam checks (faf-re CAcquireTargetTask.cpp, UnitWeapon.cpp): misses of a
// target that, like the unit, stands still turn the weapon to another spot on
// it, then pass over the attack order's target (its first weapon) or give the
// weapon's own target up for a while.

#include <catch2/catch_test_macros.hpp>

#include "map/heightmap.hpp"
#include "map/terrain.hpp"
#include "sim/collision.hpp"
#include "sim/collision_beam.hpp"
#include "sim/entity_registry.hpp"
#include "sim/manipulator.hpp"
#include "sim/projectile.hpp"
#include "sim/shield.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/unit_command.hpp"
#include "sim/weapon.hpp"

extern "C" {
#include <lua.h>
}

#include <memory>
#include <utility>
#include <vector>

using osc::f32;
using osc::u32;
using osc::sim::CommandType;
using osc::sim::EntityRegistry;
using osc::sim::SimState;
using osc::sim::Unit;
using osc::sim::UnitCommand;
using osc::sim::Weapon;

namespace {

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

Unit* unit(SimState& sim, int army, f32 x, f32 z, const char* motion = "RULEUMT_None") {
    auto u = std::make_unique<Unit>();
    u->set_army(army);
    u->set_motion_type(motion);
    u->set_position({x, 7.8f, z});
    u->set_max_health(100.0f);
    u->set_health(100.0f);
    auto* raw = u.get();
    sim.entity_registry().register_entity(std::move(u));
    return raw;
}

Weapon& arm(Unit& u, f32 range) {
    auto w = std::make_unique<Weapon>();
    w->max_range = range;
    w->damage = 10.0f;
    w->always_recheck_target = false;
    w->retarget_on_miss = true;
    w->weapon_index = static_cast<osc::i32>(u.weapons().size());
    u.add_weapon(std::move(w));
    return *u.weapons().back();
}

void ticks(SimState& sim, Unit& u, int n) {
    for (int t = 0; t < n; ++t) {
        for (const auto& w : u.weapons()) {
            w->fire_clock = 1000000;
        }
        sim.tick();
    }
}

struct Scene {
    LuaGuard g;
    SimState sim{g.L, nullptr};
    Unit* ship = nullptr;
    Unit* near = nullptr;
    Unit* far = nullptr;
    explicit Scene(const char* motion = "RULEUMT_None") {
        flat(sim);
        ship = unit(sim, 0, 64.0f, 64.0f, motion);
        near = unit(sim, 1, 74.0f, 64.0f);
        far = unit(sim, 1, 84.0f, 64.0f);
    }
    EntityRegistry& registry() { return sim.entity_registry(); }
};

Weapon::TargetMark at_entity(u32 id) {
    Weapon::TargetMark m;
    m.entity = id;
    return m;
}

} // namespace

TEST_CASE("A shot or beam check misses unless it strikes its target, a shield or a shot",
          "[weapon][retarget]") {
    // CAiTarget::ImpactDidHitEntity, then Projectile::Impact's and
    // CollisionBeamEntity::CheckCollision's three outcomes
    Scene s;
    auto shield = std::make_unique<osc::sim::Shield>();
    const osc::sim::Entity* dome =
        s.registry().find(s.registry().register_entity(std::move(shield)));
    auto turret = std::make_unique<Unit>();
    turret->set_parent(s.near->entity_id(), 0);
    const osc::sim::Entity* on_near =
        s.registry().find(s.registry().register_entity(std::move(turret)));

    using osc::sim::impact_missed;
    const Weapon::TargetMark target = at_entity(s.near->entity_id());
    CHECK_FALSE(impact_missed(target, s.near, "Unit", true));
    CHECK_FALSE(impact_missed(target, on_near, "Unit", true)); // attached to it
    CHECK(impact_missed(target, s.far, "Unit", true));
    CHECK(impact_missed(target, nullptr, "Terrain", true));
    CHECK_FALSE(impact_missed(target, dome, "Shield", true));
    CHECK_FALSE(impact_missed(target, dome, "Shield", false));
    // Meeting another shot is irrelevant to a shot, a miss to a beam
    CHECK_FALSE(impact_missed(target, s.far, "Projectile", true));
    CHECK(impact_missed(target, s.far, "Projectile", false));

    Weapon::TargetMark ground;
    ground.ground = true;
    CHECK_FALSE(impact_missed(ground, nullptr, "Terrain", false));
    CHECK_FALSE(impact_missed(ground, nullptr, "Air", false));
    CHECK(impact_missed(ground, s.far, "Unit", false));

    // No target: nothing misses. A held one: anything attached to nothing strikes it
    CHECK_FALSE(impact_missed(Weapon::TargetMark{}, nullptr, "Terrain", false));
    Weapon::TargetMark held;
    held.held = true;
    CHECK_FALSE(impact_missed(held, s.far, "Unit", false));
    CHECK(impact_missed(held, on_near, "Unit", false));
    CHECK(impact_missed(held, nullptr, "Terrain", false));
}

TEST_CASE("ReTargetOnMiss gives a target up on the eleventh miss in a row, and passes over it",
          "[weapon][retarget]") {
    Scene s;
    Weapon& gun = arm(*s.ship, 30.0f);
    ticks(s.sim, *s.ship, 2);
    REQUIRE(gun.target_entity_id == s.near->entity_id());

    // Ten misses look at other spots on it; a hit starts the count again
    for (int i = 0; i < 10; ++i) gun.note_impact(*s.ship, true, s.registry(), &s.sim);
    CHECK(gun.retarget_misses == 10);
    gun.note_impact(*s.ship, false, s.registry(), &s.sim);
    CHECK(gun.retarget_misses == 0);
    for (int i = 0; i < 10; ++i) gun.note_impact(*s.ship, true, s.registry(), &s.sim);
    CHECK(gun.target_entity_id == s.near->entity_id());

    // The eleventh gives it up: a held target, its script told nothing, and the change ages it
    gun.note_impact(*s.ship, true, s.registry(), &s.sim);
    CHECK(gun.target_entity_id == 0);
    CHECK(gun.target_held);
    CHECK_FALSE(gun.has_target());
    CHECK(gun.retarget_misses == 0);
    REQUIRE(gun.blacklist.size() == 1);
    CHECK(gun.blacklist[0] == std::pair<u32, osc::i32>{s.near->entity_id(), 9});

    // Its next look passes over it, and the change to the other target ages it again
    gun.target_check_clock = 0;
    ticks(s.sim, *s.ship, 1);
    CHECK(gun.target_entity_id == s.far->entity_id());
    CHECK_FALSE(gun.target_held);
    REQUIRE(gun.blacklist.size() == 1);
    CHECK(gun.blacklist[0].second == 8);
}

TEST_CASE("A target ReTargetOnMiss gave up is dropped once a look finds no other",
          "[weapon][retarget]") {
    Scene s;
    Weapon& gun = arm(*s.ship, 15.0f);
    ticks(s.sim, *s.ship, 2);
    REQUIRE(gun.target_entity_id == s.near->entity_id());
    for (int i = 0; i < 11; ++i) {
        gun.note_impact(*s.ship, true, s.registry(), &s.sim);
    }
    REQUIRE(gun.target_held);
    gun.target_check_clock = 0;
    ticks(s.sim, *s.ship, 1);
    CHECK_FALSE(gun.target_held);
    CHECK_FALSE(gun.has_target());
}

TEST_CASE("ReTargetOnMiss counts only misses while the unit and its target stand still",
          "[weapon][retarget]") {
    Scene s;
    Weapon& gun = arm(*s.ship, 30.0f);
    ticks(s.sim, *s.ship, 2);
    REQUIRE(gun.target_entity_id == s.near->entity_id());
    s.near->set_position({74.0f, 7.8f, 66.0f});
    ticks(s.sim, *s.ship, 1);
    REQUIRE(s.near->moved_last_tick());
    for (int i = 0; i < 20; ++i) gun.note_impact(*s.ship, true, s.registry(), &s.sim);
    CHECK(gun.retarget_misses == 0);
    CHECK(gun.target_entity_id == s.near->entity_id());
}

TEST_CASE("The targets ReTargetOnMiss gave up are forgotten once the unit moves",
          "[weapon][retarget]") {
    Scene s;
    Weapon& gun = arm(*s.ship, 30.0f);
    ticks(s.sim, *s.ship, 2);
    for (int i = 0; i < 11; ++i) gun.note_impact(*s.ship, true, s.registry(), &s.sim);
    REQUIRE(gun.blacklist.size() == 1);
    // Standing still, the acquire step keeps them
    ticks(s.sim, *s.ship, 1);
    CHECK(gun.blacklist.size() == 1);
    s.ship->set_position({64.0f, 7.8f, 62.0f});
    ticks(s.sim, *s.ship, 2);
    CHECK(gun.blacklist.empty());
}

TEST_CASE("ReTargetOnMiss's first weapon passes over its attack order's target after the "
          "sixth miss",
          "[weapon][retarget]") {
    // A mobile unit's first weapon with an attack order's target: Moho asks of the attacker's
    // desired target first, and sets the cooldown instead of giving anything up
    Scene s("RULEUMT_Land");
    Weapon& gun = arm(*s.ship, 30.0f);
    UnitCommand attack;
    attack.type = CommandType::Attack;
    attack.target_id = s.near->entity_id();
    attack.engaged = true;
    s.ship->push_command(attack, true);
    gun.set_target_entity(s.near->entity_id());
    for (int i = 0; i < 5; ++i) gun.note_impact(*s.ship, true, s.registry(), &s.sim);
    CHECK(gun.retarget_cooldown == 0);
    gun.note_impact(*s.ship, true, s.registry(), &s.sim);
    CHECK(gun.retarget_cooldown == 30);
    for (int i = 0; i < 10; ++i) gun.note_impact(*s.ship, true, s.registry(), &s.sim);
    CHECK(gun.blacklist.empty());
    CHECK(gun.target_entity_id == s.near->entity_id());
    // A hit ends it
    gun.note_impact(*s.ship, false, s.registry(), &s.sim);
    CHECK(gun.retarget_cooldown == 0);
    CHECK(gun.retarget_misses == 0);
}

TEST_CASE("While ReTargetOnMiss passes over the attack order's target, the weapon leaves it",
          "[weapon][retarget]") {
    Scene s;
    Weapon& gun = arm(*s.ship, 30.0f);
    UnitCommand attack;
    attack.type = CommandType::Attack;
    attack.target_id = s.near->entity_id();
    attack.engaged = true;
    s.ship->push_command(attack, true);
    gun.set_target_entity(s.far->entity_id());
    gun.target_check_clock = 1000000;
    gun.retarget_cooldown = 30;
    ticks(s.sim, *s.ship, 30);
    CHECK(gun.retarget_cooldown == 0);
    CHECK(gun.target_entity_id == s.far->entity_id());
    ticks(s.sim, *s.ship, 1);
    CHECK(gun.target_entity_id == s.near->entity_id());
}

TEST_CASE("While ReTargetOnMiss passes over its attack order's target, its unit goes at it",
          "[weapon][retarget]") {
    Scene s("RULEUMT_Land");
    s.ship->set_max_speed(5.0f);
    Unit::Drive drive;
    drive.max_accel = 50.0f;
    s.ship->set_drive(drive);
    Weapon& gun = arm(*s.ship, 30.0f);
    UnitCommand attack;
    attack.type = CommandType::Attack;
    attack.target_id = s.near->entity_id();
    s.ship->push_command(attack, true);
    ticks(s.sim, *s.ship, 3);
    REQUIRE_FALSE(s.ship->navigator().is_moving());
    const f32 x0 = s.ship->position().x;

    gun.retarget_cooldown = 30;
    ticks(s.sim, *s.ship, 10);
    CHECK(s.ship->navigator().is_moving());
    CHECK(s.ship->position().x > x0 + 0.5f);
    ticks(s.sim, *s.ship, 25);
    CHECK(gun.retarget_cooldown == 0);
    CHECK_FALSE(s.ship->navigator().is_moving());
}

TEST_CASE("A ReTargetOnMiss weapon's shot tells it where it landed", "[weapon][retarget]") {
    // UnitWeapon::CreateProjectile binds the shot to its weapon when the weapon has a target unit;
    // Projectile::Impact reports after OnImpact
    Scene s;
    Weapon& gun = arm(*s.ship, 30.0f);
    ticks(s.sim, *s.ship, 2);
    REQUIRE(gun.target_entity_id == s.near->entity_id());
    osc::sim::Projectile* shot =
        gun.launch(*s.ship, {64.0f, 9.0f, 64.0f}, s.near, s.registry(), s.g.L, false);
    REQUIRE(shot);
    CHECK(shot->impact_listener_weapon == 0);
    CHECK(shot->impact_listener_target == s.near->entity_id());
    // Straight down, it lands on the ground at the ship's feet: a miss
    shot->velocity = {0.0f, -40.0f, 0.0f};
    ticks(s.sim, *s.ship, 2);
    CHECK(gun.retarget_misses == 1);

    gun.retarget_on_miss = false;
    osc::sim::Projectile* plain =
        gun.launch(*s.ship, {64.0f, 9.0f, 64.0f}, s.near, s.registry(), s.g.L, false);
    REQUIRE(plain);
    CHECK(plain->impact_listener_weapon == -1);
}

TEST_CASE("A ReTargetOnMiss weapon's beam tells it whether each check struck its target",
          "[weapon][retarget]") {
    Scene s;
    Weapon& gun = arm(*s.ship, 30.0f);
    ticks(s.sim, *s.ship, 2);
    REQUIRE(gun.target_entity_id == s.near->entity_id());
    // A beam from the ship along +z: the far unit isn't there either, so it reaches nothing
    auto beam = std::make_unique<osc::sim::Entity>();
    beam->set_collision_beam(true);
    beam->set_beam_launcher_id(s.ship->entity_id());
    beam->beam_setup().weapon = 0;
    beam->beam_setup().length = 20;
    beam->set_position({64.0f, 9.0f, 64.0f});
    const u32 beam_id = s.registry().register_entity(std::move(beam));
    osc::sim::check_collision_beam(s.sim, nullptr, beam_id, false);
    CHECK(gun.retarget_misses == 1);
    osc::sim::check_collision_beam(s.sim, nullptr, beam_id, false);
    CHECK(gun.retarget_misses == 2);
}

TEST_CASE("A ReTargetOnMiss weapon's beam check that strikes its target starts the count again",
          "[weapon][retarget]") {
    Scene s;
    s.ship->set_position({64.0f, 9.0f, 64.0f});
    Unit* ahead = unit(s.sim, 1, 64.0f, 70.0f);
    ahead->set_position({64.0f, 9.0f, 70.0f});
    osc::sim::CollisionShape box;
    box.type = osc::sim::CollisionShapeType::BOX;
    box.sx = box.sy = box.sz = 1.0f;
    ahead->set_default_collision_shape(box);
    Weapon& gun = arm(*s.ship, 30.0f);
    gun.target_check_clock = 1000000;
    ticks(s.sim, *s.ship, 1);
    gun.set_target_entity(ahead->entity_id());
    gun.retarget_misses = 3;
    auto beam = std::make_unique<osc::sim::Entity>();
    beam->set_collision_beam(true);
    beam->set_beam_launcher_id(s.ship->entity_id());
    beam->beam_setup().weapon = 0;
    beam->beam_setup().length = 20;
    const u32 beam_id = s.registry().register_entity(std::move(beam));
    osc::sim::check_collision_beam(s.sim, nullptr, beam_id, false);
    CHECK(gun.retarget_misses == 0);
}

TEST_CASE("ReTargetOnMiss's cooldown counts down under hold fire", "[weapon][retarget]") {
    // Moho's acquire task counts it down ahead of its hold-fire check
    Scene s;
    Weapon& gun = arm(*s.ship, 30.0f);
    s.ship->set_fire_state(1);
    gun.retarget_cooldown = 5;
    ticks(s.sim, *s.ship, 5);
    CHECK(gun.retarget_cooldown == 0);
}
