#include <catch2/catch_test_macros.hpp>

#include "map/heightmap.hpp"
#include "map/terrain.hpp"
#include "sim/manipulator.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/weapon.hpp"

extern "C" {
#include <lua.h>
}

#include <memory>
#include <vector>

using osc::f32;
using osc::u32;
using osc::sim::SimState;
using osc::sim::Unit;
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

Unit* unit(SimState& sim, int army, f32 x, f32 z) {
    auto u = std::make_unique<Unit>();
    u->set_army(army);
    u->set_motion_type("RULEUMT_None");
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
    Scene() {
        flat(sim);
        ship = unit(sim, 0, 64.0f, 64.0f);
        near = unit(sim, 1, 74.0f, 64.0f);
        far = unit(sim, 1, 84.0f, 64.0f);
    }
};

} // namespace

TEST_CASE("PrefersPrimaryWeaponTarget takes the primary's target when it can hit it",
          "[weapon][primary-weapon]") {
    Scene s;
    Weapon& primary = arm(*s.ship, 30.0f);
    Weapon& second = arm(*s.ship, 30.0f);
    second.prefers_primary_weapon_target = true;
    primary.set_target_entity(s.far->entity_id());
    ticks(s.sim, *s.ship, 3);
    REQUIRE(primary.target_entity_id == s.far->entity_id());
    CHECK(second.target_entity_id == s.far->entity_id());
}

TEST_CASE("PrefersPrimaryWeaponTarget picks its own target when the primary's is out of range",
          "[weapon][primary-weapon]") {
    Scene s;
    Weapon& primary = arm(*s.ship, 30.0f);
    Weapon& second = arm(*s.ship, 15.0f);
    second.prefers_primary_weapon_target = true;
    primary.set_target_entity(s.far->entity_id());
    ticks(s.sim, *s.ship, 3);
    REQUIRE(primary.target_entity_id == s.far->entity_id());
    CHECK(second.target_entity_id == s.near->entity_id());
}

TEST_CASE("PrefersPrimaryWeaponTarget shares the primary's ground target",
          "[weapon][primary-weapon]") {
    Scene s;
    Weapon& primary = arm(*s.ship, 30.0f);
    Weapon& second = arm(*s.ship, 30.0f);
    second.prefers_primary_weapon_target = true;
    primary.set_target_ground({60.0f, 7.8f, 80.0f});
    ticks(s.sim, *s.ship, 3);
    REQUIRE(primary.has_ground_target);
    CHECK(second.has_ground_target);
    CHECK(second.ground_target.z == 80.0f);
}

TEST_CASE("StopOnPrimaryWeaponBusy drops its target while the primary has one",
          "[weapon][primary-weapon]") {
    Scene s;
    Weapon& primary = arm(*s.ship, 30.0f);
    Weapon& bomb = arm(*s.ship, 30.0f);
    bomb.stop_on_primary_weapon_busy = true;
    ticks(s.sim, *s.ship, 3);
    REQUIRE(primary.target_entity_id == s.near->entity_id());
    CHECK_FALSE(bomb.has_target());
}

TEST_CASE("StopOnPrimaryWeaponBusy targets as usual while the primary has none",
          "[weapon][primary-weapon]") {
    Scene s;
    Weapon& primary = arm(*s.ship, 5.0f);
    Weapon& bomb = arm(*s.ship, 30.0f);
    bomb.stop_on_primary_weapon_busy = true;
    ticks(s.sim, *s.ship, 3);
    REQUIRE_FALSE(primary.has_target());
    CHECK(bomb.target_entity_id == s.near->entity_id());
}
