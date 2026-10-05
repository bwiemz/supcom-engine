#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "map/heightmap.hpp"
#include "renderer/camera.hpp"
#include "renderer/shadow_camera.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

using namespace osc;
using namespace osc::renderer;
using Catch::Matchers::WithinAbs;

namespace {

constexpr f32 kW = 1024.0f;
constexpr f32 kH = 768.0f;
constexpr f32 kAspect = kW / kH;

/// A 256 x 256 map, flat at `ground` but for a 40-high mound at (64, 64).
map::Heightmap mound_map(f32 ground = 10.0f) {
    std::vector<u16> raw(257 * 257);
    for (u32 z = 0; z <= 256; ++z)
        for (u32 x = 0; x <= 256; ++x) {
            const f32 d = std::hypot(static_cast<f32>(x) - 64.0f, static_cast<f32>(z) - 64.0f);
            const f32 h = ground + std::max(0.0f, 40.0f - d * 2.0f);
            raw[static_cast<size_t>(z) * 257 + x] = static_cast<u16>(h * 128.0f);
        }
    return {256, 256, 1.0f / 128.0f, std::move(raw)};
}

/// A camera over that map, looking at (x, z) from `zoom`, turned to
/// `heading`, pitched `pitch` (its own for the zoom when negative: near
/// vertical at these zooms).
Camera camera_at(const map::Heightmap& ground, f32 x, f32 z, f32 zoom, f32 heading,
                 f32 pitch = -1.0f) {
    Camera cam;
    cam.set_viewport(kW, kH);
    cam.set_ground(&ground, false, 0.0f);
    cam.init(256.0f, 256.0f);
    cam.set_view(x, z, zoom, heading, pitch < 0.0f ? cam.pitch() : pitch);
    return cam;
}

ShadowView view_of(const Camera& cam, std::array<f32, 3> sun) {
    ShadowView v;
    v.view_proj = cam.view_proj(kAspect);
    v.forward = cam.direction();
    v.zoom = cam.zoom();
    v.sun = sun;
    return v;
}

/// A world point through a column-major matrix, divided by w.
std::array<f32, 3> project(const std::array<f32, 16>& m, f32 x, f32 y, f32 z) {
    std::array<f32, 4> c{};
    for (size_t row = 0; row < 4; ++row)
        c[row] = m[row] * x + m[4 + row] * y + m[8 + row] * z + m[12 + row];
    return {c[0] / c[3], c[1] / c[3], c[2] / c[3]};
}

/// The brute-force box of the cells no single plane has wholly outside.
std::optional<Box> brute_force(const map::Heightmap& h, const std::vector<Plane>& planes) {
    Box box{{1e9f, 1e9f, 1e9f}, {-1e9f, -1e9f, -1e9f}};
    bool any = false;
    for (u32 z = 0; z < h.map_height(); ++z)
        for (u32 x = 0; x < h.map_width(); ++x) {
            const f32 c[4] = {h.get_height_at_grid(x, z), h.get_height_at_grid(x + 1, z),
                              h.get_height_at_grid(x, z + 1), h.get_height_at_grid(x + 1, z + 1)};
            const std::array<f32, 3> lo = {static_cast<f32>(x), *std::min_element(c, c + 4),
                                           static_cast<f32>(z)};
            const std::array<f32, 3> hi = {static_cast<f32>(x + 1), *std::max_element(c, c + 4),
                                           static_cast<f32>(z + 1)};
            bool out = false;
            for (const Plane& p : planes) {
                const f32 px = p[0] >= 0.0f ? hi[0] : lo[0];
                const f32 py = p[1] >= 0.0f ? hi[1] : lo[1];
                const f32 pz = p[2] >= 0.0f ? hi[2] : lo[2];
                if (p[0] * px + p[1] * py + p[2] * pz + p[3] < 0.0f) out = true;
            }
            if (out) continue;
            any = true;
            for (size_t i = 0; i < 3; ++i) {
                box.min[i] = std::min(box.min[i], lo[i]);
                box.max[i] = std::max(box.max[i], hi[i]);
            }
        }
    if (!any) return std::nullopt;
    return box;
}

} // namespace

TEST_CASE("HeightBounds: the cells inside axis planes, as a brute-force scan finds them",
          "[shadow_camera]") {
    const map::Heightmap h = mound_map();
    const HeightBounds bounds(h);
    // x in [40.5, 70.2], z in [55.5, 90.5]: cells 40..70 and 55..90
    const std::vector<Plane> planes = {
        {1, 0, 0, -40.5f}, {-1, 0, 0, 70.2f}, {0, 0, 1, -55.5f}, {0, 0, -1, 90.5f}};
    const std::optional<Box> got = bounds.convex_intersection(planes);
    REQUIRE(got);
    CHECK_THAT(got->min[0], WithinAbs(40.0f, 1e-4f));
    CHECK_THAT(got->max[0], WithinAbs(71.0f, 1e-4f));
    CHECK_THAT(got->min[2], WithinAbs(55.0f, 1e-4f));
    CHECK_THAT(got->max[2], WithinAbs(91.0f, 1e-4f));
    // The mound's top is in, the flat ground too.
    CHECK_THAT(got->min[1], WithinAbs(10.0f, 0.01f));
    CHECK_THAT(got->max[1], WithinAbs(50.0f, 0.01f));
    const std::optional<Box> want = brute_force(h, planes);
    REQUIRE(want);
    for (size_t i = 0; i < 3; ++i) {
        CHECK_THAT(got->min[i], WithinAbs(want->min[i], 1e-4f));
        CHECK_THAT(got->max[i], WithinAbs(want->max[i], 1e-4f));
    }
}

TEST_CASE("HeightBounds: a tilted plane cutting the mound agrees with the brute force",
          "[shadow_camera]") {
    const map::Heightmap h = mound_map();
    const HeightBounds bounds(h);
    // Above a slope through the mound's side, and within a band of x.
    const f32 n = 1.0f / std::sqrt(1.0f + 0.25f + 0.09f);
    const std::vector<Plane> planes = {
        {-1.0f * n, 0.5f * n, 0.3f * n, 20.0f * n}, {1, 0, 0, -30.0f}, {-1, 0, 0, 120.0f}};
    const std::optional<Box> got = bounds.convex_intersection(planes);
    const std::optional<Box> want = brute_force(h, planes);
    REQUIRE(got.has_value() == want.has_value());
    REQUIRE(got);
    for (size_t i = 0; i < 3; ++i) {
        CHECK_THAT(got->min[i], WithinAbs(want->min[i], 1e-4f));
        CHECK_THAT(got->max[i], WithinAbs(want->max[i], 1e-4f));
    }
}

TEST_CASE("HeightBounds: nothing inside, nothing returned", "[shadow_camera]") {
    const map::Heightmap h = mound_map();
    const HeightBounds bounds(h);
    // Everything above y = 100 (the mound's top is 50).
    CHECK_FALSE(bounds.convex_intersection({{0, 1, 0, -100.0f}}));
}

TEST_CASE("shadow_camera: none past ren_ShadowLOD", "[shadow_camera]") {
    const map::Heightmap h = mound_map();
    const HeightBounds bounds(h);
    const Camera near_cam = camera_at(h, 128.0f, 128.0f, 200.0f, Camera::kPi);
    const Camera far_cam = camera_at(h, 128.0f, 128.0f, 300.0f, Camera::kPi);
    CHECK(shadow_camera(view_of(near_cam, {0.3f, 0.8f, 0.5f}), bounds));
    CHECK_FALSE(shadow_camera(view_of(far_cam, {0.3f, 0.8f, 0.5f}), bounds));
}

TEST_CASE("shadow_camera: the terrain in view lies inside the map", "[shadow_camera]") {
    const map::Heightmap h = mound_map();
    const HeightBounds bounds(h);
    const Camera cam = camera_at(h, 64.0f, 64.0f, 120.0f, Camera::kPi);
    const auto vp = shadow_camera(view_of(cam, {0.4f, 0.8f, -0.4f}), bounds);
    REQUIRE(vp);
    // The point the camera looks at, and the mound's top.
    for (const auto& p : {project(*vp, 64.0f, h.get_height(64.0f, 64.0f), 64.0f),
                          project(*vp, 70.0f, h.get_height(70.0f, 60.0f), 60.0f)}) {
        CHECK(std::abs(p[0]) <= 1.0f);
        CHECK(std::abs(p[1]) <= 1.0f);
        CHECK(p[2] >= 0.0f);
        CHECK(p[2] <= 1.0f);
    }
}

TEST_CASE("shadow_camera: the near plane is 25 nearer the sun than the volume's top (raised 8)",
          "[shadow_camera]") {
    // Flat ground at 10 and the sun overhead: the volume's top is 18, its
    // near plane 25 over that.
    const map::Heightmap h(256, 256, 1.0f / 128.0f, std::vector<u16>(257 * 257, 10 * 128));
    const HeightBounds bounds(h);
    const Camera cam = camera_at(h, 128.0f, 128.0f, 120.0f, Camera::kPi);
    const auto vp = shadow_camera(view_of(cam, {0.0f, 1.0f, 0.0f}), bounds);
    REQUIRE(vp);
    const f32 x = 128.0f;
    const f32 z = 128.0f;
    CHECK(project(*vp, x, 18.0f + 24.0f, z)[2] >= 0.0f);
    CHECK(project(*vp, x, 18.0f + 26.0f, z)[2] < 0.0f);
    // The ground is the far plane.
    CHECK_THAT(project(*vp, x, 10.0f, z)[2], WithinAbs(1.0f, 1e-3f));
}

TEST_CASE("shadow_camera: the map's up follows the camera's heading", "[shadow_camera]") {
    const map::Heightmap h = mound_map();
    const HeightBounds bounds(h);
    // The sun overhead: the map's up is the camera's forward, flattened
    // (a tilted camera: one looking straight down takes the fixed up).
    for (const f32 heading : {Camera::kPi, Camera::kPi * 0.5f, 0.3f}) {
        const Camera cam = camera_at(h, 128.0f, 128.0f, 120.0f, heading, 0.8f);
        const auto vp = shadow_camera(view_of(cam, {0.0f, 1.0f, 0.0f}), bounds);
        REQUIRE(vp);
        const std::array<f32, 3> f = cam.direction();
        const f32 len = std::hypot(f[0], f[2]);
        const auto a = project(*vp, 128.0f, 10.0f, 128.0f);
        const auto b =
            project(*vp, 128.0f + 10.0f * f[0] / len, 10.0f, 128.0f + 10.0f * f[2] / len);
        // Along the light map's vertical axis only.
        CHECK(std::abs(b[0] - a[0]) < 1e-3f);
        CHECK(std::abs(b[1] - a[1]) > 0.01f);
    }
}

TEST_CASE("shadow_camera: the map looks down the sun, whatever its length", "[shadow_camera]") {
    const map::Heightmap h = mound_map();
    const HeightBounds bounds(h);
    const Camera cam = camera_at(h, 128.0f, 128.0f, 120.0f, Camera::kPi);
    const std::array<f32, 3> sun = {0.3f, 0.8f, -0.5f};
    const auto vp = shadow_camera(view_of(cam, sun), bounds);
    const auto longer =
        shadow_camera(view_of(cam, {sun[0] * 7.0f, sun[1] * 7.0f, sun[2] * 7.0f}), bounds);
    REQUIRE(vp);
    REQUIRE(longer);
    for (size_t i = 0; i < 16; ++i) CHECK_THAT((*vp)[i], WithinAbs((*longer)[i], 1e-5f));
    // A point moved toward the sun lands on the same texel, nearer.
    const f32 len = std::sqrt(sun[0] * sun[0] + sun[1] * sun[1] + sun[2] * sun[2]);
    const auto a = project(*vp, 128.0f, 10.0f, 128.0f);
    const auto b = project(*vp, 128.0f + 5.0f * sun[0] / len, 10.0f + 5.0f * sun[1] / len,
                           128.0f + 5.0f * sun[2] / len);
    CHECK_THAT(b[0], WithinAbs(a[0], 1e-4f));
    CHECK_THAT(b[1], WithinAbs(a[1], 1e-4f));
    CHECK(b[2] < a[2]);
}


TEST_CASE("shadow_camera: a camera looking down the light takes the fixed up", "[shadow_camera]") {
    const map::Heightmap h = mound_map();
    const HeightBounds bounds(h);
    // Straight down at a sun overhead: forward . L >= 0.99, up (0, 0, -1)
    // whatever the heading: world -z is the light map's vertical axis.
    for (const f32 heading : {Camera::kPi, 0.7f}) {
        const Camera cam = camera_at(h, 128.0f, 128.0f, 120.0f, heading);
        const auto vp = shadow_camera(view_of(cam, {0.0f, 1.0f, 0.0f}), bounds);
        REQUIRE(vp);
        const auto a = project(*vp, 128.0f, 10.0f, 128.0f);
        const auto b = project(*vp, 128.0f, 10.0f, 118.0f);
        CHECK(std::abs(b[0] - a[0]) < 1e-3f);
        CHECK(std::abs(b[1] - a[1]) > 0.01f);
    }
}
