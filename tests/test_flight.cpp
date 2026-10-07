// Projectile flight (roadmap item 3c): Moho's MotionTick and UpdateTracking
// (faf-re Projectile.cpp), and the orientation math they turn with.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "sim/entity_registry.hpp"
#include "sim/flight_math.hpp"
#include "sim/manipulator.hpp"
#include "sim/projectile.hpp"
#include "sim/unit.hpp"

#include <cmath>
#include <memory>

using Catch::Approx;
using osc::f32;
using osc::u32;
using osc::sim::EntityRegistry;
using osc::sim::Projectile;
using osc::sim::Quaternion;
using osc::sim::Unit;
using osc::sim::Vector3;

namespace {

constexpr f32 kDeg = 0.017453292f;

f32 angle_between(const Vector3& a, const Vector3& b) {
    const f32 la = std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
    const f32 lb = std::sqrt(b.x * b.x + b.y * b.y + b.z * b.z);
    const f32 c = (a.x * b.x + a.y * b.y + a.z * b.z) / (la * lb);
    return std::acos(std::clamp(c, -1.0f, 1.0f));
}

f32 length(const Vector3& v) {
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

Projectile* add_shot(EntityRegistry& reg, const Vector3& at, const Vector3& velocity) {
    auto p = std::make_unique<Projectile>();
    p->set_position(at);
    p->velocity = velocity;
    p->set_orientation(osc::sim::coords_orient(velocity));
    p->collide_surface = false;
    p->collide_entity = false;
    p->lifetime = 100;
    return static_cast<Projectile*>(reg.find(reg.register_entity(std::move(p))));
}

u32 add_unit(EntityRegistry& reg, const Vector3& at) {
    auto u = std::make_unique<Unit>();
    u->set_position(at);
    u->set_army(1);
    return reg.register_entity(std::move(u));
}

} // namespace

TEST_CASE("COORDS_Orient faces along a direction, its right axis level", "[flight]") {
    for (const Vector3 dir :
         {Vector3{0, 0, 1}, Vector3{1, 0, 0}, Vector3{0.3f, 0.5f, -0.8f}, Vector3{-2, -1, 0.5f}}) {
        const Quaternion q = osc::sim::coords_orient(dir);
        const Vector3 ahead = osc::sim::forward_of(q);
        CHECK(angle_between(ahead, dir) == Approx(0.0f).margin(1e-3));
        const Vector3 right = osc::sim::quat_rotate(q, Vector3{1, 0, 0});
        CHECK(right.y == Approx(0.0f).margin(1e-5));
    }
    // Straight up: a quarter turn about X
    const Vector3 up = osc::sim::forward_of(osc::sim::coords_orient({0, 5, 0}));
    CHECK(up.y == Approx(1.0f));
}

TEST_CASE("QuatFromVecRot turns a facing toward a direction, at most so far", "[flight]") {
    const Quaternion ahead = osc::sim::coords_orient({0, 0, 1});
    // 10 degrees at most, toward +X (90 away): 10
    const Quaternion turned = osc::sim::turned_toward(ahead, {1, 0, 0}, 10 * kDeg);
    const Vector3 f = osc::sim::forward_of(turned);
    CHECK(angle_between(f, {0, 0, 1}) == Approx(10 * kDeg).margin(1e-4));
    CHECK(f.x > 0);
    CHECK(f.y == Approx(0.0f).margin(1e-5));
    // Within reach: all the way
    const Quaternion all = osc::sim::turned_toward(ahead, {0.1f, 0, 1}, 30 * kDeg);
    CHECK(angle_between(osc::sim::forward_of(all), {0.1f, 0, 1}) == Approx(0.0f).margin(1e-3));
    // A turn of 0 doesn't turn it; a half turn or more isn't capped
    CHECK(angle_between(osc::sim::forward_of(osc::sim::turned_toward(ahead, {1, 0, 0}, 0)),
                        {0, 0, 1}) == Approx(0.0f).margin(1e-5));
    CHECK(angle_between(osc::sim::forward_of(osc::sim::turned_toward(ahead, {1, 0, 0}, 4.0f)),
                        {1, 0, 0}) == Approx(0.0f).margin(1e-3));
}

TEST_CASE("A shell falls on its parabola, as before", "[flight]") {
    EntityRegistry reg;
    Projectile* shell = add_shot(reg, {0, 10, 0}, {0, 5, 20});
    shell->ballistic_accel = -4.9f;
    for (int i = 0; i < 10; ++i) shell->update(0.1, reg, nullptr);
    // One second: y = 10 + 5 - 4.9 / 2
    CHECK(shell->position().y == Approx(10 + 5 - 2.45f).margin(1e-3));
    CHECK(shell->position().z == Approx(20.0f).margin(1e-3));
    CHECK(shell->velocity.y == Approx(5 - 4.9f).margin(1e-4));
}

TEST_CASE("A rocket thrusts the way it faces, its speed capped", "[flight]") {
    EntityRegistry reg;
    // Facing +Z, drifting +X: the thrust adds along +Z only
    Projectile* rocket = add_shot(reg, {0, 10, 0}, {0, 0, 1});
    rocket->velocity = {2, 0, 0};
    rocket->acceleration = 10;
    rocket->max_speed = 0; // none: no cap
    rocket->update(0.1, reg, nullptr);
    CHECK(rocket->velocity.x == Approx(2.0f));
    CHECK(rocket->velocity.z == Approx(1.0f));
    // The cap clamps the whole velocity
    rocket->max_speed = 1.5f;
    rocket->update(0.1, reg, nullptr);
    CHECK(length(rocket->velocity) == Approx(1.5f));
    // At rest it sets off the way it faces (a nuke in its silo)
    Projectile* nuke = add_shot(reg, {0, 0, 0}, {0, 1, 0});
    nuke->velocity = {};
    nuke->acceleration = 3;
    nuke->update(0.1, reg, nullptr);
    CHECK(nuke->velocity.y == Approx(0.3f));
}

TEST_CASE("A velocity-aligned shot turns after its velocity at TurnRate a tick", "[flight]") {
    EntityRegistry reg;
    Projectile* shot = add_shot(reg, {0, 50, 0}, {0, 0, 10});
    shot->velocity_align = true;
    shot->ballistic_accel = -40; // falls fast: its velocity turns down
    shot->turn_rate = 2;         // degrees, per tick in this branch
    shot->update(0.1, reg, nullptr);
    const Vector3 f = osc::sim::forward_of(shot->orientation());
    CHECK(angle_between(f, {0, 0, 1}) == Approx(2 * kDeg).margin(1e-4));
    // With no turn rate it keeps the facing it left with
    Projectile* still = add_shot(reg, {10, 50, 0}, {0, 0, 10});
    still->velocity_align = true;
    still->ballistic_accel = -40;
    still->update(0.1, reg, nullptr);
    CHECK(angle_between(osc::sim::forward_of(still->orientation()), {0, 0, 1}) ==
          Approx(0.0f).margin(1e-5));
}

TEST_CASE("A homing shot turns toward its target at a tenth of TurnRate a tick", "[flight]") {
    EntityRegistry reg;
    const u32 target = add_unit(reg, {100, 10, 0});
    Projectile* missile = add_shot(reg, {0, 10, 0}, {0, 0, 20});
    missile->tracking = true;
    missile->target_entity_id = target;
    missile->has_target_position = true;
    missile->turn_rate = 90; // 9 degrees a tick
    missile->acceleration = 0;
    missile->update(0.1, reg, nullptr);
    const Vector3 f = osc::sim::forward_of(missile->orientation());
    CHECK(angle_between(f, {0, 0, 1}) == Approx(9 * kDeg).margin(1e-4));
    CHECK(f.x > 0);
    // Not a velocity-aligned one: its velocity is as it was (no thrust)
    CHECK(missile->velocity.z == Approx(20.0f));
    // A velocity-aligned one keeps what of its velocity lies along its facing
    missile->velocity_align = true;
    const Vector3 before = missile->velocity;
    missile->update(0.1, reg, nullptr);
    const Vector3 g = osc::sim::forward_of(missile->orientation());
    const f32 along = before.x * g.x + before.y * g.y + before.z * g.z;
    CHECK(missile->velocity.x == Approx(g.x * along).margin(1e-4));
    CHECK(missile->velocity.z == Approx(g.z * along).margin(1e-4));
}

TEST_CASE("A homing shot whose target is gone stops tracking", "[flight]") {
    EntityRegistry reg;
    const u32 target = add_unit(reg, {100, 10, 0});
    Projectile* missile = add_shot(reg, {0, 10, 0}, {0, 0, 20});
    missile->tracking = true;
    missile->target_entity_id = target;
    missile->has_target_position = true;
    missile->turn_rate = 90;
    missile->update(0.1, reg, nullptr);
    reg.find(target)->mark_destroyed();
    const Quaternion facing = missile->orientation();
    missile->update(0.1, reg, nullptr);
    CHECK_FALSE(missile->tracking);
    CHECK_FALSE(missile->has_target_position);
    // Without the latch it turned no further
    CHECK(angle_between(osc::sim::forward_of(missile->orientation()),
                        osc::sim::forward_of(facing)) == Approx(0.0f).margin(1e-5));

    // With it (a target on the ground), it turned once more at the last aim
    const u32 tank = add_unit(reg, {100, 10, 0});
    static_cast<Unit*>(reg.find(tank))->set_layer("Land");
    Projectile* second = add_shot(reg, {0, 10, 0}, {0, 0, 20});
    second->tracking = true;
    second->target_entity_id = tank;
    second->has_target_position = true;
    second->turn_rate = 90;
    second->arm_lost_target_aim(reg);
    CHECK(second->keep_last_aim);
    second->update(0.1, reg, nullptr);
    reg.find(tank)->mark_destroyed();
    const Quaternion was = second->orientation();
    second->update(0.1, reg, nullptr);
    CHECK_FALSE(second->tracking);
    CHECK(angle_between(osc::sim::forward_of(second->orientation()), osc::sim::forward_of(was)) ==
          Approx(9 * kDeg).margin(1e-3));
}

TEST_CASE("Only a target neither in the air nor under the water arms the latch", "[flight]") {
    EntityRegistry reg;
    const u32 plane = add_unit(reg, {100, 30, 0});
    static_cast<Unit*>(reg.find(plane))->set_layer("Air");
    Projectile* aa = add_shot(reg, {0, 10, 0}, {0, 0, 20});
    aa->tracking = true;
    aa->target_entity_id = plane;
    aa->arm_lost_target_aim(reg);
    CHECK_FALSE(aa->keep_last_aim);
    // Nor does a shot that doesn't track
    const u32 tank = add_unit(reg, {100, 10, 0});
    Projectile* shell = add_shot(reg, {0, 10, 0}, {0, 0, 20});
    shell->target_entity_id = tank;
    shell->arm_lost_target_aim(reg);
    CHECK_FALSE(shell->keep_last_aim);
}

TEST_CASE("A leading shot aims where its target will be", "[flight]") {
    EntityRegistry reg;
    // Its target 100 ahead, crossing at 10/s; the shot's top speed 50:
    // about 2 s to get there, so about 20 to the side
    const u32 target = add_unit(reg, {0, 10, 100});
    static_cast<Unit*>(reg.find(target))->set_velocity({10, 0, 0});
    Projectile* missile = add_shot(reg, {0, 10, 0}, {0, 0, 20});
    missile->tracking = true;
    missile->lead_target = true;
    missile->target_entity_id = target;
    missile->has_target_position = true;
    missile->turn_rate = 3600; // turns all the way
    missile->max_speed = 50;
    missile->update(0.1, reg, nullptr);
    const Vector3 f = osc::sim::forward_of(missile->orientation());
    // t1 = 100 / 50 = 2 -> (20, 10, 100); t2 = |P - that| / 50
    const f32 t2 = std::sqrt(20.0f * 20.0f + 100.0f * 100.0f) / 50.0f;
    CHECK(f.x / f.z == Approx(10.0f * t2 / 100.0f).margin(1e-3));
}

TEST_CASE("Zig-zag: an offset redrawn every ZigZagFrequency, kept above the ground", "[flight]") {
    EntityRegistry reg;
    const u32 target = add_unit(reg, {0, 10, 1000});
    Projectile* missile = add_shot(reg, {0, 10, 0}, {0, 0, 20});
    missile->tracking = true;
    missile->target_entity_id = target;
    missile->has_target_position = true;
    missile->turn_rate = 3600;
    missile->max_speed = 20;
    missile->max_zig_zag = 5;
    missile->zig_zag_freq = 0.2f; // 2 ticks
    missile->update(0.1, reg, nullptr, nullptr, 7);
    const Vector3 first = missile->zig_zag_offset;
    CHECK(missile->zig_zag_next_tick == 9);
    CHECK(std::abs(first.x) <= 5.0f);
    CHECK(std::abs(first.z) <= 5.0f);
    CHECK((first.x != 0.0f || first.z != 0.0f));
    // Not redrawn before its tick, redrawn on it
    missile->update(0.1, reg, nullptr, nullptr, 8);
    CHECK(missile->zig_zag_offset.x == first.x);
    missile->update(0.1, reg, nullptr, nullptr, 9);
    CHECK(missile->zig_zag_offset.x != first.x);
    CHECK(missile->zig_zag_next_tick == 11);
    // It steered at a point its top speed ahead plus the offset; never
    // lower than that point
    const Vector3 f = osc::sim::forward_of(missile->orientation());
    CHECK(f.y >= -1e-4f);
}

TEST_CASE("A spinning shot turns in its own frame, about its forward axis if aligned", "[flight]") {
    EntityRegistry reg;
    Projectile* shot = add_shot(reg, {0, 10, 0}, {0, 0, 20});
    shot->velocity_align = true;
    shot->angular_velocity = {3, 4, 0}; // rate 5, reprojected onto forward
    shot->update(0.1, reg, nullptr);
    CHECK(shot->angular_velocity.x == Approx(0.0f));
    CHECK(shot->angular_velocity.z == Approx(5.0f));
    // It still faces ahead, rolled half a radian
    CHECK(angle_between(osc::sim::forward_of(shot->orientation()), {0, 0, 1}) ==
          Approx(0.0f).margin(1e-4));
    const Vector3 up = osc::sim::quat_rotate(shot->orientation(), Vector3{0, 1, 0});
    CHECK(std::atan2(-up.x, up.y) == Approx(0.5f).margin(1e-4));
}
