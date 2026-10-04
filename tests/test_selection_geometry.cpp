#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "renderer/input_handler.hpp"
#include "renderer/selection_renderer.hpp"

using osc::renderer::bracket_quads;
using osc::renderer::inside_ground_quad;
using osc::renderer::SelectionBox;
using osc::sim::Vector3;

TEST_CASE("A selection box's brackets sit at its corners, along the unit", "[selection]") {
    SelectionBox box;
    box.center = {10, 2, 20};
    box.right = {0, 0, 1};
    box.forward = {-1, 0, 0};
    box.half_x = 3;
    box.half_z = 1;
    const auto quads = bracket_quads(box, 0.5f);
    CHECK(quads[0][0].x == Catch::Approx(11.0f));
    CHECK(quads[0][0].z == Catch::Approx(17.0f));
    CHECK(quads[0][1].z == Catch::Approx(17.5f));
    CHECK(quads[0][3].x == Catch::Approx(10.5f));
    CHECK(quads[2][0].x == Catch::Approx(9.0f));
    CHECK(quads[2][0].z == Catch::Approx(23.0f));
    CHECK(quads[2][2].x == Catch::Approx(9.5f));
    CHECK(quads[2][2].z == Catch::Approx(22.5f));
    for (const auto& q : quads) {
        for (const Vector3& p : q) {
            CHECK(p.y == Catch::Approx(2.0f));
        }
    }
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

TEST_CASE("A bracket corner holds its stroke to a few pixels, within its box", "[selection]") {
    SelectionBox box;
    box.half_x = 2.4f;
    box.half_z = 2.3f;
    CHECK(osc::renderer::bracket_corner(box, 0.3f, 3.0f, 0.01f) == Catch::Approx(0.69f));
    // Farther off, 3 pixels of stroke, 12 of the texture's 64
    CHECK(osc::renderer::bracket_corner(box, 0.3f, 3.0f, 0.1f) == Catch::Approx(1.6f));
    CHECK(osc::renderer::bracket_corner(box, 0.3f, 3.0f, 0.15f) == Catch::Approx(2.3f));
}

TEST_CASE("A drag box keeps the units of the highest selection priority", "[selection]") {
    using osc::renderer::highest_selection_priority;
    using Ids = std::vector<osc::u32>;
    CHECK(highest_selection_priority({{1, 3}, {2, 1}, {3, 5}}) == Ids{2});
    CHECK(highest_selection_priority({{1, 3}, {3, 5}, {4, 3}}) == Ids{1, 4});
    CHECK(highest_selection_priority({{3, 5}}) == Ids{3});
    CHECK(highest_selection_priority({}).empty());
}
