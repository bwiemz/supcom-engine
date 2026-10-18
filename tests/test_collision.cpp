// Collision (M201d): the shapes shots meet, the query that finds them, and a
// shot's sweep through a tick.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "lua/lua_state.hpp"
#include "sim/bone_data.hpp"
#include "sim/collision.hpp"
#include "sim/entity_registry.hpp"
#include "sim/manipulator.hpp"
#include "sim/projectile.hpp"
#include "sim/sim_random.hpp"
#include "sim/shield.hpp"
#include "sim/unit.hpp"
#include "sim/weapon.hpp"

extern "C" {
#include <lua.h>
}

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>
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

void fly(EntityRegistry& reg, Projectile& p) {
    p.update(0.1, reg, nullptr);
    reg.advance_coords();
}

} // namespace

TEST_CASE("a blueprint's footprint is its own, else its size rounded up", "[collision]") {
    // Moho's REntityBlueprint default: most units and props have no
    // Footprint table.
    osc::lua::LuaState lua;
    lua_State* L = lua.raw();
    REQUIRE(lua.do_string("fp_small = {SizeX = 0.6, SizeZ = 1.2}\n"
                          "fp_own = {SizeX = 3, SizeZ = 3, Footprint = {SizeX = 5, SizeZ = 5}}\n"
                          "fp_half = {SizeX = 0.5, SizeZ = 3.2, Footprint = {SizeX = 2}}\n"
                          "fp_none = {}\n"));
    const auto footprint = [&](const char* name) {
        lua_pushstring(L, name);
        lua_rawget(L, LUA_GLOBALSINDEX);
        const auto fp = osc::sim::blueprint_footprint(L, lua_gettop(L));
        lua_pop(L, 1);
        return fp;
    };
    CHECK(footprint("fp_small") == std::pair<f32, f32>{1.0f, 2.0f});
    CHECK(footprint("fp_own") == std::pair<f32, f32>{5.0f, 5.0f});
    CHECK(footprint("fp_half") == std::pair<f32, f32>{2.0f, 4.0f});
    CHECK(footprint("fp_none") == std::pair<f32, f32>{0.0f, 0.0f});
}

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

TEST_CASE("a shape's world box: a sphere's radius, a box as it is turned", "[collision]") {
    Unit ball;
    ball.set_position({10, 2, 20});
    CollisionShape s = sphere(1.5f);
    s.cy = 1; // its centre above the entity's position
    ball.set_default_collision_shape(s);
    const auto b = osc::sim::collision_bounds(ball);
    REQUIRE(b);
    CHECK(b->first.x == Approx(8.5f));
    CHECK(b->first.y == Approx(1.5f));
    CHECK(b->second.y == Approx(4.5f));
    CHECK(b->second.z == Approx(21.5f));

    // Long along its own z (half 3), turned 90 degrees about y: long along x.
    Unit brick;
    brick.set_position({0, 0, 0});
    brick.set_orientation(osc::sim::euler_to_quat(3.14159265f * 0.5f, 0, 0));
    brick.set_default_collision_shape(box(0.25f, 1, 3, 1));
    const auto t = osc::sim::collision_bounds(brick);
    REQUIRE(t);
    CHECK(t->first.x == Approx(-3).margin(1e-4));
    CHECK(t->second.x == Approx(3).margin(1e-4));
    CHECK(t->second.z == Approx(0.25f).margin(1e-4));
    CHECK(t->first.y == Approx(0).margin(1e-4));
    CHECK(t->second.y == Approx(2).margin(1e-4));

    // Turned 45 degrees, a square's box grows to its diagonal.
    Unit crate;
    crate.set_orientation(osc::sim::euler_to_quat(3.14159265f * 0.25f, 0, 0));
    crate.set_default_collision_shape(box(1, 1, 1));
    const auto d = osc::sim::collision_bounds(crate);
    REQUIRE(d);
    CHECK(d->second.x == Approx(std::sqrt(2.0f)).margin(1e-4));
    CHECK(d->second.y == Approx(1).margin(1e-4));

    Unit bare;
    CHECK_FALSE(osc::sim::collision_bounds(bare));
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
    fly(reg, *shot);
    // It met the nearer box's face at z = 103.
    CHECK_FALSE(shot->destroyed());
    CHECK(shot->position().z == Approx(103.0f));
    fly(reg, *shot);
    CHECK(shot->destroyed());
    CHECK(shot->position().z == Approx(103.0f));
}

TEST_CASE("a shot with entity collision off flies through", "[collision]") {
    EntityRegistry reg;
    reg.init_spatial_grid(512, 512);
    add_unit(reg, {100, 0, 102}, box(1, 1, 1, 1));
    Projectile* shot = add_shot(reg, {100, 1, 100}, {0, 0, 40});
    shot->collide_entity = false;
    fly(reg, *shot);
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
    fly(reg, *out);
    CHECK_FALSE(out->destroyed());

    Projectile* in = add_shot(reg, {100, 1, 115}, {0, 0, -80});
    fly(reg, *in);
    fly(reg, *in);
    CHECK(in->destroyed());
    CHECK(in->position().z == Approx(100.0f + std::sqrt(100.0f - 1.0f)).margin(1e-3));
}

TEST_CASE("a shot leaves along its muzzle unless its weapon fires along its solution",
          "[collision]") {
    // Moho's CreateProjectile: the launch is the muzzle bone's facing as the
    // aim controller posed it; UseFiringSolutionInsteadOfAimBone uses the aim.
    EntityRegistry reg;
    const u32 owner_id = add_unit(reg, {0, 0, 0}, box(1, 1, 1));
    auto& owner = static_cast<Unit&>(*reg.find(owner_id));
    osc::sim::Weapon w;
    w.muzzle_velocity = 30;
    w.max_range = 24;
    // No target: its solution is along its facing (+z); its muzzle faces +x.
    const Vector3 muzzle{1, 0, 0};
    const Projectile* along = w.launch(owner, {0, 0, 0}, nullptr, reg, nullptr, false, muzzle);
    REQUIRE(along);
    CHECK(along->velocity.x == Approx(30.0f));
    CHECK(along->velocity.z == Approx(0.0f).margin(1e-4));
    w.use_firing_solution = true;
    const Projectile* solved = w.launch(owner, {0, 0, 0}, nullptr, reg, nullptr, false, muzzle);
    REQUIRE(solved);
    CHECK(solved->velocity.x == Approx(0.0f).margin(1e-4));
    CHECK(solved->velocity.z == Approx(30.0f));
    // StraightDownOrdinance: down, whatever the muzzle.
    w.use_firing_solution = false;
    w.projectile_physics = osc::sim::Weapon::ProjectilePhysics{};
    w.projectile_physics->straight_down = true;
    const Projectile* down = w.launch(owner, {0, 0, 0}, nullptr, reg, nullptr, false, muzzle);
    REQUIRE(down);
    CHECK(down->velocity.y == Approx(-30.0f));
    CHECK(down->velocity.x == Approx(0.0f).margin(1e-4));
}

TEST_CASE("firing randomness turns a shot by a normal heading and pitch, in degrees",
          "[collision]") {
    // Moho's CreateProjectile: heading and pitch each N(0, FiringRandomness)
    // degrees; FAF's circle of r x distance / 12 is what it looks like.
    EntityRegistry reg;
    const u32 owner_id = add_unit(reg, {0, 0, 0}, box(1, 1, 1));
    auto& owner = static_cast<Unit&>(*reg.find(owner_id));
    osc::sim::Weapon w;
    w.muzzle_velocity = 30;
    w.max_range = 24;
    w.firing_randomness = 2.0f;
    constexpr int kShots = 4000;
    double sum_h = 0;
    double sum_h2 = 0;
    double sum_p2 = 0;
    for (int i = 0; i < kShots; ++i) {
        const Projectile* p = w.launch(owner, {0, 0, 0}, nullptr, reg, nullptr, false);
        REQUIRE(p);
        const Vector3& v = p->velocity;
        CHECK(std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z) == Approx(30.0f).epsilon(1e-4));
        const double heading = std::atan2(v.x, v.z) * 57.29578;
        const double pitch = std::asin(v.y / 30.0) * 57.29578;
        sum_h += heading;
        sum_h2 += heading * heading;
        sum_p2 += pitch * pitch;
    }
    const double mean = sum_h / kShots;
    CHECK(std::abs(mean) < 0.15);
    CHECK(std::sqrt(sum_h2 / kShots - mean * mean) == Approx(2.0).epsilon(0.08));
    CHECK(std::sqrt(sum_p2 / kShots) == Approx(2.0).epsilon(0.08));
}

TEST_CASE("a shot's speed is MuzzleVelocity, randomised and slowed close in", "[collision]") {
    // Moho's GetMuzzleVelocity: plus N(0, MuzzleVelocityRandom), then times
    // sqrt(distance / MuzzleVelocityReduceDistance) within that distance.
    EntityRegistry reg;
    const u32 owner_id = add_unit(reg, {0, 0, 0}, box(1, 1, 1));
    auto& owner = static_cast<Unit&>(*reg.find(owner_id));
    osc::sim::Weapon w;
    w.muzzle_velocity = 30;
    w.max_range = 24;
    w.muzzle_velocity_random = 3;
    double sum = 0;
    double sum2 = 0;
    constexpr int kShots = 4000;
    for (int i = 0; i < kShots; ++i) {
        const Projectile* p = w.launch(owner, {0, 0, 0}, nullptr, reg, nullptr, false);
        REQUIRE(p);
        const Vector3& v = p->velocity;
        const double speed = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
        sum += speed;
        sum2 += speed * speed;
    }
    const double mean = sum / kShots;
    CHECK(mean == Approx(30.0).epsilon(0.01));
    CHECK(std::sqrt(sum2 / kShots - mean * mean) == Approx(3.0).epsilon(0.08));

    // 10 away within a ReduceDistance of 40: half the speed, and the
    // barrel's arc is the slowed shell's.
    w.muzzle_velocity_random = 0;
    w.max_range = 10;
    w.muzzle_velocity_reduce_distance = 40;
    const Projectile* close = w.launch(owner, {0, 0, 0}, nullptr, reg, nullptr, false);
    REQUIRE(close);
    CHECK(close->velocity.z == Approx(15.0f));
    CHECK(w.muzzle_speed_at(10.0f, nullptr) == Approx(15.0f));
    w.ballistic_arc = osc::sim::Weapon::Arc::Low;
    const f32 elevation = w.launch_elevation(10.0f, 0.0f);
    // Level ground: sin(2 theta) = g d / v^2 at v = 15.
    CHECK(std::sin(2.0f * elevation) == Approx(Projectile::GRAVITY * 10.0f / 225.0f).epsilon(1e-3));
}

TEST_CASE("a shot falls at Moho's gravity unless its blueprint says UseGravity = false",
          "[collision]") {
    osc::lua::LuaState lua;
    REQUIRE(lua.do_string("__blueprints = {['/shell'] = {Physics = {}},"
                          " ['/missile'] = {Physics = {TrackTarget = true}},"
                          " ['/bolt'] = {Physics = {UseGravity = false}}}")
                .ok());
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
    for (const char* bp : {"/shell", "/missile"}) {
        gun.projectile_bp_id = bp;
        const Projectile* shot = gun.launch(owner, {0, 20, 0}, nullptr, reg, lua.raw(), false);
        REQUIRE(shot);
        CHECK(shot->ballistic_accel == Approx(-Projectile::GRAVITY));
    }
    gun.projectile_bp_id = "/bolt";
    const Projectile* bolt = gun.launch(owner, {0, 20, 0}, nullptr, reg, lua.raw(), false);
    REQUIRE(bolt);
    CHECK(bolt->ballistic_accel == 0.0f);
}

TEST_CASE("a straight weapon lobs its falling shots onto their target", "[collision]") {
    osc::lua::LuaState lua;
    REQUIRE(lua.do_string("__blueprints = {['/shell'] = {Physics = {}}}").ok());
    EntityRegistry reg;
    reg.init_spatial_grid(512, 512);
    const u32 owner_id = add_unit(reg, {100, 0, 100}, box(1, 1, 1, 1));
    auto& owner = static_cast<Unit&>(*reg.find(owner_id));
    const u32 target_id = add_unit(reg, {100, 0, 130}, box(1, 1, 1, 1));
    const auto& target = static_cast<const Unit&>(*reg.find(target_id));
    osc::sim::Weapon gun;
    gun.muzzle_velocity = 20;
    gun.max_range = 40;
    gun.projectile_bp_id = "/shell";
    gun.projectile_physics = osc::sim::Weapon::ProjectilePhysics{};
    const f32 level = target.target_point(-1).y;
    Projectile* shot = gun.launch(owner, {100, level, 100}, &target, reg, lua.raw(), false);
    REQUIRE(shot);
    const Vector3 v = shot->velocity;
    const f32 pitch = std::asin(v.y / std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z));
    CHECK(std::sin(2.0f * pitch) == Approx(Projectile::GRAVITY * 30.0f / 400.0f).epsilon(1e-3));
    for (int tick = 0; tick < 20 && !shot->destroyed(); ++tick) {
        fly(reg, *shot);
    }
    CHECK(shot->destroyed());
    CHECK(shot->position().z == Approx(129.0f).margin(1e-3));
}

namespace {

struct Turret {
    osc::lua::LuaState lua;
    osc::sim::BoneData bones;
    EntityRegistry reg;
    Unit* owner = nullptr;
    osc::sim::AimManipulator* aim = nullptr;
    osc::sim::Weapon gun;

    explicit Turret(f32 muzzle_velocity) {
        REQUIRE(lua.do_string("__blueprints = {['/shell'] = {Physics = {}}}").ok());
        osc::sim::BoneInfo turret;
        turret.name = "turret";
        bones.bones.push_back(turret);
        osc::sim::BoneInfo barrel;
        barrel.name = "barrel";
        barrel.parent_index = 0;
        barrel.local_position = {0, 0.5f, 0.5f};
        barrel.world_position = {0, 0.5f, 0.5f};
        barrel.inverse_bind_pose = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, -0.5f, -0.5f, 1};
        bones.bones.push_back(barrel);
        bones.name_to_index["turret"] = 0;
        bones.name_to_index["barrel"] = 1;
        owner = static_cast<Unit*>(reg.find(add_unit(reg, {0, 0, 0}, box(1, 1, 1, 1))));
        owner->set_bone_data(&bones);
        owner->init_animated_bones();
        aim = static_cast<osc::sim::AimManipulator*>(
            owner->add_manipulator(std::make_unique<osc::sim::AimManipulator>()));
        aim->set_weapon(0, "Gun");
        aim->set_yaw_bone(0);
        aim->set_pitch_bone(1);
        aim->set_firing_arc(-180.0f, 180.0f, 180.0f, -90.0f, 90.0f, 180.0f);
        gun.label = "Gun";
        gun.muzzle_bone_name = "barrel";
        gun.max_range = 40;
        gun.damage = 10;
        gun.firing_tolerance = 1;
        gun.muzzle_velocity = muzzle_velocity;
        gun.projectile_bp_id = "/shell";
        gun.projectile_physics = osc::sim::Weapon::ProjectilePhysics{};
    }

    Projectile* fire_at(const Vector3& at, int ticks) {
        gun.set_target_ground(at);
        for (int tick = 0; tick < ticks; ++tick) {
            gun.update(*owner, reg, lua.raw());
            if (gun.fire_clock > 0) {
                break;
            }
            owner->tick_manipulators(0.1f, nullptr);
        }
        Projectile* shot = nullptr;
        reg.for_each([&](osc::sim::Entity& e) {
            if (e.is_projectile()) {
                shot = static_cast<Projectile*>(&e);
            }
        });
        return shot;
    }
};

} // namespace

TEST_CASE("a straight weapon's turret pitches up to lob its falling shots", "[collision]") {
    Turret t(20);
    Projectile* shot = t.fire_at({0, 0, 30}, 30);
    REQUIRE(shot);
    const f32 v2 = 400.0f;
    const f32 g = Projectile::GRAVITY;
    const f32 d = 29.5f;
    const f32 h = -0.5f;
    const f32 low = std::atan((v2 - std::sqrt(v2 * v2 - g * (g * d * d + 2 * h * v2))) / (g * d));
    CHECK(t.aim->pitch() == Approx(low).margin(1e-4));
    Vector3 was = shot->position();
    for (int tick = 0; tick < 30 && shot->position().y > 0.0f; ++tick) {
        was = shot->position();
        fly(t.reg, *shot);
    }
    const Vector3 now = shot->position();
    REQUIRE(now.y <= 0.0f);
    const f32 landed = was.z + (now.z - was.z) * was.y / (was.y - now.y);
    CHECK(landed == Approx(30.0f).margin(0.05f));
}

TEST_CASE("a straight weapon's turret holds, off target, where no arc reaches", "[collision]") {
    Turret t(15);
    CHECK(t.fire_at({0, 25, 30}, 30) == nullptr);
    CHECK_FALSE(t.aim->on_target());
    CHECK(t.aim->pitch() == 0.0f);
}

TEST_CASE("a shot whose projectile gives no Lifetime lives Moho's 15 seconds", "[collision]") {
    osc::lua::LuaState lua;
    REQUIRE(lua.do_string("__blueprints = {bare = {Physics = {}},"
                          " spread = {Physics = {LifetimeRange = 2}}}")
                .ok());
    EntityRegistry reg;
    const u32 owner_id = add_unit(reg, {0, 20, 0}, box(1, 1, 1));
    auto& owner = static_cast<Unit&>(*reg.find(owner_id));
    const auto lifetime = [&](osc::sim::Weapon& w) {
        const Projectile* p = w.launch(owner, {0, 20, 0}, nullptr, reg, lua.raw(), false);
        REQUIRE(p);
        return p->lifetime_end;
    };
    osc::sim::Weapon gun;
    gun.muzzle_velocity = 20;
    gun.max_range = 40;
    gun.projectile_bp_id = "bare";
    CHECK(lifetime(gun) == 150);
    osc::sim::Weapon bomb;
    bomb.need_compute_bomb_drop = true;
    bomb.projectile_bp_id = "bare";
    CHECK(lifetime(bomb) == 150);
    osc::sim::Weapon silo;
    silo.counted_projectile = true;
    silo.projectile_bp_id = "bare";
    CHECK(lifetime(silo) == 150);

    gun.projectile_bp_id = "spread";
    u32 low = 1000;
    u32 high = 0;
    for (int i = 0; i < 200; ++i) {
        const u32 t = lifetime(gun);
        low = std::min(low, t);
        high = std::max(high, t);
    }
    CHECK(low >= 130);
    CHECK(low < 135);
    CHECK(high > 165);
    CHECK(high <= 170);
}

TEST_CASE("a projectile blueprint's LifeTime is not its Lifetime", "[collision]") {
    osc::lua::LuaState lua;
    REQUIRE(lua.do_string("__blueprints = {typo = {Physics = {LifeTime = 30}}}").ok());
    EntityRegistry reg;
    const u32 owner_id = add_unit(reg, {0, 0, 0}, box(1, 1, 1));
    auto& owner = static_cast<Unit&>(*reg.find(owner_id));
    osc::sim::Weapon w;
    w.muzzle_velocity = 20;
    w.max_range = 40;
    w.projectile_bp_id = "typo";
    const Projectile* p = w.launch(owner, {0, 0, 0}, nullptr, reg, lua.raw(), false);
    REQUIRE(p);
    CHECK(p->lifetime_end == 150);
}

TEST_CASE("a weapon's lifetime for its shots overrides the default", "[collision]") {
    EntityRegistry reg;
    const u32 owner_id = add_unit(reg, {0, 0, 0}, box(1, 1, 1));
    auto& owner = static_cast<Unit&>(*reg.find(owner_id));
    osc::sim::Weapon w;
    w.muzzle_velocity = 20;
    w.max_range = 40;
    const Projectile* plain = w.launch(owner, {0, 0, 0}, nullptr, reg, nullptr, false);
    REQUIRE(plain);
    CHECK(plain->lifetime_end == 150);
    // ProjectileLifetime sets it.
    w.projectile_lifetime = 7;
    const Projectile* timed = w.launch(owner, {0, 0, 0}, nullptr, reg, nullptr, false);
    REQUIRE(timed);
    CHECK(timed->lifetime_end == 70);
    // The multiplier wins: 1.15 x 40 / 20.
    w.projectile_lifetime_multiplier = 1.15f;
    const Projectile* scaled = w.launch(owner, {0, 0, 0}, nullptr, reg, nullptr, false);
    REQUIRE(scaled);
    CHECK(scaled->lifetime_end == 23);
}

TEST_CASE("a leading weapon aims where a missile will be", "[collision]") {
    EntityRegistry reg;
    const u32 owner_id = add_unit(reg, {0, 0, 0}, box(1, 1, 1));
    const auto& owner = static_cast<const Unit&>(*reg.find(owner_id));
    Projectile* missile = add_shot(reg, {40, 0, 0}, {0, 0, 10});
    osc::sim::Weapon w;
    w.muzzle_velocity = 100;
    const Vector3 ahead{0, 0, 1};
    w.lead_target = false;
    CHECK(w.aim_point(*missile, owner, owner.position(), ahead).z == Approx(1.0f));
    w.lead_target = true;
    CHECK(w.aim_point(*missile, owner, owner.position(), ahead).z == Approx(4.6376f));
}

TEST_CASE("a leading weapon aims at its target when no shot can meet it", "[collision]") {
    EntityRegistry reg;
    const u32 owner_id = add_unit(reg, {0, 0, 0}, box(1, 1, 1));
    const auto& owner = static_cast<const Unit&>(*reg.find(owner_id));
    Projectile* missile = add_shot(reg, {40, 0, 0}, {-10, 0, 0});
    osc::sim::Weapon w;
    w.muzzle_velocity = 5;
    w.lead_target = true;
    const Vector3 at = w.aim_point(*missile, owner, owner.position(), {1, 0, 0});
    CHECK(at.x == Approx(39.0f));
    CHECK(at.z == Approx(0.0f));
}

TEST_CASE("a weapon with no muzzle velocity, as a beam's, leads by nothing", "[collision]") {
    EntityRegistry reg;
    const u32 owner_id = add_unit(reg, {0, 0, 0}, box(1, 1, 1));
    const auto& owner = static_cast<const Unit&>(*reg.find(owner_id));
    Projectile* missile = add_shot(reg, {40, 0, 0}, {0, 0, 10});
    const osc::sim::Weapon w;
    CHECK(w.aim_point(*missile, owner, owner.position(), {0, 0, 1}).z == Approx(1.0f));
}

TEST_CASE("a weapon firing tracking shots leads at their top speed", "[collision]") {
    EntityRegistry reg;
    const u32 owner_id = add_unit(reg, {0, 0, 0}, box(1, 1, 1));
    const auto& owner = static_cast<const Unit&>(*reg.find(owner_id));
    Projectile* missile = add_shot(reg, {40, 0, 0}, {0, 0, 10});
    osc::sim::Weapon w;
    w.muzzle_velocity = 5;
    w.lead_target = true;
    w.projectile_physics = osc::sim::Weapon::ProjectilePhysics{true, true, 100};
    CHECK(w.aim_point(*missile, owner, owner.position(), {0, 0, 1}).z == Approx(4.6376f));
}

TEST_CASE("a weapon firing falling shots leads by its muzzle's level speed", "[collision]") {
    EntityRegistry reg;
    const u32 owner_id = add_unit(reg, {0, 0, 0}, box(1, 1, 1));
    const auto& owner = static_cast<const Unit&>(*reg.find(owner_id));
    const u32 target_id = add_unit(reg, {50, 0, 0}, box(1, 1, 1));
    auto& target = static_cast<Unit&>(*reg.find(target_id));
    target.set_velocity({0, 0, 2});
    osc::sim::Weapon w;
    w.muzzle_velocity = 20;
    w.lead_target = true;
    w.projectile_physics = osc::sim::Weapon::ProjectilePhysics{};
    const Vector3 raised{0.5f, std::sqrt(3.0f) * 0.5f, 0};
    CHECK(w.aim_point(target, owner, owner.position(), raised).z == Approx(10.4143f));
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

TEST_CASE("a projectile's blueprint ranges spread each one made", "[collision]") {
    // Moho's Projectile: TurnRate, MaxSpeed, Acceleration and Lifetime each
    // base +- their *Range, uniformly.
    osc::lua::LuaState lua;
    REQUIRE(lua.do_string("__blueprints = {['/p'] = {Physics = {MaxSpeed = 10, MaxSpeedRange = 2,"
                          " TurnRate = 90, Acceleration = 4, Lifetime = 5, LifetimeRange = 1}}}")
                .ok());
    osc::sim::SimRandom rng(9);
    f32 low = 1e9f;
    f32 high = -1e9f;
    for (int i = 0; i < 400; ++i) {
        Projectile p;
        p.set_blueprint_id("/p");
        const auto found = p.apply_blueprint_physics(lua.raw(), &rng);
        CHECK(p.max_speed >= 8.0f);
        CHECK(p.max_speed <= 12.0f);
        CHECK(p.turn_rate == 90.0f); // no range: as it is
        CHECK(p.acceleration == 4.0f);
        CHECK(found.lifetime >= 4.0f);
        CHECK(found.lifetime <= 6.0f);
        low = std::min(low, p.max_speed);
        high = std::max(high, p.max_speed);
    }
    CHECK(low < 8.2f);
    CHECK(high > 11.8f);
    // Without the stream (a query, not a projectile made), the blueprint's own.
    Projectile plain;
    plain.set_blueprint_id("/p");
    plain.apply_blueprint_physics(lua.raw());
    CHECK(plain.max_speed == 10.0f);
}

TEST_CASE("a projectile spins at its blueprint's RotationalVelocity about a random axis",
          "[collision]") {
    osc::lua::LuaState lua;
    REQUIRE(lua.do_string("__blueprints = {['/bomblet'] = {Physics = {RotationalVelocity = 200,"
                          " RotationalVelocityRange = 100}},"
                          " ['/torpedo'] = {Physics = {RotationalVelocity = 800}},"
                          " ['/shell'] = {Physics = {MaxSpeed = 30}}}")
                .ok());
    constexpr f32 kDeg = 0.017453292f;
    const auto rate = [](const Projectile& p) {
        const Vector3& w = p.angular_velocity;
        return std::sqrt(w.x * w.x + w.y * w.y + w.z * w.z);
    };
    osc::sim::SimRandom rng(9);
    f32 low = 1e9f;
    f32 high = -1e9f;
    f32 lowest_x = 1e9f;
    f32 highest_x = -1e9f;
    for (int i = 0; i < 400; ++i) {
        Projectile p;
        p.set_blueprint_id("/bomblet");
        (void)p.apply_blueprint_physics(lua.raw(), &rng);
        CHECK(rate(p) >= Approx(100 * kDeg));
        CHECK(rate(p) <= Approx(300 * kDeg));
        low = std::min(low, rate(p));
        high = std::max(high, rate(p));
        lowest_x = std::min(lowest_x, p.angular_velocity.x / rate(p));
        highest_x = std::max(highest_x, p.angular_velocity.x / rate(p));
    }
    CHECK(low < 110 * kDeg);
    CHECK(high > 290 * kDeg);
    CHECK(lowest_x < -0.9f);
    CHECK(highest_x > 0.9f);

    Projectile torpedo;
    torpedo.set_blueprint_id("/torpedo");
    (void)torpedo.apply_blueprint_physics(lua.raw(), &rng);
    CHECK(rate(torpedo) == Approx(800 * kDeg));

    const auto before = rng.state();
    Projectile shell;
    shell.set_blueprint_id("/shell");
    (void)shell.apply_blueprint_physics(lua.raw(), &rng);
    CHECK(rate(shell) == 0.0f);
    CHECK(rng.state() == before);
}
