#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "renderer/minimap_renderer.hpp"

#include <cmath>

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

TEST_CASE("Cartographic minimap: the view's pixels as the UI clips them", "[minimap]") {
    using Catch::Matchers::WithinAbs;
    using osc::renderer::cartographic_minimap;
    // Left, Top, Width and Height each truncated, as the UI renderer's clip
    // takes them: the frame is drawn at that size, pixel for pixel
    const auto m = cartographic_minimap(40.4f, 30.6f, 241.3f, 180.8f, 1024, 1024);
    REQUIRE(m);
    CHECK(m->rect.x == 40);
    CHECK(m->rect.y == 30);
    CHECK(m->rect.w == 241);
    CHECK(m->rect.h == 180);
    CHECK(m->frame.width == 241);
    CHECK(m->frame.height == 180);
    // A square map in a wide view: the map fills it top to bottom, centred,
    // the world running past its sides (the frame's black)
    CHECK(m->frame.z0 == 0);
    CHECK(m->frame.z1 == 1024);
    CHECK_THAT(m->frame.x0 + m->frame.x1, WithinAbs(1024.0, 1e-3));
    CHECK(m->frame.x0 < 0);
    // A pixel is as many world units each way
    CHECK_THAT((m->frame.x1 - m->frame.x0) / 241.0f, WithinAbs(1024.0f / 180.0f, 1e-3));
}

TEST_CASE("Cartographic minimap: on a scaled display, the device pixels the UI clips it to",
          "[minimap]") {
    using Catch::Matchers::WithinAbs;
    using osc::renderer::cartographic_minimap;
    const auto one = cartographic_minimap(40.4f, 30.6f, 241.3f, 180.8f, 1024, 1024);
    const auto two = cartographic_minimap(40.4f, 30.6f, 241.3f, 180.8f, 1024, 1024, 2.0f);
    REQUIRE(one);
    REQUIRE(two);
    CHECK(two->rect.x == 40);
    CHECK(two->rect.y == 30);
    CHECK(two->rect.w == 241);
    CHECK(two->rect.h == 180);
    CHECK(two->frame.width == 482);
    CHECK(two->frame.height == 360);
    CHECK(two->frame.x0 == one->frame.x0);
    CHECK(two->frame.x1 == one->frame.x1);
    CHECK(two->frame.z0 == one->frame.z0);
    CHECK(two->frame.z1 == one->frame.z1);

    const auto odd = cartographic_minimap(41, 30, 241, 180, 1024, 1024, 1.5f);
    REQUIRE(odd);
    CHECK(odd->frame.width == 362);
    CHECK(odd->frame.height == 270);
    CHECK_THAT(odd->rect.x * 1.5f, WithinAbs(61.0, 1e-4));
    CHECK_THAT((odd->rect.x + odd->rect.w) * 1.5f, WithinAbs(423.0, 1e-4));
    CHECK_THAT(odd->rect.y * 1.5f, WithinAbs(45.0, 1e-4));
    CHECK_THAT((odd->rect.y + odd->rect.h) * 1.5f, WithinAbs(315.0, 1e-4));
}

TEST_CASE("Cartographic minimap: a wide map in a square view runs past its top and bottom",
          "[minimap]") {
    const auto m = osc::renderer::cartographic_minimap(0, 0, 200, 200, 1024, 512);
    REQUIRE(m);
    CHECK(m->frame.x0 == 0);
    CHECK(m->frame.x1 == 1024);
    CHECK(m->frame.z0 == -256);
    CHECK(m->frame.z1 == 768);
}

TEST_CASE("Cartographic minimap: at least a pixel; nothing without a map", "[minimap]") {
    using osc::renderer::cartographic_minimap;
    const auto tiny = cartographic_minimap(5.9f, 5.9f, 0.4f, 0.4f, 512, 512);
    REQUIRE(tiny);
    CHECK(tiny->rect.w == 1);
    CHECK(tiny->frame.width == 1);
    CHECK(tiny->frame.height == 1);
    CHECK_FALSE(cartographic_minimap(0, 0, 200, 200, 0, 512));
    CHECK_FALSE(cartographic_minimap(0, 0, std::nanf(""), 200, 512, 512));
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
