// What a weapon's script hears of its target, as Moho's UnitWeapon::SetTarget tells it (faf-re
// UnitWeapon.cpp, 0x006D5F30; Steam 0x6DC9B0): OnGotTarget only for a target after none,
// OnLostTarget only for none after a target, nothing for one target after another. A target that
// dies keeps its slot until the weapon next looks (CAcquireTargetTask::Execute); under hold fire
// that look drops a target the weapon doesn't keep, and finds no other.

#include <catch2/catch_test_macros.hpp>

#include "map/heightmap.hpp"
#include "map/terrain.hpp"
#include "sim/entity_registry.hpp"
#include "sim/manipulator.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/weapon.hpp"
#include "support/tick_until.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <memory>
#include <string>
#include <utility>
#include <vector>

using osc::f32;
using osc::sim::EntityId;
using osc::sim::SimState;
using osc::sim::Unit;
using osc::sim::Weapon;
using osc::test::tick_until;

namespace {

struct LuaGuard {
    lua_State* L = lua_open();
    ~LuaGuard() { lua_close(L); }
};

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

// A tank whose gun (range 30, a look every 5 ticks) has a script that notes what it hears, and two
// enemies: one 10 off, the other at `far_x` (in range at 84, 20 off).
struct Scene {
    LuaGuard g;
    SimState sim{g.L, nullptr};
    Unit* tank = nullptr;
    Unit* near = nullptr;
    Unit* far = nullptr;
    Weapon* gun = nullptr;

    explicit Scene(f32 far_x = 84.0f) {
        std::vector<osc::u16> heights(129 * 129, 1000);
        osc::map::Heightmap hm(128, 128, 1.0f / 128.0f, std::move(heights));
        sim.set_terrain(std::make_unique<osc::map::Terrain>(std::move(hm), 0.0f, false));
        sim.build_pathfinding_grid();
        sim.add_army("ARMY_1", "ARMY_1");
        sim.add_army("ARMY_2", "ARMY_2");
        sim.set_fog_of_war("none");
        sim.set_victory_condition("sandbox");
        tank = unit(sim, 0, 64.0f, 64.0f);
        near = unit(sim, 1, 74.0f, 64.0f);
        far = unit(sim, 1, far_x, 64.0f);
        REQUIRE(lua_dostring(g.L, "heard = ''"
                                  " gun = {OnGotTarget = function() heard = heard .. 'got ' end,"
                                  " OnLostTarget = function() heard = heard .. 'lost ' end}") == 0);
        auto w = std::make_unique<Weapon>();
        w->target_priorities = {osc::sim::parse_category_list("ALLUNITS")};
        w->max_range = 30.0f;
        w->damage = 10.0f;
        w->target_check_period = 5;
        w->always_recheck_target = false;
        lua_pushstring(g.L, "gun");
        lua_gettable(g.L, LUA_GLOBALSINDEX);
        w->lua_table_ref = luaL_ref(g.L, LUA_REGISTRYINDEX);
        w->script_class = true;
        tank->add_weapon(std::move(w));
        gun = tank->weapons().back().get();
    }

    // A tick in which the gun can't fire: only its targeting runs.
    void tick() {
        gun->fire_clock = 1000000;
        sim.tick();
    }

    std::string heard() {
        lua_pushstring(g.L, "heard");
        lua_gettable(g.L, LUA_GLOBALSINDEX);
        std::string text = lua_isstring(g.L, -1) ? lua_tostring(g.L, -1) : "";
        lua_pop(g.L, 1);
        return text;
    }

    // The gun takes the near enemy.
    void engage() {
        tick_until([&] { tick(); }, "the gun takes the near enemy", 10,
                   [&] { return gun->target_entity_id == near->entity_id(); });
        REQUIRE(heard() == "got ");
    }

    // `u` dies (is destroyed: gone from the world), and a tick runs.
    void kill(Unit*& u) {
        sim.entity_registry().destroy(*u);
        u = nullptr;
        tick();
    }
};

} // namespace

TEST_CASE("A gun whose target dies turns to the next without telling its script",
          "[weapon][target-events]") {
    Scene s;
    s.engage();
    const EntityId far_id = s.far->entity_id();
    s.kill(s.near);
    // Its slot holds the dead target until its next look
    CHECK_FALSE(s.gun->target_entity_id);
    CHECK(s.gun->target_held);
    CHECK_FALSE(s.gun->has_target());
    CHECK(s.heard() == "got ");
    const int ticks = tick_until([&] { s.tick(); }, "the gun turns to the other enemy", 10,
                                 [&] { return s.gun->target_entity_id == far_id; });
    CHECK(ticks > 0);
    CHECK_FALSE(s.gun->target_held);
    CHECK(s.heard() == "got ");
}

TEST_CASE("A gun whose target dies with no other in range drops it at its next look, with "
          "OnLostTarget",
          "[weapon][target-events]") {
    Scene s(120.0f);
    s.engage();
    s.kill(s.near);
    CHECK(s.gun->target_held);
    CHECK(s.heard() == "got ");
    tick_until([&] { s.tick(); }, "the gun's next look", 10, [&] { return !s.gun->target_held; });
    CHECK_FALSE(s.gun->has_target());
    CHECK(s.heard() == "got lost ");
}

TEST_CASE("A gun whose target leaves its range for another enemy tells its script nothing",
          "[weapon][target-events]") {
    Scene s;
    s.engage();
    const EntityId far_id = s.far->entity_id();
    s.near->set_position({110.0f, 7.8f, 64.0f});
    tick_until([&] { s.tick(); }, "the gun turns to the other enemy", 10,
               [&] { return s.gun->target_entity_id == far_id; });
    CHECK(s.heard() == "got ");
}

TEST_CASE("A gun turning from a target ReTargetOnMiss gave up to another tells its script nothing",
          "[weapon][target-events]") {
    Scene s;
    s.gun->retarget_on_miss = true;
    s.engage();
    for (int i = 0; i < 11; ++i) {
        s.gun->note_impact(*s.tank, true, s.sim.entity_registry(), &s.sim);
    }
    REQUIRE(s.gun->target_held);
    s.gun->target_check_clock = 0;
    s.tick();
    CHECK(s.gun->target_entity_id == s.far->entity_id());
    CHECK(s.heard() == "got ");
}

TEST_CASE("An order that drops a gun's dead target tells its script OnLostTarget",
          "[weapon][target-events]") {
    // Moho's CAiAttackerImpl::SetDesiredTarget clears each weapon's target through SetTarget, and
    // a slot holding a dead target had one
    Scene s;
    s.engage();
    s.kill(s.near);
    REQUIRE(s.gun->target_held);
    s.gun->drop_target(s.g.L);
    CHECK_FALSE(s.gun->target_held);
    CHECK(s.heard() == "got lost ");
    s.gun->drop_target(s.g.L);
    CHECK(s.heard() == "got lost ");
}

TEST_CASE("Under hold fire a gun keeps a target in range, takes no other, and drops one that "
          "leaves, with OnLostTarget",
          "[weapon][target-events]") {
    Scene s;
    s.engage();
    s.tank->set_fire_state(1);
    for (int i = 0; i < 12; ++i) {
        s.tick();
    }
    CHECK(s.gun->target_entity_id == s.near->entity_id());
    CHECK(s.heard() == "got ");

    s.near->set_position({110.0f, 7.8f, 64.0f});
    tick_until([&] { s.tick(); }, "the gun drops the target", 10,
               [&] { return !s.gun->has_target(); });
    CHECK(s.heard() == "got lost ");
    for (int i = 0; i < 12; ++i) {
        s.tick();
    }
    CHECK_FALSE(s.gun->has_target());
    CHECK(s.heard() == "got lost ");
}

TEST_CASE("Under hold fire a gun drops a dead target at its next look, with OnLostTarget",
          "[weapon][target-events]") {
    Scene s;
    s.engage();
    s.tank->set_fire_state(1);
    s.kill(s.near);
    CHECK(s.gun->target_held);
    tick_until([&] { s.tick(); }, "the gun's next look", 10, [&] { return !s.gun->target_held; });
    CHECK_FALSE(s.gun->has_target());
    CHECK(s.heard() == "got lost ");
}

TEST_CASE("Under hold fire an AlwaysRecheckTarget gun drops its target at its next look",
          "[weapon][target-events]") {
    // Moho's acquire task keeps a target only for a weapon without AlwaysRecheckTarget
    Scene s;
    s.gun->always_recheck_target = true;
    s.engage();
    s.tank->set_fire_state(1);
    tick_until([&] { s.tick(); }, "the gun drops the target", 10,
               [&] { return !s.gun->has_target(); });
    CHECK(s.heard() == "got lost ");
}
