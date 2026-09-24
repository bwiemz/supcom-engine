// Collision (M201d): the shapes shots meet, the query that finds them, and a
// shot's sweep through a tick.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "sim/collision.hpp"
#include "sim/entity_registry.hpp"
#include "sim/manipulator.hpp"
#include "sim/unit.hpp"

#include <cmath>
#include <memory>
#include <vector>

using Catch::Approx;
using osc::f32;
using osc::u32;
using osc::sim::CollisionShape;
using osc::sim::CollisionShapeType;
using osc::sim::EntityRegistry;
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

