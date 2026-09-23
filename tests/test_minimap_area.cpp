#include <catch2/catch_test_macros.hpp>

#include "renderer/minimap_renderer.hpp"

using osc::renderer::fit_map_area;
using osc::f32;

TEST_CASE("Minimap area: a square map fills a square view", "[minimap]") {
    const auto a = fit_map_area(10, 20, 200, 200, 512, 512);
    CHECK(a.x == 10);
    CHECK(a.y == 20);
    CHECK(a.w == 200);
    CHECK(a.h == 200);
}

TEST_CASE("Minimap area: a map keeps its aspect, centred in the view", "[minimap]") {
    // FA's minimap window is resizable; a 2:1 map in a square view is
    // letterboxed, so clicks map to the right world position.
    const auto wide = fit_map_area(0, 0, 200, 200, 1024, 512);
    CHECK(wide.x == 0);
    CHECK(wide.w == 200);
    CHECK(wide.h == 100);
    CHECK(wide.y == 50);

    const auto tall = fit_map_area(100, 0, 300, 150, 512, 512);
    CHECK(tall.w == 150);
    CHECK(tall.h == 150);
    CHECK(tall.x == 175);
    CHECK(tall.y == 0);
}

TEST_CASE("Minimap area: no view or no map draws nothing", "[minimap]") {
    CHECK(fit_map_area(0, 0, 0, 200, 512, 512).w == 0);
    CHECK(fit_map_area(0, 0, 200, 200, 0, 512).w == 0);
}

TEST_CASE("Minimap clicks: the whole view is the minimap's", "[minimap]") {
    using osc::renderer::MapArea;
    using osc::renderer::minimap_to_world;
    // A 1024x512 map letterboxed in a 200x200 view: drawn at y 50..150.
    const MapArea view{0, 0, 200, 200};
    const MapArea area = fit_map_area(0, 0, 200, 200, 1024, 512);
    f32 wx = -1, wz = -1;

    REQUIRE(minimap_to_world(view, area, 100, 100, 1024, 512, wx, wz));
    CHECK(wx == 512);
    CHECK(wz == 256);

    // The letterbox margin is still the minimap: no click-through to the
    // world behind it; it maps to the nearest map edge.
    REQUIRE(minimap_to_world(view, area, 50, 10, 1024, 512, wx, wz));
    CHECK(wx == 256);
    CHECK(wz == 0);

    CHECK_FALSE(minimap_to_world(view, area, 250, 100, 1024, 512, wx, wz)); // outside
    CHECK_FALSE(minimap_to_world({}, {}, 0, 0, 1024, 512, wx, wz));        // not drawn
}
