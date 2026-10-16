// A weapon picks only what its army knows (SimState::weapons_know; Moho's
// Unit::UpdateBlipsInRange keeps the enemies the army holds a blip of): a
// unit it senses now, or a structure it has seen. With the fog off, every
// unit.

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
#include <string>
#include <vector>

using osc::f32;
using osc::sim::SimState;
using osc::sim::Unit;
using osc::sim::Weapon;

namespace {

struct LuaGuard {
    lua_State* L = lua_open();
    ~LuaGuard() { lua_close(L); }
};

Unit* unit(SimState& sim, int army, f32 x, f32 z, const char* motion) {
    auto u = std::make_unique<Unit>();
    u->set_army(army);
    u->set_motion_type(motion);
    u->set_position({x, 8.3f, z});
    u->set_max_health(100.0f);
    u->set_health(100.0f);
    auto* raw = u.get();
    sim.entity_registry().register_entity(std::move(u));
    return raw;
}

/// A gun of army 0 at x = 64 and an enemy of army 1 10 east of it, on flat
/// ground with the fog as `fog`. Neither has any sight of its own.
struct Scene {
    LuaGuard g;
    SimState sim{g.L, nullptr};
    Unit* gun = nullptr;
    Unit* enemy = nullptr;
    Weapon* weapon = nullptr;
    Scene(const std::string& fog, const char* enemy_motion) {
        std::vector<osc::u16> heights(129 * 129, 1000);
        osc::map::Heightmap hm(128, 128, 1.0f / 128.0f, std::move(heights));
        sim.set_terrain(std::make_unique<osc::map::Terrain>(std::move(hm), 0.0f, false));
        sim.build_pathfinding_grid();
        sim.add_army("ARMY_1", "ARMY_1");
        sim.add_army("ARMY_2", "ARMY_2");
        sim.set_fog_of_war(fog);
        sim.set_victory_condition("sandbox");
        gun = unit(sim, 0, 64.0f, 64.0f, "RULEUMT_None");
        enemy = unit(sim, 1, 74.0f, 64.0f, enemy_motion);
        auto w = std::make_unique<Weapon>();
        w->max_range = 20.0f;
        w->damage = 10.0f;
        gun->add_weapon(std::move(w));
        weapon = gun->weapons().back().get();
    }
    void ticks(int n) {
        for (int t = 0; t < n; ++t) {
            weapon->fire_clock = 1000000; // nothing is fired
            sim.tick();
        }
    }
};

} // namespace

TEST_CASE("With the fog on, a weapon doesn't pick what its army can't sense",
          "[weapon][intel]") {
    Scene s("explored", "RULEUMT_Land");
    s.ticks(35);
    CHECK(s.weapon->target_entity_id == 0);
}

TEST_CASE("It picks what its army sees", "[weapon][intel]") {
    Scene s("explored", "RULEUMT_Land");
    s.sim.add_temp_vision(0, 74.0f, 64.0f, 5.0f, 10.0f);
    s.ticks(35);
    CHECK(s.weapon->target_entity_id == s.enemy->entity_id());
}

TEST_CASE("A unit gone from sight is dropped; a structure once seen is kept",
          "[weapon][intel]") {
    SECTION("a unit") {
        Scene s("explored", "RULEUMT_Land");
        s.sim.add_temp_vision(0, 74.0f, 64.0f, 5.0f, 1.0f);
        s.ticks(5);
        REQUIRE(s.weapon->target_entity_id == s.enemy->entity_id());
        s.ticks(30); // the sight is gone
        CHECK(s.weapon->target_entity_id == 0);
    }
    SECTION("a structure") {
        Scene s("explored", "RULEUMT_None");
        s.sim.add_temp_vision(0, 74.0f, 64.0f, 5.0f, 1.0f);
        s.ticks(5);
        REQUIRE(s.weapon->target_entity_id == s.enemy->entity_id());
        s.ticks(30);
        CHECK(s.weapon->target_entity_id == s.enemy->entity_id());
    }
}

TEST_CASE("With the fog off every unit is known, as in Moho", "[weapon][intel]") {
    Scene s("none", "RULEUMT_Land");
    s.ticks(35);
    CHECK(s.weapon->target_entity_id == s.enemy->entity_id());
}
