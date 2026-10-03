#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "renderer/overlay_renderer.hpp"
#include "sim/world_snapshot.hpp"

#include <cmath>

using osc::f32;
using osc::renderer::convex_rows;

TEST_CASE("A footprint on screen is filled row by row, within its edges", "[overlay]") {
    const auto rect = convex_rows({10, 30, 30, 10}, {0, 0, 8, 8}, 2.0f);
    REQUIRE(rect.size() == 4);
    for (const auto& q : rect) {
        CHECK(q[0] == Catch::Approx(10.0f));
        CHECK(q[2] == Catch::Approx(20.0f));
        CHECK(q[3] == Catch::Approx(2.0f));
    }

    const auto diamond = convex_rows({0, 10, 0, -10}, {-10, 0, 10, 0}, 2.0f);
    REQUIRE(diamond.size() == 10);
    f32 area = 0.0f;
    for (const auto& q : diamond) {
        const f32 mid = q[1] + q[3] * 0.5f;
        CHECK(q[2] == Catch::Approx(2.0f * (10.0f - std::abs(mid))));
        CHECK(q[0] == Catch::Approx(-q[2] * 0.5f));
        area += q[2] * q[3];
    }
    CHECK(area == Catch::Approx(200.0f));

    CHECK(convex_rows({0, 10, 20, 30}, {5, 5, 5, 5}, 2.0f).empty());
}

TEST_CASE("A placement outline covers its edges and leaves the middle", "[overlay]") {
    const auto rows = osc::renderer::outline_rows({0, 20, 20, 0}, {0, 0, 20, 20}, 2.0f);
    const auto covered = [&](f32 x, f32 y) {
        for (const auto& q : rows) {
            if (x >= q[0] && x <= q[0] + q[2] && y >= q[1] && y <= q[1] + q[3]) {
                return true;
            }
        }
        return false;
    };
    CHECK(covered(10, 0.5f));
    CHECK(covered(19.5f, 10));
    CHECK(covered(0.5f, 19.5f));
    CHECK_FALSE(covered(10, 10));
    CHECK_FALSE(covered(10, 3));
}

TEST_CASE("Placing a structure outlines the pads of those standing", "[overlay]") {
    osc::sim::WorldSnapshot world;
    osc::sim::EntityRecord factory;
    factory.id = 1;
    factory.is_unit = true;
    factory.is_structure = true;
    factory.position = {30.5f, 4.0f, 40.5f};
    factory.footprint_size_x = factory.footprint_size_z = 5;
    factory.skirt_size_x = factory.skirt_size_z = 8;
    factory.skirt_offset_x = factory.skirt_offset_z = -1.5f;
    world.entities.push_back(factory);
    osc::sim::EntityRecord tank;
    tank.id = 2;
    tank.is_unit = true;
    tank.position = {10, 0, 10};
    world.entities.push_back(tank);

    const auto pads =
        osc::renderer::structure_pads(osc::sim::FrameView(&world, &world, 1.0f), nullptr);
    REQUIRE(pads.size() == 1);
    CHECK(pads[0].first.x0 == Catch::Approx(26.5f));
    CHECK(pads[0].first.x1 == Catch::Approx(34.5f));
    CHECK(pads[0].first.z0 == Catch::Approx(36.5f));
    CHECK(pads[0].second == Catch::Approx(4.0f));
}
