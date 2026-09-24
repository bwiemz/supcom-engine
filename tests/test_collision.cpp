// Collision (M201d): the shapes shots meet, the query that finds them, and a
// shot's sweep through a tick.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "sim/collision.hpp"
#include "sim/entity_registry.hpp"
#include "sim/manipulator.hpp"
#include "sim/projectile.hpp"
#include "sim/shield.hpp"
#include "sim/unit.hpp"
#include "sim/weapon.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

using Catch::Approx;
using osc::f32;
using osc::u32;
using osc::sim::CollisionShape;
using osc::sim::CollisionShapeType;
using osc::sim::EntityRegistry;
using osc::sim::Projectile;
using osc::sim::Quaternion;
using osc::sim::Unit;
using osc::sim::Vector3;

namespace {

CollisionShape box(f32 hx, f32 hy, f32 hz, f32 cy = 0) {
    CollisionShape s;
    s.type = CollisionShapeType::BOX;
    s.cy = cy;
    s.sx = hx;
    s.sy = hy;
    s.sz = hz;
    return s;
}

CollisionShape sphere(f32 r) {
    CollisionShape s;
    s.type = CollisionShapeType::SPHERE;
    s.sx = r;
    return s;
}

u32 add_unit(EntityRegistry& reg, const Vector3& at, const CollisionShape& shape, int army = 0) {
    auto u = std::make_unique<Unit>();
    u->set_position(at);
    u->set_army(army);
    u->set_default_collision_shape(shape);
    return reg.register_entity(std::move(u));
}

Projectile* add_shot(EntityRegistry& reg, const Vector3& at, const Vector3& velocity,
                     u32 launcher = 0) {
    auto p = std::make_unique<Projectile>();
    p->set_position(at);
    p->velocity = velocity;
    p->launcher_id = launcher;
    return static_cast<Projectile*>(reg.find(reg.register_entity(std::move(p))));
}

} // namespace

TEST_CASE("a segment enters a box where it crosses its face", "[collision]") {
    const auto s = box(1, 1, 1);
    const Quaternion level{};
    const auto t = osc::sim::segment_enters(s, {0, 0, 0}, level, {-3, 0, 0}, {1, 0, 0});
    REQUIRE(t);
    CHECK(*t == Approx(0.5f)); // x = -1 of -3..1
    CHECK_FALSE(osc::sim::segment_enters(s, {0, 0, 0}, level, {-3, 2, 0}, {3, 2, 0}));
    CHECK_FALSE(osc::sim::segment_enters(s, {0, 0, 0}, level, {-3, 0, 0}, {-2, 0, 0}));
    // Starting inside: a hit at once, unless only coming in counts.
    CHECK(osc::sim::segment_enters(s, {0, 0, 0}, level, {0, 0, 0}, {3, 0, 0}) == 0.0f);
    CHECK_FALSE(osc::sim::segment_enters(s, {0, 0, 0}, level, {0, 0, 0}, {3, 0, 0}, false));
}

TEST_CASE("a box turns with its entity", "[collision]") {
    // Long along its own z, turned 90 degrees: long along the world's x.
    const auto s = box(0.25f, 1, 3);
    const Quaternion turned = osc::sim::euler_to_quat(3.14159265f * 0.5f, 0, 0);
    const auto along_x =
        osc::sim::segment_enters(s, {0, 0, 0}, turned, {2.5f, 0, -5}, {2.5f, 0, 5});
    const auto along_z =
        osc::sim::segment_enters(s, {0, 0, 0}, turned, {-5, 0, 2.5f}, {5, 0, 2.5f});
    CHECK(along_x);
    CHECK_FALSE(along_z);
}

TEST_CASE("a segment enters a sphere at its surface", "[collision]") {
    const auto s = sphere(2);
    const Quaternion level{};
    const auto t = osc::sim::segment_enters(s, {10, 0, 0}, level, {0, 0, 0}, {10, 0, 0});
    REQUIRE(t);
    CHECK(*t == Approx(0.8f));
    CHECK_FALSE(osc::sim::segment_enters(s, {10, 0, 0}, level, {0, 3, 0}, {20, 3, 0}));
    // Leaving from inside is no hit on a shield.
    CHECK_FALSE(osc::sim::segment_enters(s, {10, 0, 0}, level, {10, 0, 0}, {20, 0, 0}, false));
    // Heading away, outside it.
    CHECK_FALSE(osc::sim::segment_enters(s, {10, 0, 0}, level, {13, 0, 0}, {20, 0, 0}));
}

TEST_CASE("the collider query finds shapes near a path, large ones from afar", "[collision]") {
    EntityRegistry reg;
    reg.init_spatial_grid(512, 512);
    const u32 near = add_unit(reg, {100, 0, 102}, box(1, 1, 1));
    add_unit(reg, {100, 0, 150}, box(1, 1, 1));               // far off the path
    add_unit(reg, {101, 0, 100}, CollisionShape{});           // no shape
    const u32 big = add_unit(reg, {100, 0, 130}, sphere(30)); // a shield's reach
    std::vector<u32> out;
    reg.collect_colliders(95, 100, 105, 100, out);
    CHECK(out == std::vector<u32>{near, big});

    // A shape set later is tracked; 'None' drops it.
    reg.find(big)->set_collision_shape(CollisionShape{});
    reg.collect_colliders(95, 100, 105, 100, out);
    CHECK(out == std::vector<u32>{near});
    reg.find(big)->revert_collision_shape();
    reg.collect_colliders(95, 100, 105, 100, out);
    CHECK(out == std::vector<u32>{near, big});
}

TEST_CASE("a shot stops at the first thing in its way", "[collision]") {
    EntityRegistry reg;
    reg.init_spatial_grid(512, 512);
    const u32 shooter = add_unit(reg, {100, 0, 100}, box(1, 1, 1, 1));
    add_unit(reg, {100, 0, 106}, box(1, 1, 1, 1));
    add_unit(reg, {100, 0, 104}, box(1, 1, 1, 1)); // nearer, though registered later
    // From inside its launcher, which it passes, at 60 u/s: both boxes lie
    // on this tick's path (100..106).
    Projectile* shot = add_shot(reg, {100, 1, 100}, {0, 0, 60}, shooter);
    shot->update(0.1, reg, nullptr);
    // It met the nearer box's face at z = 103.
    CHECK(shot->destroyed());
    CHECK(shot->position().z == Approx(103.0f));
}

TEST_CASE("a shot with entity collision off flies through", "[collision]") {
    EntityRegistry reg;
    reg.init_spatial_grid(512, 512);
    add_unit(reg, {100, 0, 102}, box(1, 1, 1, 1));
    Projectile* shot = add_shot(reg, {100, 1, 100}, {0, 0, 40});
    shot->collide_entity = false;
    shot->update(0.1, reg, nullptr);
    CHECK_FALSE(shot->destroyed());
    CHECK(shot->position().z == Approx(104.0f));
}

TEST_CASE("a shield stops shots coming in, not going out", "[collision]") {
    EntityRegistry reg;
    reg.init_spatial_grid(512, 512);
    auto shield = std::make_unique<osc::sim::Shield>();
    shield->set_position({100, 0, 100});
    shield->set_collision_shape(sphere(10));
    reg.register_entity(std::move(shield));

    Projectile* out = add_shot(reg, {100, 1, 105}, {0, 0, 80});
    out->update(0.1, reg, nullptr);
    CHECK_FALSE(out->destroyed());

    Projectile* in = add_shot(reg, {100, 1, 115}, {0, 0, -80});
    in->update(0.1, reg, nullptr);
    CHECK(in->destroyed());
    CHECK(in->position().z == Approx(100.0f + std::sqrt(100.0f - 1.0f)).margin(1e-3));
}

TEST_CASE("firing randomness scatters over a circle that grows with range", "[collision]") {
    EntityRegistry reg;
    const u32 owner_id = add_unit(reg, {0, 0, 0}, box(1, 1, 1));
    auto& owner = static_cast<Unit&>(*reg.find(owner_id));
    osc::sim::Weapon w;
    w.muzzle_velocity = 30;
    w.max_range = 24;
    w.firing_randomness = 1.2f;
    // With no target it fires along its facing (+z) to its reach: a circle
    // of radius 1.2 x 24 / 12 = 2.4 about (0, 0, 24).
    f32 widest = 0;
    for (int i = 0; i < 200; ++i) {
        const Projectile* p = w.launch(owner, {0, 0, 0}, nullptr, reg, nullptr, false);
        REQUIRE(p);
        const f32 off = std::hypot(p->target_position.x, p->target_position.z - 24.0f);
        CHECK(off <= 2.4f + 1e-3f);
        widest = std::max(widest, off);
    }
    CHECK(widest > 2.0f); // it fills the circle
}

TEST_CASE("a bomb falls at Moho's gravity; a straight shot doesn't", "[collision]") {
    EntityRegistry reg;
    const u32 owner_id = add_unit(reg, {0, 20, 0}, box(1, 1, 1));
    auto& owner = static_cast<Unit&>(*reg.find(owner_id));
    osc::sim::Weapon bomb;
    bomb.need_compute_bomb_drop = true;
    const Projectile* dropped = bomb.launch(owner, {0, 20, 0}, nullptr, reg, nullptr, false);
    REQUIRE(dropped);
    CHECK(dropped->ballistic_accel == Approx(-Projectile::GRAVITY));

    osc::sim::Weapon gun;
    gun.muzzle_velocity = 30;
    const Projectile* shot = gun.launch(owner, {0, 20, 0}, nullptr, reg, nullptr, false);
    REQUIRE(shot);
    CHECK(shot->ballistic_accel == 0.0f);
}

TEST_CASE("a weapon's lifetime for its shots overrides the default", "[collision]") {
    EntityRegistry reg;
    const u32 owner_id = add_unit(reg, {0, 0, 0}, box(1, 1, 1));
    auto& owner = static_cast<Unit&>(*reg.find(owner_id));
    osc::sim::Weapon w;
    w.muzzle_velocity = 20;
    w.max_range = 40;
    // With neither, a shot lives its flight to its reach and two seconds.
    const Projectile* plain = w.launch(owner, {0, 0, 0}, nullptr, reg, nullptr, false);
    REQUIRE(plain);
    CHECK(plain->lifetime == Approx(40.0f / 20.0f + 2.0f));
    // ProjectileLifetime sets it.
    w.projectile_lifetime = 7;
    const Projectile* timed = w.launch(owner, {0, 0, 0}, nullptr, reg, nullptr, false);
    REQUIRE(timed);
    CHECK(timed->lifetime == Approx(7.0f));
    // The multiplier wins: 1.15 x 40 / 20.
    w.projectile_lifetime_multiplier = 1.15f;
    const Projectile* scaled = w.launch(owner, {0, 0, 0}, nullptr, reg, nullptr, false);
    REQUIRE(scaled);
    CHECK(scaled->lifetime == Approx(1.15f * 40.0f / 20.0f));
}

TEST_CASE("a leading weapon aims where a missile will be", "[collision]") {
    EntityRegistry reg;
    const u32 owner_id = add_unit(reg, {0, 0, 0}, box(1, 1, 1));
    const auto& owner = static_cast<const Unit&>(*reg.find(owner_id));
    // A missile 40 east, flying north at 10.
    Projectile* missile = add_shot(reg, {40, 0, 0}, {0, 0, 10});
    osc::sim::Weapon w;
    w.muzzle_velocity = 100;
    CHECK(w.aim_point(*missile, owner.position()).z == Approx(0.0f));
    // Leading: 0.4 s to it, then refined once, a little further north.
    w.lead_target = true;
    const Vector3 at = w.aim_point(*missile, owner.position());
    CHECK(at.z > 4.0f);
    CHECK(at.z < 4.2f);
}

TEST_CASE("a point's distance from a shape is negative inside it", "[collision]") {
    const Quaternion level{};
    const auto s = sphere(10);
    CHECK(osc::sim::shape_distance(s, {0, 0, 0}, level, {13, 0, 0}) == Approx(3.0f));
    CHECK(osc::sim::shape_distance(s, {0, 0, 0}, level, {0, 4, 0}) == Approx(-6.0f));
    // A box 2 x 2 x 2 standing on the ground (centre 1 up).
    const auto b = box(1, 1, 1, 1);
    CHECK(osc::sim::shape_distance(b, {0, 0, 0}, level, {4, 1, 0}) == Approx(3.0f));
    CHECK(osc::sim::shape_distance(b, {0, 0, 0}, level, {4, 6, 0}) ==
          Approx(5.0f)); // past a corner: 3-4-5
    CHECK(osc::sim::shape_distance(b, {0, 0, 0}, level, {0, 1.5f, 0}) == Approx(-0.5f));
    CHECK(osc::sim::shape_distance(CollisionShape{}, {0, 0, 0}, level, {0, 0, 0}) > 1e30f);
}
