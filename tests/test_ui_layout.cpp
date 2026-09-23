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
    SECTION("an inverted or empty edge pair falls back per axis") {
        const auto r = control_rect(100, 50, 100, 150, 40, 999);
        CHECK(r.w == 40);   // right == left: no horizontal extent from edges
        CHECK(r.h == 100);  // vertical extent from edges
    }
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
