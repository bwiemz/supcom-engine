#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "renderer/input_handler.hpp"
#include "renderer/selection_renderer.hpp"

using osc::renderer::bracket_quads;
using osc::renderer::inside_ground_quad;
using osc::renderer::SelectionBox;
using osc::sim::Vector3;

TEST_CASE("A selection box's bracket tiles are centred on its corners, along the unit",
          "[selection]") {
    SelectionBox box;
    box.center = {10, 2, 20};
    box.right = {0, 0, 1};
    box.forward = {-1, 0, 0};
    box.half_x = 3;
    box.half_z = 1;
    const auto quads = bracket_quads(box, 0.5f);
    CHECK(quads[0][0].x == Catch::Approx(11.5f));
    CHECK(quads[0][0].z == Catch::Approx(16.5f));
    CHECK(quads[0][2].x == Catch::Approx(10.5f));
    CHECK(quads[0][2].z == Catch::Approx(17.5f));
    CHECK(quads[2][0].x == Catch::Approx(9.5f));
    CHECK(quads[2][0].z == Catch::Approx(22.5f));
    CHECK(quads[2][2].x == Catch::Approx(8.5f));
    CHECK(quads[2][2].z == Catch::Approx(23.5f));
    for (const auto& q : quads) {
        for (const Vector3& p : q) {
            CHECK(p.y == Catch::Approx(2.0f));
        }
    }
}

TEST_CASE("A unit's selection box: its blueprint's size scaled, round its mesh's centre",
          "[selection]") {
    osc::renderer::SelectionBlueprint bp;
    bp.size_x = 0.7f;
    bp.size_z = 0.6f;
    bp.offset = {0.1f, 0.0f, -0.4f};
    const Vector3 mesh_min{-1.15f, 0.0f, -0.52f};
    const Vector3 mesh_max{0.95f, 2.0f, 1.42f};
    const osc::sim::Quaternion level{};
    const SelectionBox box =
        osc::renderer::selection_box(bp, mesh_min, mesh_max, {10, 2, 20}, level);
    CHECK(box.half_x == Catch::Approx(0.525f));
    CHECK(box.half_z == Catch::Approx(0.45f));
    CHECK(box.center.x == Catch::Approx(10.0f));
    CHECK(box.center.y == Catch::Approx(2.12f));
    CHECK(box.center.z == Catch::Approx(20.05f));

    const osc::sim::Quaternion quarter{0, 0.70710678f, 0, 0.70710678f};
    const SelectionBox turned =
        osc::renderer::selection_box(bp, mesh_min, mesh_max, {10, 2, 20}, quarter);
    CHECK(turned.center.x == Catch::Approx(10.05f));
    CHECK(turned.center.z == Catch::Approx(20.0f).margin(1e-4));

    const osc::renderer::SelectionBlueprint none;
    const SelectionBox fudged =
        osc::renderer::selection_box(none, mesh_min, mesh_max, {10, 2, 20}, level);
    CHECK(fudged.half_x == Catch::Approx(1.85f * 1.05f));
    CHECK(fudged.half_z == Catch::Approx(1.85f * 0.97f));
}

TEST_CASE("A drag box on the ground holds what is inside it", "[selection]") {
    // A turned camera's box: a rhombus on the ground
    const std::array<Vector3, 4> quad = {{{0, 0, -5}, {5, 0, 0}, {0, 0, 5}, {-5, 0, 0}}};
    CHECK(inside_ground_quad(quad, 0, 0));
    CHECK(inside_ground_quad(quad, 2, 2));
    CHECK_FALSE(inside_ground_quad(quad, 4, 4));
    const std::array<Vector3, 4> back = {quad[3], quad[2], quad[1], quad[0]};
    CHECK(inside_ground_quad(back, 2, 2));
    CHECK_FALSE(inside_ground_quad(back, -4, 4));
}

TEST_CASE("A bracket tile is its box's longer half times its thickness, a few pixels at least",
          "[selection]") {
    SelectionBox box;
    box.half_x = 2.3f;
    box.half_z = 2.4f;
    CHECK(osc::renderer::bracket_corner(box, 0.3f, 3.0f, 0.01f) == Catch::Approx(0.72f));
    CHECK(osc::renderer::bracket_corner(box, 0.3f, 3.0f, 0.1f) == Catch::Approx(0.72f));
    CHECK(osc::renderer::bracket_corner(box, 0.3f, 3.0f, 0.5f) == Catch::Approx(1.5f));
}

TEST_CASE("A drag box keeps the units of the highest selection priority", "[selection]") {
    using osc::renderer::highest_selection_priority;
    using Ids = std::vector<osc::u32>;
    CHECK(highest_selection_priority({{1, 3}, {2, 1}, {3, 5}}) == Ids{2});
    CHECK(highest_selection_priority({{1, 3}, {3, 5}, {4, 3}}) == Ids{1, 4});
    CHECK(highest_selection_priority({{3, 5}}) == Ids{3});
    CHECK(highest_selection_priority({}).empty());
}

TEST_CASE("A ray meets a turned box where it enters, or not at all", "[selection]") {
    using osc::renderer::PickRay;
    using osc::renderer::ray_box_distance;
    const osc::sim::Quaternion level{};
    const Vector3 half{1, 0.5f, 2};
    // Straight down onto its top, 0.5 above its centre.
    const PickRay down{{0, 10, 0}, {0, -1, 0}};
    REQUIRE(ray_box_distance(down, {0, 0, 0}, level, half));
    CHECK(*ray_box_distance(down, {0, 0, 0}, level, half) == Catch::Approx(9.5f));
    // Beside it: past its x half size of 1, within its z of 2.
    CHECK_FALSE(ray_box_distance({{1.5f, 10, 0}, {0, -1, 0}}, {0, 0, 0}, level, half));
    CHECK(ray_box_distance({{0, 10, 1.5f}, {0, -1, 0}}, {0, 0, 0}, level, half));
    // Turned a quarter about y, its long side lies along x.
    const osc::sim::Quaternion quarter{0, 0.70710678f, 0, 0.70710678f};
    CHECK(ray_box_distance({{1.5f, 10, 0}, {0, -1, 0}}, {0, 0, 0}, quarter, half));
    CHECK_FALSE(ray_box_distance({{0, 10, 1.5f}, {0, -1, 0}}, {0, 0, 0}, quarter, half));
    // A slanting ray from the side; one going away; one from inside.
    const PickRay slant{{-10, 10.5f, 0}, {0.70710678f, -0.70710678f, 0}};
    REQUIRE(ray_box_distance(slant, {0, 0, 0}, level, half));
    CHECK(*ray_box_distance(slant, {0, 0, 0}, level, half) == Catch::Approx(14.142f).epsilon(1e-3));
    CHECK_FALSE(ray_box_distance({{0, 10, 0}, {0, 1, 0}}, {0, 0, 0}, level, half));
    CHECK(*ray_box_distance({{0, 0, 0}, {1, 0, 0}}, {0, 0, 0}, level, half) == 0.0f);
}

TEST_CASE("A world point lands on the screen as the overlays project it", "[selection]") {
    using osc::renderer::screen_point;
    // Identity: clip space is the world, w = 1; y runs down the screen.
    std::array<osc::f32, 16> identity{};
    identity[0] = identity[5] = identity[10] = identity[15] = 1.0f;
    const auto mid = screen_point(identity, {0, 0, 0}, 800, 600);
    REQUIRE(mid);
    CHECK((*mid)[0] == Catch::Approx(400.0f));
    CHECK((*mid)[1] == Catch::Approx(300.0f));
    const auto corner = screen_point(identity, {1, 1, 0}, 800, 600);
    REQUIRE(corner);
    CHECK((*corner)[0] == Catch::Approx(800.0f));
    CHECK((*corner)[1] == Catch::Approx(600.0f));
    // Behind the camera: w <= 0.
    std::array<osc::f32, 16> behind = identity;
    behind[15] = -1.0f;
    CHECK_FALSE(screen_point(behind, {0, 0, 0}, 800, 600));
}
