#include <catch2/catch_test_macros.hpp>

#include "map/heightmap.hpp"
#include "map/terrain.hpp"
#include "sim/manipulator.hpp"
#include "sim/projectile.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/weapon.hpp"
#include "support/tick_until.hpp"

extern "C" {
#include <lua.h>
}

#include <memory>
#include <vector>

// Weapons that shoot projectiles down (TargetType RULEWTT_Projectile: missile defences,
// anti-torpedoes, anti-nukes) pick their targets with Moho's CAiAttackerImpl::TrackToTarget,
// on each TargetCheckInterval of CAcquireTargetTask.

using osc::f32;
using osc::u32;
using osc::sim::Projectile;
using osc::sim::SimState;
using osc::sim::Unit;
using osc::sim::Weapon;
using osc::test::tick_until;

namespace {

struct LuaGuard {
    lua_State* L = lua_open();
    ~LuaGuard() { lua_close(L); }
};

/// The height missiles fly at, over ground at about 7.8.
constexpr f32 kAir = 30.0f;

Unit* unit(SimState& sim, int army, f32 x, f32 z) {
    auto u = std::make_unique<Unit>();
    u->set_army(army);
    u->set_motion_type("RULEUMT_None");
    u->set_position({x, 8.3f, z});
    u->set_max_health(100.0f);
    u->set_health(100.0f);
    auto* raw = u.get();
    sim.entity_registry().register_entity(std::move(u));
    return raw;
}

/// A flat map, with water at 20 when `water`; army 1 defends against army 0's missiles.
struct Scene {
    LuaGuard g;
    SimState sim{g.L, nullptr};
    std::vector<Weapon*> guns;

    explicit Scene(bool water = false) {
        std::vector<osc::u16> heights(129 * 129, 1000);
        osc::map::Heightmap hm(128, 128, 1.0f / 128.0f, std::move(heights));
        sim.set_terrain(
            std::make_unique<osc::map::Terrain>(std::move(hm), water ? 20.0f : 0.0f, water));
        sim.build_pathfinding_grid();
        sim.add_army("ARMY_1", "ARMY_1");
        sim.add_army("ARMY_2", "ARMY_2");
        sim.set_fog_of_war("none");
        sim.set_victory_condition("sandbox");
    }
    /// A missile defence on `u`: MaxRadius `max_radius`, TrackingRadius 2 (it looks twice as
    /// far), checking every tick, against what flies.
    Weapon* gun(Unit* u, f32 max_radius = 30.0f, const char* caps = "Air") {
        auto w = std::make_unique<Weapon>();
        w->targets_projectiles = true;
        w->max_range = max_radius;
        w->tracking_radius = 2.0f;
        w->damage = 1.0f;
        w->target_check_period = 1;
        w->fire_target_layer_caps = osc::sim::parse_layer_caps(caps);
        u->add_weapon(std::move(w));
        guns.push_back(u->weapons().back().get());
        return guns.back();
    }
    /// A unit of army 1 with one missile defence.
    Unit* defence(f32 x, f32 z) {
        Unit* u = unit(sim, 1, x, z);
        gun(u);
        return u;
    }
    /// A missile of army 0 holding still at (x, y, z), with DesiredShooterCap `cap`.
    Projectile* missile(f32 x, f32 z, u32 cap = 3, f32 y = kAir) {
        auto p = std::make_unique<Projectile>();
        p->set_army(0);
        p->set_position({x, y, z});
        p->lifetime_end = 1000000;
        p->collide_entity = false;
        p->collide_surface = false;
        const auto* terrain = sim.terrain();
        p->in_water = terrain->has_water() && y < terrain->water_elevation();
        auto info = std::make_shared<osc::sim::ProjectileBlueprintInfo>();
        info->categories = {"ALLPROJECTILES", "MISSILE"};
        info->desired_shooter_cap = cap;
        p->set_blueprint_info(std::move(info));
        auto* raw = p.get();
        sim.entity_registry().register_entity(std::move(p));
        return raw;
    }
    static void move(Projectile* p, f32 x, f32 z) {
        p->set_position({x, kAir, z});
        p->place_now();
    }
    /// Run the sim, every gun holding its fire.
    void ticks(int n) {
        for (int t = 0; t < n; ++t) {
            for (Weapon* w : guns) {
                w->fire_clock = 1000000;
            }
            sim.tick();
        }
    }
};

bool aims_at(const Weapon* w, const Projectile* p) {
    return w->target_entity_id == p->entity_id();
}

bool has_target(const Weapon* w) {
    return static_cast<bool>(w->target_entity_id);
}

} // namespace

TEST_CASE("A missile defence takes a missile only within its MaxRadius, not out to "
          "TrackingRadius times it",
          "[weapon][projectile-targets]") {
    Scene s;
    s.defence(40.0f, 64.0f);
    Weapon* w = s.guns[0];
    // 45 away: in the 60 it looks out to, beyond the 30 it reaches.
    Projectile* m = s.missile(85.0f, 64.0f);
    s.ticks(5);
    CHECK_FALSE(has_target(w));
    Scene::move(m, 65.0f, 64.0f);
    tick_until([&] { s.ticks(1); }, "it takes the missile 25 away", 5,
               [&] { return aims_at(w, m); });
}

TEST_CASE("A missile defence turns to whichever missile is nearest at each check",
          "[weapon][projectile-targets]") {
    Scene s;
    s.defence(40.0f, 64.0f);
    Weapon* w = s.guns[0];
    Projectile* first = s.missile(65.0f, 64.0f);
    tick_until([&] { s.ticks(1); }, "it takes the first missile", 5,
               [&] { return aims_at(w, first); });
    Projectile* second = s.missile(30.0f, 64.0f);
    tick_until([&] { s.ticks(1); }, "it turns to the second missile, nearer", 5,
               [&] { return aims_at(w, second); });
    Scene::move(second, 12.0f, 64.0f);
    Scene::move(first, 45.0f, 64.0f);
    tick_until([&] { s.ticks(1); }, "it turns back to the first, nearer now", 5,
               [&] { return aims_at(w, first); });
}

TEST_CASE("Without AlwaysRecheckTarget a missile defence keeps its missile while it can hit it",
          "[weapon][projectile-targets]") {
    Scene s;
    s.defence(40.0f, 64.0f);
    Weapon* w = s.guns[0];
    w->always_recheck_target = false;
    Projectile* first = s.missile(65.0f, 64.0f);
    tick_until([&] { s.ticks(1); }, "it takes the first missile", 5,
               [&] { return aims_at(w, first); });
    Projectile* second = s.missile(30.0f, 64.0f);
    s.ticks(5);
    CHECK(aims_at(w, first));
    // Beyond its MaxRadius, though within TrackingRadius times it.
    Scene::move(first, 85.0f, 64.0f);
    tick_until([&] { s.ticks(1); }, "it turns to the second missile", 5,
               [&] { return aims_at(w, second); });
}

TEST_CASE("A silo missile defence takes no missile while its silo is empty",
          "[weapon][projectile-targets]") {
    Scene s;
    Unit* d = s.defence(40.0f, 64.0f);
    Weapon* w = s.guns[0];
    w->counted_projectile = true;
    Projectile* m = s.missile(65.0f, 64.0f);
    s.ticks(5);
    CHECK_FALSE(has_target(w));
    d->give_tactical_silo_ammo(1);
    tick_until([&] { s.ticks(1); }, "loaded, it takes the missile", 5,
               [&] { return aims_at(w, m); });
    SECTION("checking again, emptied, it lets the missile go") {
        d->remove_tactical_silo_ammo(1);
        tick_until([&] { s.ticks(1); }, "it lets the missile go", 5,
                   [&] { return !has_target(w); });
    }
    SECTION("without AlwaysRecheckTarget, emptied, it keeps the missile it can hit") {
        w->always_recheck_target = false;
        d->remove_tactical_silo_ammo(1);
        s.ticks(5);
        CHECK(aims_at(w, m));
    }
}

TEST_CASE("A stunned or Busy unit's missile defence takes no missile",
          "[weapon][projectile-targets]") {
    for (const bool stunned : {true, false}) {
        CAPTURE(stunned);
        Scene s;
        Unit* d = s.defence(40.0f, 64.0f);
        Weapon* w = s.guns[0];
        if (stunned) {
            d->set_stunned(5.0);
        } else {
            d->set_busy(true);
        }
        s.missile(65.0f, 64.0f);
        s.ticks(5);
        CHECK_FALSE(has_target(w));
    }
}

TEST_CASE("A missile defence out of reach doesn't take a missile's last shooter place from one "
          "in reach",
          "[weapon][projectile-targets]") {
    // Anti-nukes against a nuke: DesiredShooterCap 1, and they keep what they can hit.
    Scene s;
    // A, made first, is 45 from the missile: it sees it, but can't hit it.
    s.defence(20.0f, 64.0f);
    s.defence(80.0f, 64.0f);
    Weapon* a = s.guns[0];
    Weapon* b = s.guns[1];
    a->always_recheck_target = false;
    b->always_recheck_target = false;
    Projectile* m = s.missile(65.0f, 64.0f, 1);
    tick_until([&] { s.ticks(1); }, "B takes the missile", 5, [&] { return aims_at(b, m); });
    s.ticks(5);
    CHECK_FALSE(has_target(a));
    CHECK(aims_at(b, m));
}

TEST_CASE("A missile defence holding its fire takes no missile, and leaves a missile's last "
          "shooter place to one that fires",
          "[weapon][projectile-targets]") {
    Scene s;
    Unit* holding = s.defence(50.0f, 64.0f);
    s.defence(80.0f, 64.0f);
    Weapon* a = s.guns[0];
    Weapon* b = s.guns[1];
    a->always_recheck_target = false;
    b->always_recheck_target = false;
    holding->set_fire_state(1);
    Projectile* m = s.missile(65.0f, 64.0f, 1);
    tick_until([&] { s.ticks(1); }, "B takes the missile", 5, [&] { return aims_at(b, m); });
    s.ticks(5);
    CHECK_FALSE(has_target(a));
    CHECK(aims_at(b, m));
}

TEST_CASE("A missile defence set to hold its fire keeps the missile it can hit only without "
          "AlwaysRecheckTarget",
          "[weapon][projectile-targets]") {
    for (const bool recheck : {false, true}) {
        CAPTURE(recheck);
        Scene s;
        Unit* d = s.defence(40.0f, 64.0f);
        Weapon* w = s.guns[0];
        w->always_recheck_target = recheck;
        Projectile* m = s.missile(65.0f, 64.0f);
        tick_until([&] { s.ticks(1); }, "it takes the missile", 5, [&] { return aims_at(w, m); });
        d->set_fire_state(1);
        s.ticks(5);
        CHECK(aims_at(w, m) == !recheck);
    }
}

TEST_CASE("A unit counts once among a missile's shooters, however many of its weapons hold it",
          "[weapon][projectile-targets]") {
    Scene s;
    Unit* u = unit(s.sim, 1, 40.0f, 64.0f);
    s.gun(u);
    s.gun(u);
    s.defence(40.0f, 84.0f);
    for (Weapon* w : s.guns) {
        w->always_recheck_target = false;
    }
    Projectile* m = s.missile(50.0f, 64.0f, 2);
    tick_until(
        [&] { s.ticks(1); }, "all three weapons take the missile", 5,
        [&] { return aims_at(s.guns[0], m) && aims_at(s.guns[1], m) && aims_at(s.guns[2], m); });
}

TEST_CASE("A weapon that lets its missile go takes its unit off the missile's shooters, though "
          "another of its weapons still holds it",
          "[weapon][projectile-targets]") {
    Scene s;
    Unit* u = unit(s.sim, 1, 40.0f, 64.0f);
    Weapon* short_gun = s.gun(u, 15.0f);
    Weapon* long_gun = s.gun(u, 30.0f);
    s.defence(40.0f, 84.0f);
    s.defence(40.0f, 44.0f);
    Weapon* v = s.guns[2];
    Weapon* x = s.guns[3];
    for (Weapon* w : s.guns) {
        w->always_recheck_target = false;
    }
    // Two units' worth of shooters: U's two weapons and V's.
    Projectile* m = s.missile(50.0f, 64.0f, 2);
    tick_until([&] { s.ticks(1); }, "U's weapons and V's take the missile", 5,
               [&] { return aims_at(short_gun, m) && aims_at(long_gun, m) && aims_at(v, m); });
    s.ticks(5);
    CHECK_FALSE(has_target(x));
    // Out of the short gun's reach, which lets it go: U comes off the list, so X has room.
    Scene::move(m, 60.0f, 64.0f);
    tick_until([&] { s.ticks(1); }, "X takes the missile", 5, [&] { return aims_at(x, m); });
    CHECK_FALSE(has_target(short_gun));
    CHECK(aims_at(long_gun, m));
    CHECK(aims_at(v, m));
}

TEST_CASE("A missile defence checking again counts its own unit among the missile's shooters",
          "[weapon][projectile-targets]") {
    // With DesiredShooterCap 1, its own place fills the cap: each check that finds no other
    // missile drops the one it has, and the next takes it again.
    Scene s;
    s.defence(40.0f, 64.0f);
    Weapon* w = s.guns[0];
    w->always_recheck_target = true;
    Projectile* m = s.missile(65.0f, 64.0f, 1);
    tick_until([&] { s.ticks(1); }, "it takes the missile", 5, [&] { return aims_at(w, m); });
    std::vector<bool> held;
    for (int t = 0; t < 4; ++t) {
        s.ticks(1);
        held.push_back(aims_at(w, m));
    }
    CHECK(held == std::vector<bool>{false, true, false, true});
}

TEST_CASE("Under the water a defence needs layer caps Water, above it Air",
          "[weapon][projectile-targets]") {
    Scene s(true);
    Unit* ships = unit(s.sim, 1, 40.0f, 64.0f);
    Weapon* anti_torpedo = s.gun(ships, 30.0f, "Water");
    Weapon* anti_missile = s.gun(ships, 30.0f, "Air");
    Projectile* torpedo = s.missile(60.0f, 64.0f, 3, 15.0f);
    Projectile* missile = s.missile(50.0f, 64.0f);
    tick_until([&] { s.ticks(1); }, "each takes what it can hit", 5,
               [&] { return aims_at(anti_torpedo, torpedo) && aims_at(anti_missile, missile); });
}
