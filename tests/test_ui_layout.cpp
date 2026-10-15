#include <catch2/catch_test_macros.hpp>

#include "ui/ui_layout.hpp"

using osc::ui::control_rect;

TEST_CASE("A control's rect comes from its MAUI edges", "[ui][layout]") {
    SECTION("edges win over a texture-sized Width") {
        // A bitmap's Width/Height default to its texture (here a 1280x1024
        // loading image); LayoutHelpers.FillParent then sets all four edges
        // to the 1600x900 frame. Moho draws the edges.
        const auto r = control_rect(0, 0, 1600, 900, 1280, 1024);
        CHECK(r.x == 0);
        CHECK(r.y == 0);
        CHECK(r.w == 1600);
        CHECK(r.h == 900);
    }
    SECTION("Width/Height stand in for a missing edge") {
        const auto r = control_rect(10, 20, 0, 0, 64, 32);
        CHECK(r.x == 10);
        CHECK(r.y == 20);
        CHECK(r.w == 64);
        CHECK(r.h == 32);
    }
    SECTION("an empty edge pair is empty, as Moho's") {
        const auto bar = control_rect(441, 883, 441, 887, 74, 4);
        CHECK(bar.w == 0);
        CHECK(bar.h == 4);
        const auto strip = control_rect(1579, 222, 1593, 222, 14, 2);
        CHECK(strip.w == 14);
        CHECK(strip.h == 0);
    }
}

TEST_CASE("A bitmap draws from Left to Right, mirrored when they are inverted", "[ui][layout]") {
    using osc::ui::bitmap_quad;
    const auto strip = bitmap_quad(464, 630, 366, 648, 6, 18);
    CHECK(strip.rect.x == 366);
    CHECK(strip.rect.w == 98);
    CHECK(strip.rect.y == 630);
    CHECK(strip.rect.h == 18);
    CHECK(strip.mirror_x);
    CHECK_FALSE(strip.mirror_y);

    const auto flipped = bitmap_quad(10, 80, 50, 20, 6, 18);
    CHECK(flipped.rect.y == 20);
    CHECK(flipped.rect.h == 60);
    CHECK(flipped.mirror_y);
    CHECK_FALSE(flipped.mirror_x);

    const auto plain = bitmap_quad(0, 0, 1600, 900, 1280, 1024);
    CHECK(plain.rect.w == 1600);
    CHECK(plain.rect.h == 900);
    CHECK_FALSE(plain.mirror_x);
    CHECK_FALSE(plain.mirror_y);

    const auto empty = bitmap_quad(441, 883, 441, 887, 74, 4);
    CHECK(empty.rect.w == 0);
    CHECK_FALSE(empty.mirror_x);
}

TEST_CASE("UI below a main world view is hidden where it covers it", "[ui][layout]") {
    using osc::ui::ControlRect;
    using osc::ui::WorldOccluder;
    using osc::ui::hidden_by_world;
    // FA draws the 3D world into the main WorldView, opaque, at its depth.
    // Retail leaves its loading movie (depth 1) under the game UI; the world
    // view (depth 3, filling the map area) hides it.
    const std::vector<WorldOccluder> views{{{0, 0, 1600, 900}, 3.0f}};

    CHECK(hidden_by_world({0, 0, 1600, 900}, 1.0f, views));      // loading movie
    CHECK(hidden_by_world({700, 640, 200, 20}, 2.0f, views));    // its text
    CHECK_FALSE(hidden_by_world({0, 0, 100, 40}, 5.0f, views));  // a panel above
    CHECK_FALSE(hidden_by_world({0, 0, 100, 40}, 3.0f, views));  // same depth: not below
    // Only where the view covers it: a quad sticking out stays drawn.
    const std::vector<WorldOccluder> map_area{{{0, 100, 1600, 700}, 3.0f}};
    CHECK_FALSE(hidden_by_world({0, 0, 1600, 900}, 1.0f, map_area));
    CHECK_FALSE(hidden_by_world({0, 0, 10, 10}, 1.0f, {}));      // no world view
}

TEST_CASE("UI draws in render-pass bands", "[ui][layout]") {
    using osc::ui::DrawBand;
    using osc::ui::control_draw_band;
    // UIUtil: UIRP_UnderWorld = 1, UIRP_PostGlow = 8. Retail's minimap window
    // background is UnderWorld alone; it draws before the map it frames.
    CHECK(control_draw_band(1) == DrawBand::UnderWorld);
    CHECK(control_draw_band(0) == DrawBand::Overlay);      // no pass set
    CHECK(control_draw_band(8) == DrawBand::Overlay);      // PostGlow (glow, handles)
    CHECK(control_draw_band(1 | 8) == DrawBand::Overlay);  // the main views' mask
    CHECK(DrawBand::UnderWorld < DrawBand::WorldContent);
    CHECK(DrawBand::WorldContent < DrawBand::Overlay);
}
