#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "renderer/camera.hpp"
#include "ui/world_view.hpp"

#include <array>
#include <utility>

using Catch::Approx;

TEST_CASE("A world view's unprojection undoes its projection", "[ui][worldview]") {
    // UnProject (a dragged ping's new place) is Project's inverse: a point on
    // the ground projected to the screen comes back where it was.
    osc::renderer::Camera camera;
    camera.init(512.0f, 512.0f);
    camera.set_target(200.0f, 300.0f);
    camera.set_distance(150.0f);
    osc::ui::WorldView view;
    view.register_camera("WorldCamera", &camera);
    view.set_viewport(1280, 720);

    const std::array<std::pair<float, float>, 4> points{
        {{200.0f, 300.0f}, {230.0f, 280.0f}, {170.0f, 335.0f}, {215.0f, 310.0f}}};
    for (const auto& [x, z] : points) {
        float sx = 0.0f, sy = 0.0f;
        REQUIRE(view.project(x, 0.0f, z, sx, sy));
        float wx = 0.0f, wy = 0.0f, wz = 0.0f;
        REQUIRE(view.get_mouse_world_pos(sx, sy, wx, wy, wz));
        CHECK(wx == Approx(x).margin(0.05));
        CHECK(wz == Approx(z).margin(0.05));
        CHECK(wy == Approx(0.0f).margin(0.05));
    }
}

TEST_CASE("A world view without a camera unprojects nothing", "[ui][worldview]") {
    osc::ui::WorldView view;
    view.set_viewport(1280, 720);
    float wx = 0.0f, wy = 0.0f, wz = 0.0f;
    CHECK_FALSE(view.get_mouse_world_pos(640.0f, 360.0f, wx, wy, wz));
}
