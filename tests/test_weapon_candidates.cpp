#include <catch2/catch_test_macros.hpp>

#include "map/heightmap.hpp"
#include "map/terrain.hpp"
#include "sim/manipulator.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/unit_command.hpp"
#include "sim/weapon.hpp"
#include "support/tick_until.hpp"

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
using osc::test::tick_until;

namespace {

struct LuaGuard {
    lua_State* L = lua_open();
    ~LuaGuard() { lua_close(L); }
};

Unit* unit(SimState& sim, int army, f32 x, f32 z, const char* motion = "RULEUMT_None") {
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

/// A gun of army 0 at (64, 64) facing +z, and an enemy of army 1 10 east of it.
struct Scene {
    LuaGuard g;
    SimState sim{g.L, nullptr};
    Unit* gun = nullptr;
    Unit* enemy = nullptr;
    Weapon* weapon = nullptr;
    explicit Scene(const std::string& fog = "none", bool ridge = false,
                   const char* enemy_motion = "RULEUMT_None") {
        std::vector<osc::u16> heights(129 * 129, 1000);
        if (ridge) {
            for (int z = 0; z <= 128; ++z) {
                heights[static_cast<size_t>(z) * 129 + 69] = 4000;
            }
        }
        osc::map::Heightmap hm(128, 128, 1.0f / 128.0f, std::move(heights));
        sim.set_terrain(std::make_unique<osc::map::Terrain>(std::move(hm), 0.0f, false));
        sim.build_pathfinding_grid();
        sim.add_army("ARMY_1", "ARMY_1");
        sim.add_army("ARMY_2", "ARMY_2");
        sim.set_fog_of_war(fog);
        sim.set_victory_condition("sandbox");
        gun = unit(sim, 0, 64.0f, 64.0f);
        enemy = unit(sim, 1, 74.0f, 64.0f, enemy_motion);
        auto w = std::make_unique<Weapon>();
        w->max_range = 20.0f;
        w->damage = 10.0f;
        w->target_priorities = {osc::sim::parse_category_list("ALLUNITS")};
        gun->add_weapon(std::move(w));
        weapon = gun->weapons().back().get();
    }
    void attach_enemy() {
        Unit* holder = unit(sim, 1, 74.0f, 64.0f);
        holder->set_do_not_target(true);
        enemy->set_parent(holder->entity_id(), 0);
    }
    void ticks(int n) {
        for (int t = 0; t < n; ++t) {
            weapon->fire_clock = 1000000;
            sim.tick();
        }
    }
    bool picks(const Unit* u, int n = 5) {
        ticks(n);
        return weapon->target_entity_id == u->entity_id();
    }
};

} // namespace

TEST_CASE("A weapon passes over an attached unit in the air or unfinished, and an unfinished "
          "aircraft",
          "[weapon][targets]") {
    struct Case {
        const char* what;
        bool attached;
        const char* layer;
        bool being_built;
        bool picked;
    };
    for (const Case& c : {
             Case{"a unit", false, "Land", false, true},
             Case{"an aircraft", false, "Air", false, true},
             Case{"an unfinished structure", false, "Land", true, true},
             Case{"an unfinished aircraft", false, "Air", true, false},
             Case{"an unfinished unit on its factory", true, "Land", true, false},
             Case{"an unfinished aircraft on its factory", true, "Air", true, false},
             Case{"cargo in the air", true, "Air", false, false},
         }) {
        CAPTURE(c.what);
        Scene s;
        s.enemy->set_layer(c.layer);
        s.enemy->set_is_being_built(c.being_built);
        if (c.attached) {
            s.attach_enemy();
        }
        CHECK(s.picks(s.enemy) == c.picked);
    }
}

TEST_CASE("A weapon passes over a unit on the ground off the map, and one its unit is to reclaim",
          "[weapon][targets]") {
    SECTION("off the map on the ground") {
        Scene s;
        s.sim.set_playable_rect(0.0f, 0.0f, 70.0f, 128.0f);
        CHECK_FALSE(s.picks(s.enemy));
    }
    SECTION("off the map in the air") {
        Scene s;
        s.sim.set_playable_rect(0.0f, 0.0f, 70.0f, 128.0f);
        s.enemy->set_layer("Air");
        CHECK(s.picks(s.enemy));
    }
    SECTION("a reclaim queued behind a guard") {
        Scene s;
        const Unit* ward = unit(s.sim, 0, 60.0f, 64.0f);
        osc::sim::UnitCommand guard;
        guard.type = osc::sim::CommandType::Guard;
        guard.target_id = ward->entity_id();
        guard.target_pos = ward->position();
        osc::sim::UnitCommand reclaim;
        reclaim.type = osc::sim::CommandType::Reclaim;
        reclaim.target_id = s.enemy->entity_id();
        reclaim.target_pos = s.enemy->position();
        s.gun->push_command(guard, true);
        s.gun->push_command(reclaim, false);
        CHECK_FALSE(s.picks(s.enemy));
        CHECK(s.gun->command_queue().size() == 2);
    }
}

TEST_CASE("A weapon with no target priorities picks no unit", "[weapon][targets]") {
    Scene s;
    s.weapon->target_priorities.clear();
    s.ticks(5);
    CHECK_FALSE(s.weapon->target_entity_id);
}

TEST_CASE("A turreted weapon takes the enemy nearest its line of fire, another the nearest",
          "[weapon][targets]") {
    for (const bool turreted : {false, true}) {
        CAPTURE(turreted);
        Scene s;
        s.weapon->turreted = turreted;
        s.enemy->set_position({64.0f, 8.3f, 56.0f});
        const Unit* ahead = unit(s.sim, 1, 64.0f, 82.0f);
        CHECK(s.picks(turreted ? ahead : s.enemy));
    }
}

TEST_CASE("A mobile unit's AutoInitiateAttackCommand or SlavedToBody weapon takes an enemy out of "
          "its arc, another doesn't",
          "[weapon][targets]") {
    struct Case {
        bool mobile;
        bool auto_initiate;
        bool slaved;
        bool picked;
    };
    for (const Case& c : {
             Case{true, false, false, false},
             Case{true, true, false, true},
             Case{true, false, true, true},
             Case{false, true, false, false},
         }) {
        CAPTURE(c.mobile, c.auto_initiate, c.slaved);
        Scene s;
        if (c.mobile) {
            s.gun->set_motion_type("RULEUMT_Land");
        }
        s.weapon->heading_arc_range = 45.0f;
        s.weapon->auto_initiate_attack_command = c.auto_initiate;
        s.weapon->slaved_to_body = c.slaved;
        s.enemy->set_position({64.0f, 8.3f, 54.0f});
        CHECK(s.picks(s.enemy, 1) == c.picked);
    }
}

TEST_CASE("A weapon takes an enemy its army has seen over a nearer one it knows only by radar",
          "[weapon][targets][intel]") {
    Scene s("explored", false, "RULEUMT_Land");
    s.weapon->target_check_period = 1;
    const Unit* seen = unit(s.sim, 1, 64.0f, 80.0f, "RULEUMT_Land");
    s.sim.add_temp_vision(0, 64.0f, 80.0f, 5.0f, 1.0f);
    tick_until([&] { s.ticks(1); }, "the weapon takes the enemy in sight", 70,
               [&] { return s.weapon->target_entity_id == seen->entity_id(); });
    tick_until([&] { s.ticks(1); }, "the weapon drops it, gone from sight", 70,
               [&] { return !s.weapon->target_entity_id; });
    s.gun->add_intel("Radar", 40.0f);
    s.gun->enable_intel("Radar");
    tick_until([&] { s.ticks(1); }, "the weapon takes an enemy on radar", 70,
               [&] { return static_cast<bool>(s.weapon->target_entity_id); });
    CHECK(s.weapon->target_entity_id == seen->entity_id());
}

TEST_CASE("A weapon passes over an enemy the ground hides, unless WeaponTerrainBlockageTest is off",
          "[weapon][targets]") {
    for (const bool ridge : {false, true}) {
        for (const bool test : {true, false}) {
            CAPTURE(ridge, test);
            Scene s("none", ridge);
            s.sim.con_vars().weapon_terrain_blockage_test = test;
            CHECK(s.picks(s.enemy) == (!ridge || !test));
        }
    }
}
