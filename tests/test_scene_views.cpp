#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "renderer/camera.hpp"
#include "renderer/scene_views.hpp"

#include <vector>

using Catch::Approx;
using osc::renderer::Camera;
using osc::renderer::CameraSet;
using osc::renderer::SceneView;

TEST_CASE("Cameras are found by their names", "[renderer][camera]") {
    Camera world;
    CameraSet cameras(world);
    CHECK(cameras.find("WorldCamera") == &world);
    CHECK(cameras.find("WorldCamera2") == nullptr);

    bool created = false;
    Camera& second = cameras.add("WorldCamera2", created);
    CHECK(created);
    CHECK(&second != &world);
    CHECK(cameras.find("WorldCamera2") == &second);
    CHECK(&cameras.add("WorldCamera2", created) == &second);
    CHECK_FALSE(created);
    CHECK(&cameras.add("WorldCamera", created) == &world);
    CHECK_FALSE(created);
    CHECK(cameras.find("worldcamera2") == nullptr);
    CHECK(cameras.find_tag(&second) == &second);
    CHECK(cameras.find_tag(&world) == &world);
    Camera stray;
    CHECK(cameras.find_tag(&stray) == nullptr);
    CHECK(cameras.find_tag(nullptr) == nullptr);
}

TEST_CASE("A point in a split view picks the ground through that view's camera",
          "[renderer][camera]") {
    Camera left;
    Camera right;
    for (Camera* camera : {&left, &right}) {
        camera->init(512.0f, 512.0f);
        camera->set_viewport(640.0f, 720.0f);
        camera->set_zoom(100.0f);
    }
    left.set_target(100.0f, 120.0f);
    right.set_target(400.0f, 300.0f);
    const std::vector<SceneView> views{{&left, 0.0f, 0.0f, 640.0f, 720.0f, true},
                                       {&right, 640.0f, 0.0f, 640.0f, 720.0f, false}};

    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    REQUIRE(osc::renderer::pick_ground_in(views, 960.0f, 360.0f, nullptr, x, y, z));
    CHECK(x == Approx(right.focus_x()).margin(0.5));
    CHECK(z == Approx(right.focus_z()).margin(0.5));
    REQUIRE(osc::renderer::pick_ground_in(views, 320.0f, 360.0f, nullptr, x, y, z));
    CHECK(x == Approx(left.focus_x()).margin(0.5));
    CHECK(z == Approx(left.focus_z()).margin(0.5));
    CHECK(&osc::renderer::view_under(views, 1000.0f, 100.0f) == &views[1]);
    CHECK(&osc::renderer::view_under(views, 1300.0f, 360.0f) == &views[0]);
}

TEST_CASE("Split views laid out in points are placed in the frame's pixels", "[renderer][camera]") {
    const float scale = GENERATE(1.0f, 2.0f);
    Camera left;
    Camera right;
    const std::vector<SceneView> laid_out{{&left, 0.0f, 0.0f, 399.0f, 600.0f, true},
                                          {&right, 401.0f, 0.0f, 399.0f, 600.0f, false}};

    const std::vector<SceneView> placed =
        osc::renderer::placed_views(laid_out, scale, 800.0f * scale, 600.0f * scale, 2);
    REQUIRE(placed.size() == 2);
    CHECK(placed[0].camera == &left);
    CHECK(placed[0].w == 399.0f * scale);
    CHECK(placed[1].camera == &right);
    CHECK(placed[1].x == 401.0f * scale);
    CHECK(placed[1].w == 399.0f * scale);
    CHECK(placed[1].h == 600.0f * scale);
}
