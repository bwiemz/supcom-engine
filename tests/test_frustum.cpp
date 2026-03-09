#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "renderer/frustum.hpp"
#include "renderer/camera.hpp"

using namespace osc;
using namespace osc::renderer;

// Build a known VP matrix: camera at (0, 100, 0) looking down at origin
static std::array<f32, 16> make_test_vp() {
    auto view = math::look_at(0, 100, 0,   // eye
                               0, 0, 0,     // target
                               0, 0, -1);   // up
    auto proj = math::perspective(3.14159f / 4.0f, 1.0f, 1.0f, 500.0f);
    return math::mat4_mul(proj, view);
}

TEST_CASE("Frustum: sphere at origin is visible from above", "[frustum]") {
    Frustum f(make_test_vp());
    CHECK(f.is_sphere_visible(0, 0, 0, 5.0f));
}

TEST_CASE("Frustum: sphere far behind camera is not visible", "[frustum]") {
    Frustum f(make_test_vp());
    CHECK_FALSE(f.is_sphere_visible(0, 200, 0, 5.0f));
}

TEST_CASE("Frustum: sphere far outside laterally is not visible", "[frustum]") {
    Frustum f(make_test_vp());
    CHECK_FALSE(f.is_sphere_visible(200, 0, 0, 5.0f));
}

TEST_CASE("Frustum: sphere beyond far plane is not visible", "[frustum]") {
    Frustum f(make_test_vp());
    CHECK_FALSE(f.is_sphere_visible(0, -500, 0, 5.0f));
}

TEST_CASE("Frustum: large sphere partially intersecting is visible", "[frustum]") {
    Frustum f(make_test_vp());
    CHECK(f.is_sphere_visible(50, 0, 0, 20.0f));
}

TEST_CASE("Frustum: RTS camera culls correctly", "[frustum]") {
    Camera cam;
    cam.init(512, 512);
    cam.set_target(256, 256);
    cam.set_distance(300);
    auto vp = cam.view_proj(16.0f / 9.0f);
    Frustum f(vp);

    CHECK(f.is_sphere_visible(256, 0, 256, 10.0f));
    CHECK_FALSE(f.is_sphere_visible(2256, 0, 256, 10.0f));
}
