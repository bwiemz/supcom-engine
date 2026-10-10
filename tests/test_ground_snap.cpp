#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "map/heightmap.hpp"
#include "sim/bone_data.hpp"
#include "map/terrain.hpp"
#include "sim/manipulator.hpp"
#include "sim/unit.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using Catch::Approx;
using osc::f32;
using osc::sim::Unit;
using osc::sim::Vector3;

namespace {

constexpr f32 kBase = 1000.0f / 128.0f;

osc::map::Terrain ridge(f32 water, bool has_water) {
    std::vector<osc::u16> heights(129 * 129);
    for (int z = 0; z <= 128; ++z) {
        for (int x = 0; x <= 128; ++x) {
            const int up = std::max(0, 10 - std::abs(x - 64));
            heights[(static_cast<size_t>(z) * 129) + static_cast<size_t>(x)] =
                static_cast<osc::u16>(1000 + 128 * up);
        }
    }
    return {osc::map::Heightmap(128, 128, 1.0f / 128.0f, std::move(heights)), water, has_water};
}

std::unique_ptr<Unit> unit(const std::string& motion, f32 heading) {
    auto u = std::make_unique<Unit>();
    u->set_motion_type(motion);
    u->set_position({64.0f, kBase + 10.0f, 64.0f});
    u->set_orientation(osc::sim::euler_to_quat(heading, 0.0f, 0.0f));
    u->set_size_xz(2.0f, 1.0f);
    return u;
}

void check_vec(const Vector3& v, f32 x, f32 y, f32 z) {
    CHECK(v.x == Approx(x).margin(1e-5));
    CHECK(v.y == Approx(y).margin(1e-5));
    CHECK(v.z == Approx(z).margin(1e-5));
}

Vector3 up(const Unit& u) {
    return osc::sim::quat_rotate(u.orientation(), {0.0f, 1.0f, 0.0f});
}

Vector3 ahead(const Unit& u) {
    return osc::sim::quat_rotate(u.orientation(), {0.0f, 0.0f, 1.0f});
}

constexpr f32 kHalfRoot2 = 0.70710678f;

} // namespace

TEST_CASE("A land unit on a ridge stands at the mean of its box's corners", "[ground_snap]") {
    const auto terrain = ridge(0.0f, false);
    const auto across = unit("RULEUMT_Land", 0.0f);
    CHECK(across->ground_y(&terrain, 64.0f, 64.0f) == Approx(kBase + 9.0f));
    const auto along = unit("RULEUMT_Land", 1.5707964f);
    CHECK(along->ground_y(&terrain, 64.0f, 64.0f) == Approx(kBase + 9.5f));
    CHECK(across->ground_y(&terrain, 70.0f, 64.0f) == Approx(kBase + 4.0f));
}

TEST_CASE("A hover unit's corners stand on the water, a land unit's on the ground",
          "[ground_snap]") {
    const auto terrain = ridge(kBase + 9.5f, true);
    CHECK(unit("RULEUMT_Hover", 0.0f)->ground_y(&terrain, 64.0f, 64.0f) == Approx(kBase + 9.5f));
    CHECK(unit("RULEUMT_Land", 0.0f)->ground_y(&terrain, 64.0f, 64.0f) == Approx(kBase + 9.0f));
    const auto ship = unit("RULEUMT_Water", 0.0f);
    ship->set_layer("Water");
    CHECK(ship->ground_y(&terrain, 64.0f, 64.0f) == Approx(kBase + 10.0f));
}

TEST_CASE("StandUpright and SinkLower lower a unit by a quarter of its ground's spread",
          "[ground_snap]") {
    const auto terrain = ridge(0.0f, false);
    const auto upright = unit("RULEUMT_Land", 0.0f);
    upright->set_ground_snap_flags(true, false);
    CHECK(upright->ground_y(&terrain, 64.0f, 64.0f) == Approx(kBase + 8.75f));
    const auto sink = unit("RULEUMT_Land", 0.0f);
    sink->set_ground_snap_flags(false, true);
    CHECK(sink->ground_y(&terrain, 64.0f, 64.0f) == Approx(kBase + 8.75f));
}

TEST_CASE("A land unit on a slope tilts to the plane of its box's corners", "[ground_snap]") {
    const auto terrain = ridge(0.0f, false);
    const auto across = unit("RULEUMT_Land", 0.0f);
    across->stand_on_ground(&terrain, {70.0f, 0.0f, 64.0f}, osc::sim::euler_to_quat(0, 0, 0));
    CHECK(across->position().y == Approx(kBase + 4.0f));
    CHECK(across->orientation().x == Approx(0.0f).margin(1e-6));
    CHECK(across->orientation().y == Approx(0.0f).margin(1e-6));
    CHECK(across->orientation().z == Approx(-0.38268343f));
    CHECK(across->orientation().w == Approx(0.92387953f));
    check_vec(up(*across), kHalfRoot2, kHalfRoot2, 0.0f);
    check_vec(ahead(*across), 0.0f, 0.0f, 1.0f);

    const auto downhill = unit("RULEUMT_Land", 0.0f);
    downhill->stand_on_ground(&terrain, {70.0f, 0.0f, 64.0f},
                              osc::sim::euler_to_quat(1.5707964f, 0, 0));
    CHECK(downhill->position().y == Approx(kBase + 4.0f));
    check_vec(up(*downhill), kHalfRoot2, kHalfRoot2, 0.0f);
    check_vec(ahead(*downhill), kHalfRoot2, -kHalfRoot2, 0.0f);
    CHECK(osc::sim::quat_yaw(downhill->orientation()) == Approx(1.5707964f));

    const auto tilted = unit("RULEUMT_Land", 0.0f);
    tilted->set_orientation(across->orientation());
    tilted->stand_on_ground(&terrain, {64.0f, 0.0f, 20.0f}, tilted->orientation());
    check_vec(up(*tilted), 0.0f, 1.0f, 0.0f);
    check_vec(ahead(*tilted), 0.0f, 0.0f, 1.0f);
}

TEST_CASE("A StandUpright unit, a hover unit on the water and a ship stay level on a slope",
          "[ground_snap]") {
    const auto upright = unit("RULEUMT_Land", 0.0f);
    upright->set_ground_snap_flags(true, false);
    const auto ridge_dry = ridge(0.0f, false);
    upright->stand_on_ground(&ridge_dry, {70.0f, 0.0f, 64.0f}, upright->orientation());
    check_vec(up(*upright), 0.0f, 1.0f, 0.0f);
    upright->set_orientation({0.0f, 0.0f, -0.38268343f, 0.92387953f});
    upright->stand_on_ground(&ridge_dry, {70.0f, 0.0f, 64.0f}, upright->orientation());
    check_vec(up(*upright), 0.0f, 1.0f, 0.0f);
    check_vec(ahead(*upright), 0.0f, 0.0f, 1.0f);

    const auto flooded = ridge(kBase + 6.0f, true);
    const auto hover = unit("RULEUMT_Hover", 0.0f);
    hover->stand_on_ground(&flooded, {70.0f, 0.0f, 64.0f}, hover->orientation());
    CHECK(hover->position().y == Approx(kBase + 6.0f));
    check_vec(up(*hover), 0.0f, 1.0f, 0.0f);
    const auto ship = unit("RULEUMT_Water", 0.0f);
    ship->set_layer("Water");
    ship->stand_on_ground(&ridge_dry, {70.0f, 0.0f, 64.0f}, ship->orientation());
    check_vec(up(*ship), 0.0f, 1.0f, 0.0f);
}

TEST_CASE("A turret on a slope aims and lobs at a target on level ground", "[ground_snap][aim]") {
    osc::sim::BoneData bd;
    osc::sim::BoneInfo turret;
    turret.name = "turret";
    turret.parent_index = -1;
    bd.bones.push_back(turret);
    osc::sim::BoneInfo barrel;
    barrel.name = "barrel";
    barrel.parent_index = 0;
    barrel.local_position = {0, 0.5f, 0.5f};
    barrel.world_position = {0, 0.5f, 0.5f};
    barrel.inverse_bind_pose = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, -0.5f, -0.5f, 1};
    bd.bones.push_back(barrel);
    bd.name_to_index["turret"] = 0;
    bd.name_to_index["barrel"] = 1;

    const auto terrain = ridge(0.0f, false);
    const auto tank = unit("RULEUMT_Land", 0.0f);
    tank->set_bone_data(&bd);
    tank->init_animated_bones();
    tank->stand_on_ground(&terrain, {70.0f, 0.0f, 64.0f}, tank->orientation());
    REQUIRE(up(*tank).x == Approx(kHalfRoot2));
    auto& aim = static_cast<osc::sim::AimManipulator&>(
        *tank->add_manipulator(std::make_unique<osc::sim::AimManipulator>()));
    aim.set_yaw_bone(0);
    aim.set_pitch_bone(1);
    aim.set_firing_arc(-180.0f, 180.0f, 180.0f, -90.0f, 90.0f, 180.0f);
    const Vector3 target{100.0f, kBase, 90.0f};
    aim.set_target(target, 0.01f);
    for (int t = 0; t < 30; ++t) {
        tank->tick_manipulators(0.1f, nullptr);
    }
    CHECK(aim.on_target());
    const Vector3 from = tank->bone_world_position(1);
    const Vector3 to{target.x - from.x, target.y - from.y, target.z - from.z};
    const f32 len = std::sqrt(to.x * to.x + to.y * to.y + to.z * to.z);
    check_vec(tank->bone_world_forward(1), to.x / len, to.y / len, to.z / len);

    const f32 elevation = 0.3f;
    aim.set_elevation(elevation);
    for (int t = 0; t < 30; ++t) {
        tank->tick_manipulators(0.1f, nullptr);
    }
    CHECK(aim.on_target());
    const Vector3 at = tank->bone_world_position(1);
    const f32 dx = target.x - at.x;
    const f32 dz = target.z - at.z;
    const f32 flat = std::sqrt(dx * dx + dz * dz);
    const Vector3 lob = tank->bone_world_forward(1);
    CHECK(lob.x == Approx(dx / flat * std::cos(elevation)).margin(1e-3));
    CHECK(lob.y == Approx(std::sin(elevation)).margin(1e-3));
    CHECK(lob.z == Approx(dz / flat * std::cos(elevation)).margin(1e-3));
}
