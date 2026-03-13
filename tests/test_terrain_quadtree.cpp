#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "map/terrain_quadtree.hpp"

using Catch::Matchers::WithinAbs;
using osc::f32;

TEST_CASE("Terrain quadtree build and basic query", "[quadtree]") {
    std::vector<osc::f32> heights = {
        0, 0, 0, 0,
        0, 5, 5, 0,
        0, 5, 5, 0,
        0, 0, 0, 0
    };
    osc::map::TerrainQuadtree qt;
    qt.build(heights.data(), 4, 4, 1.0f);

    osc::f32 hx, hy, hz;
    REQUIRE(qt.raycast(1.5f, 10.0f, 1.5f, 0, -1, 0, hx, hy, hz));
    REQUIRE_THAT(hy, WithinAbs(5.0, 0.5));
}

TEST_CASE("Terrain quadtree ray miss", "[quadtree]") {
    std::vector<osc::f32> heights(16, 0.0f);
    osc::map::TerrainQuadtree qt;
    qt.build(heights.data(), 4, 4, 1.0f);

    osc::f32 hx, hy, hz;
    REQUIRE_FALSE(qt.raycast(0, 10, 0, 1, 0, 0, hx, hy, hz));
}
