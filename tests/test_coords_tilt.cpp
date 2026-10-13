// Moho's COORDS_Tilt (tilt_to), which sets a unit on the slope of the
// ground under it.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "sim/manipulator.hpp"
#include "sim/unit.hpp"

#include <cmath>

using Catch::Approx;
using osc::f32;
using osc::sim::quat_rotate;
using osc::sim::Quaternion;
using osc::sim::tilt_to;
using osc::sim::Vector3;

namespace {

f32 length(const Quaternion& q) {
    return std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
}

} // namespace

TEST_CASE("a tilt puts the up along the normal and keeps the heading", "[tilt]") {
    // Facing east (a quarter turn about y), set on ground rising to the east.
    const f32 h = std::sqrt(0.5f);
    const Quaternion q = tilt_to(Quaternion{0, h, 0, h}, Vector3{-0.3f, 1.0f, 0.0f});
    const f32 n = std::sqrt(0.09f + 1.0f);
    const Vector3 up = quat_rotate(q, {0, 1, 0});
    CHECK(up.x == Approx(-0.3f / n).margin(1e-5));
    CHECK(up.y == Approx(1.0f / n).margin(1e-5));
    CHECK(up.z == Approx(0.0f).margin(1e-5));
    CHECK(length(q) == Approx(1.0f).margin(1e-6));
    // Its nose still points east, raised up the slope.
    const Vector3 nose = quat_rotate(q, {0, 0, 1});
    CHECK(nose.x > 0.95f);
    CHECK(nose.y > 0.0f);
    CHECK(nose.z == Approx(0.0f).margin(1e-5));
}

TEST_CASE("a zero normal leaves the orientation as it was", "[tilt]") {
    const Quaternion q{0.1f, 0.2f, 0.3f, 0.9f};
    const Quaternion t = tilt_to(q, Vector3{0, 0, 0});
    CHECK(t.x == q.x);
    CHECK(t.y == q.y);
    CHECK(t.z == q.z);
    CHECK(t.w == q.w);
}

TEST_CASE("upside down, a tilt makes a half turn, not a zero quaternion", "[tilt]") {
    // Moho's BuildTiltShortestArcDelta: no half-way vector between up and a
    // normal straight down, so a turn about an axis square to up.
    const Quaternion q = tilt_to(Quaternion{0, 0, 0, 1}, Vector3{0, -1, 0});
    CHECK(length(q) == Approx(1.0f).margin(1e-6));
    const Vector3 up = quat_rotate(q, {0, 1, 0});
    CHECK(up.y == Approx(-1.0f).margin(1e-5));
}

TEST_CASE("a tilt carries a quaternion's error along, not cubed", "[tilt]") {
    // Slightly long, as float rounding leaves one. Its up, read off the
    // rotation matrix as Moho does, stays unit length, so the turn does too.
    const f32 h = std::sqrt(0.5f) * 1.001f;
    const Quaternion q = tilt_to(Quaternion{0, h, 0, h}, Vector3{0.05f, 1.0f, 0.0f});
    CHECK(length(q) == Approx(1.001f).margin(1e-5));
}
