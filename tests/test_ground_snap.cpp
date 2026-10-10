#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "map/heightmap.hpp"
#include "map/terrain.hpp"
#include "sim/manipulator.hpp"
#include "sim/unit.hpp"

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using Catch::Approx;
using osc::f32;
using osc::sim::Unit;

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
