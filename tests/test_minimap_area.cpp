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

namespace {

int covered(const std::vector<osc::renderer::UIInstance>& runs, f32 x, f32 y) {
    int n = 0;
    for (const auto& r : runs) {
        if (x >= r.rect[0] && x < r.rect[0] + r.rect[2] && y >= r.rect[1] &&
            y < r.rect[1] + r.rect[3]) {
            ++n;
        }
    }
    return n;
}

} // namespace

TEST_CASE("Minimap camera outline: yellow one-pixel lines, slanted edges too", "[minimap]") {
    using osc::renderer::camera_outline;
    const auto runs =
        camera_outline({{{60, 40}, {140, 40}, {160, 120}, {40, 120}}}, {0, 0, 200, 200});
    REQUIRE_FALSE(runs.empty());
    f32 area = 0;
    for (const auto& r : runs) {
        CHECK((r.rect[2] == 1 || r.rect[3] == 1));
        CHECK(r.color[0] == 1.0f);
        CHECK(r.color[1] == 1.0f);
        CHECK(r.color[2] == 0.0f);
        CHECK(r.color[3] == 1.0f);
        area += r.rect[2] * r.rect[3];
    }
    CHECK(area < 80 + 120 + 2 * 83 + 8);
    CHECK(covered(runs, 50, 80) == 1);
    CHECK(covered(runs, 60, 80) == 0);
    CHECK(covered(runs, 100, 40) == 1);
    CHECK(covered(runs, 100, 80) == 0);
}

TEST_CASE("Minimap camera outline: clipped to the view, not pressed onto its edge", "[minimap]") {
    using osc::renderer::camera_outline;
    const osc::renderer::MapArea view{10, 10, 100, 100};
    const auto runs = camera_outline({{{40, -50}, {80, -50}, {80, 60}, {40, 60}}}, view);
    REQUIRE_FALSE(runs.empty());
    for (const auto& r : runs) {
        CHECK(r.rect[0] >= view.x);
        CHECK(r.rect[1] >= view.y);
        CHECK(r.rect[0] + r.rect[2] <= view.x + view.w);
        CHECK(r.rect[1] + r.rect[3] <= view.y + view.h);
    }
    CHECK(covered(runs, 60, 10) == 0);
    CHECK(covered(runs, 40, 30) == 1);
    CHECK(covered(runs, 60, 60) == 1);
}
