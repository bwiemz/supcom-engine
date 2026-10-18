// FAF's world drawing (FA-Binary-Patches DrawFunc.cpp and HDraw.cpp, over
// Moho's DRAW_Circle and DRAW_Rect, faf-re): the quads UI_DrawLine,
// UI_DrawCircle and UI_DrawRect make.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "renderer/world_shapes.hpp"

#include <cmath>

using Catch::Approx;
using osc::f32;
using osc::renderer::WorldShapes;
using osc::sim::Vector3;

TEST_CASE("A line is a level band, its thickness at least two pixels at its farther end",
          "[renderer][shapes]") {
    WorldShapes shapes;
    // A pixel is a world unit everywhere
    shapes.begin([](const Vector3&) { return 1.0f; });
    shapes.line({0, 5, 0}, {10, 5, 0}, 0xFF00FF00u, 0.15f);
    REQUIRE(shapes.quads().size() == 1);
    const auto& q = shapes.quads()[0];
    CHECK(q.color == 0xFF00FF00u);
    // 0.15 is under two pixels: two wide, one either side, across z
    CHECK(q.corners[0].x == Approx(0.0f));
    CHECK(q.corners[0].z == Approx(1.0f));
    CHECK(q.corners[1].x == Approx(10.0f));
    CHECK(q.corners[1].z == Approx(1.0f));
    CHECK(q.corners[2].z == Approx(-1.0f));
    CHECK(q.corners[3].z == Approx(-1.0f));
    CHECK(q.corners[0].y == Approx(5.0f));

    // Thicker than two pixels: as given; the farther end's pixel decides
    shapes.begin([](const Vector3& p) { return p.x > 5.0f ? 0.5f : 0.1f; });
    shapes.line({0, 0, 0}, {0, 0, 10}, 0xFFFFFFFFu, 3.0f);
    CHECK(std::abs(shapes.quads()[0].corners[0].x) == Approx(1.5f));
    shapes.begin([](const Vector3& p) { return p.z > 5.0f ? 2.0f : 0.1f; });
    shapes.line({0, 0, 0}, {0, 0, 10}, 0xFFFFFFFFu, 0.15f);
    CHECK(std::abs(shapes.quads()[0].corners[0].x) == Approx(2.0f)); // 2 px of 2 units, halved
}

TEST_CASE("A circle is 48 segments either side of its radius, level", "[renderer][shapes]") {
    WorldShapes shapes;
    shapes.begin([](const Vector3&) { return 0.25f; });
    shapes.circle({100, 7, 200}, 10.0f, 0x80FF0000u, 0.15f);
    REQUIRE(shapes.quads().size() == WorldShapes::kCircleSegments);
    // The band: 0.5 (two pixels of 0.25) either side of the radius
    for (const auto& q : shapes.quads()) {
        const f32 inner = std::hypot(q.corners[0].x - 100.0f, q.corners[0].z - 200.0f);
        const f32 outer = std::hypot(q.corners[1].x - 100.0f, q.corners[1].z - 200.0f);
        CHECK(inner == Approx(9.5f));
        CHECK(outer == Approx(10.5f));
        CHECK(q.corners[2].y == Approx(7.0f));
    }
    // A thickness over two pixels is the band's half
    shapes.begin([](const Vector3&) { return 0.01f; });
    shapes.circle({0, 0, 0}, 10.0f, 0xFFFFFFFFu, 1.0f);
    CHECK(std::hypot(shapes.quads()[0].corners[1].x, shapes.quads()[0].corners[1].z) ==
          Approx(11.0f));
}

TEST_CASE("A box is a rect of any aspect, its corners mitred alike", "[renderer][shapes]") {
    WorldShapes shapes;
    shapes.begin({});
    shapes.box({10, 3, 20}, 8.0f, 4.0f, 0xFF0000FFu, 2.0f);
    REQUIRE(shapes.quads().size() == 4);
    const f32 d = 1.0f / std::sqrt(2.0f);
    const auto& top = shapes.quads()[0];
    CHECK(top.corners[0].x == Approx(10.0f - d)); // top outer left
    CHECK(top.corners[0].z == Approx(20.0f - d));
    CHECK(top.corners[3].x == Approx(18.0f + d)); // top outer right
    CHECK(top.corners[3].z == Approx(20.0f - d));
    const auto& right = shapes.quads()[1];
    CHECK(right.corners[3].x == Approx(18.0f + d)); // bottom outer right
    CHECK(right.corners[3].z == Approx(24.0f + d));
    const auto& left = shapes.quads()[3];
    CHECK(left.corners[1].x == Approx(10.0f - d)); // bottom outer left
    CHECK(left.corners[1].z == Approx(24.0f + d));
    // A square box is the rect
    WorldShapes square;
    square.begin({});
    square.rect({10, 3, 20}, 8.0f, 0xFF0000FFu, 2.0f);
    shapes.begin({});
    shapes.box({10, 3, 20}, 8.0f, 8.0f, 0xFF0000FFu, 2.0f);
    for (size_t q = 0; q < 4; ++q)
        for (size_t c = 0; c < 4; ++c) {
            CHECK(shapes.quads()[q].corners[c].x == Approx(square.quads()[q].corners[c].x));
            CHECK(shapes.quads()[q].corners[c].z == Approx(square.quads()[q].corners[c].z));
        }
}

TEST_CASE("A rect is DRAW_Rect's border from its corner, size across", "[renderer][shapes]") {
    WorldShapes shapes;
    shapes.begin({}); // no camera: the thickness as given
    shapes.rect({10, 3, 20}, 8.0f, 0xFF0000FFu, 2.0f);
    REQUIRE(shapes.quads().size() == 4);
    // DRAW_Rect's corners: half the border along the axes' sum and
    // difference, each normalized (so the diagonals carry it)
    const f32 d = 1.0f / std::sqrt(2.0f);
    const auto& top = shapes.quads()[0];
    CHECK(top.corners[0].x == Approx(10.0f - d)); // top outer left
    CHECK(top.corners[0].z == Approx(20.0f - d));
    CHECK(top.corners[1].x == Approx(10.0f + d)); // top inner left
    CHECK(top.corners[1].z == Approx(20.0f + d));
    const auto& right = shapes.quads()[1];
    CHECK(right.corners[3].x == Approx(18.0f + d)); // bottom outer right
    CHECK(right.corners[3].z == Approx(28.0f + d));
    for (const auto& q : shapes.quads()) CHECK(q.corners[0].y == Approx(3.0f));
    // Each border quad runs round its ring (convex, one turning), so its two
    // triangles, (0, 1, 2) and (2, 3, 0), cover it with no hole
    for (const auto& q : shapes.quads()) {
        int left = 0;
        int right = 0;
        for (int i = 0; i < 4; ++i) {
            const auto& a = q.corners[static_cast<size_t>(i)];
            const auto& b = q.corners[static_cast<size_t>((i + 1) % 4)];
            const auto& c = q.corners[static_cast<size_t>((i + 2) % 4)];
            const f32 cross = (b.x - a.x) * (c.z - b.z) - (b.z - a.z) * (c.x - b.x);
            left += cross > 1e-6f ? 1 : 0;
            right += cross < -1e-6f ? 1 : 0;
        }
        CHECK((left == 4 || right == 4));
    }
}
