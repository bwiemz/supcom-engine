// Moho's path spline (faf-re CAiPathSpline::Generate), against retail's drive
// grid probe: a unit's position each tick as it follows the spline.

#include <catch2/catch_test_macros.hpp>

#include "core/dmath.hpp"
#include "sim/path_spline.hpp"

#include <cmath>
#include <string>
#include <vector>

using osc::f32;
using osc::sim::Vector3;
using osc::sim::path::Node;
using osc::sim::path::PathType;
using osc::sim::path::Pose;
using osc::sim::path::Spline;
using osc::sim::path::SplineUnit;
using osc::sim::path::SplineWorld;

namespace {

constexpr f32 kPi = 3.14159265358979f;

/// A unit's heading as the probe logs it (degrees, 0 toward +z).
Vector3 facing(f32 degrees) {
    const f32 r = degrees * kPi / 180.0f;
    return {osc::dmath::sin(r), 0, osc::dmath::cos(r)};
}

/// The nodes a unit stands on, tick by tick, following its spline as Moho's
/// steering does: generated afresh, then again from its continuation at its
/// last node, `ticks` of them.
std::vector<Node> follow(const SplineUnit& u, const Pose& pose, const Vector3& to, int ticks) {
    Spline s;
    const SplineWorld world;
    s.generate(u, pose, to, PathType::Normal, false, false, world);
    std::vector<Node> out;
    for (int t = 0; t < ticks; ++t) {
        if (!s.empty() && s.at_end()) s.generate(u, pose, to, PathType::Normal, true, false, world);
        const Node* n = s.advance();
        if (!n) break;
        out.push_back(*n);
    }
    return out;
}

std::string near(const std::vector<Node>& got, const std::vector<f32>& want_z, f32 x, f32 tol) {
    if (got.size() < want_z.size()) return "only " + std::to_string(got.size()) + " nodes";
    for (size_t i = 0; i < want_z.size(); ++i) {
        if (std::fabs(got[i].position.z - want_z[i]) > tol ||
            std::fabs(got[i].position.x - x) > tol * 20) {
            return "node " + std::to_string(i) + ": " + std::to_string(got[i].position.x) + ", " +
                   std::to_string(got[i].position.z) + " not z " + std::to_string(want_z[i]);
        }
    }
    return "";
}

} // namespace

TEST_CASE("A Striker's spline to a goal 1.85 ahead is retail's, tick by tick", "[spline]") {
    // uel0201: pivots, so drives forward; accelerates and brakes at 3.4.
    SplineUnit u;
    u.max_speed = 3.4f;
    u.max_accel = 3.4f;
    u.max_brake = 3.4f;
    u.turn_rate = 90;
    u.rotate_on_spot = true;
    u.rotate_threshold = 0.1f;
    u.backup_distance = 2.7f;
    u.size_x = 0.7f;
    u.size_z = 0.9f;
    u.top_speed = 3.4f;
    Pose pose;
    pose.position = {657.5f, 0, 106.354f};
    pose.forward = facing(180);
    // Retail (drivegrid, uel0201 leg 3) from the tick it set off to the tick its
    // move ended in the goal's cell: 2.38, 2.48, 2.13 at the top.
    const std::vector<f32> retail{106.320f, 106.252f, 106.150f, 106.014f, 105.844f,
                                  105.640f, 105.402f, 105.154f, 104.941f};
    const auto got = follow(u, pose, {657.5f, 0, 104.5f}, 12);
    CHECK(near(got, retail, 657.5f, 0.002f).empty());
    INFO(near(got, retail, 657.5f, 0.002f));
}

TEST_CASE("A scout backs up tail first to a goal just behind it, as retail's", "[spline]") {
    // uel0101: reverses (MaxSpeedReverse 4.5) and doesn't pivot; its goal 1.7
    // behind is inside its BackUpDistance of 2.
    SplineUnit u;
    u.max_speed = 4.5f;
    u.max_speed_reverse = 4.5f;
    u.max_accel = 4.5f;
    u.max_brake = 4.5f;
    u.turn_rate = 90;
    u.turn_radius = 5;
    u.rotate_threshold = 0.5f;
    u.backup_distance = 2;
    u.size_x = 0.35f;
    u.size_z = 0.65f;
    u.top_speed = 4.5f;
    Pose pose;
    pose.position = {615.537f, 0, 106.201f};
    pose.forward = facing(-7.13f);
    // Retail (drivegrid, uel0101 leg 3): its tail swung onto the goal in a tick
    // (heading 1.26), then straight back.
    const std::vector<f32> retail{106.156f, 106.066f, 105.931f, 105.751f,
                                  105.526f, 105.256f, 105.031f, 104.851f};
    const auto got = follow(u, pose, {615.5f, 0, 104.5f}, 12);
    CHECK(near(got, retail, 615.52f, 0.002f).empty());
    INFO(near(got, retail, 615.52f, 0.002f));
    REQUIRE(!got.empty());
    // Facing as retail's after its first tick: 1.26 degrees.
    const f32 heading = osc::dmath::atan2(got[0].direction.x, got[0].direction.z) * 180.0f / kPi;
    CHECK(std::fabs(heading - 1.26f) < 0.05f);
}

TEST_CASE("A spline holds 20 nodes and carries on from its continuation", "[spline]") {
    SplineUnit u;
    u.max_speed = 3.4f;
    u.max_accel = 3.4f;
    u.max_brake = 3.4f;
    u.turn_rate = 90;
    u.top_speed = 3.4f;
    Pose pose;
    Spline s;
    const SplineWorld world;
    s.generate(u, pose, {0, 0, 40}, PathType::Normal, false, false, world);
    REQUIRE(s.nodes().size() == 20);
    const Vector3 last = s.nodes().back().position;
    const Vector3 before = s.nodes()[18].position;
    s.generate(u, pose, {0, 0, 40}, PathType::Normal, true, false, world);
    REQUIRE(!s.nodes().empty());
    // The new spline starts again from before the old one's last node, so its
    // first node is that one.
    CHECK(std::fabs(s.nodes()[0].position.z - last.z) < 1e-5f);
    CHECK(s.nodes()[0].position.z > before.z);
}

TEST_CASE("A mode 4 spline brakes at twice its brake and ends past its sixth node", "[spline]") {
    // CAiPathSpline::Update: a Striker going 3.4 forward, told to hold for a unit crossing.
    SplineUnit u;
    u.max_speed = 3.4f;
    u.max_accel = 3.4f;
    u.max_brake = 3.4f;
    u.turn_rate = 90;
    u.top_speed = 3.4f;
    Pose pose;
    pose.velocity = {0, 0, 0.34f};
    Spline s;
    s.brake(u, pose, 4, SplineWorld{});
    const auto& n = s.nodes();
    // Steps of 0.34 less 0.068 a tick: 0.272, 0.204, 0.136, 0.068, then still; it ends on
    // the still node pushed once it has more than five (the count is taken before the push).
    REQUIRE(n.size() == 7);
    const std::vector<f32> want{0.272f, 0.476f, 0.612f, 0.680f, 0.680f, 0.680f, 0.680f};
    for (size_t i = 0; i < want.size(); ++i) CHECK(std::fabs(n[i].position.z - want[i]) < 1e-5f);
}
