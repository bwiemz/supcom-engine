#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "map/heightmap.hpp"
#include "map/terrain.hpp"
#include "renderer/camera.hpp"

#include <array>
#include <cmath>
#include <vector>

using namespace osc;
using namespace osc::renderer;
using Catch::Matchers::WithinAbs;

namespace {

constexpr f32 kW = 800.0f;
constexpr f32 kH = 600.0f;

constexpr f32 kScale = 1.0f / 128.0f;

/// A 64 x 64 map, flat at `height`, with water at `water` if `has_water`.
map::Terrain flat_map(f32 height, f32 water = 0.0f, bool has_water = false) {
    const auto raw = static_cast<u16>(height / kScale);
    return map::Terrain(map::Heightmap(64, 64, kScale, std::vector<u16>(65 * 65, raw)), water,
                        has_water);
}

/// A 64 x 64 map, low (0) west of x = 32 and a plateau at `height` east.
map::Terrain stepped_map(f32 height) {
    std::vector<u16> raw(65 * 65, 0);
    for (u32 z = 0; z <= 64; ++z)
        for (u32 x = 33; x <= 64; ++x) raw[z * 65 + x] = static_cast<u16>(height / kScale);
    return map::Terrain(map::Heightmap(64, 64, kScale, std::move(raw)), 0.0f, false);
}

Camera camera_at(f32 x, f32 z, f32 focus_y, f32 distance) {
    Camera cam;
    cam.init(64.0f, 64.0f);
    cam.set_input_enabled(false);
    cam.set_target(x, z);
    cam.set_target_y(focus_y);
    cam.set_distance(distance);
    return cam;
}

/// A world point's pixel through the camera (y down, as the screen's).
std::array<f32, 2> to_screen(const Camera& cam, f32 x, f32 y, f32 z) {
    const auto m = cam.view_proj(kW / kH);
    const f32 cx = m[0] * x + m[4] * y + m[8] * z + m[12];
    const f32 cy = m[1] * x + m[5] * y + m[9] * z + m[13];
    const f32 cw = m[3] * x + m[7] * y + m[11] * z + m[15];
    return {(cx / cw * 0.5f + 0.5f) * kW, (cy / cw * 0.5f + 0.5f) * kH};
}

} // namespace

TEST_CASE("The camera's focus sits at its target's height (M217a)", "[renderer][camera]") {
    const Camera low = camera_at(32.0f, 32.0f, 0.0f, 40.0f);
    const Camera high = camera_at(32.0f, 32.0f, 20.0f, 40.0f);
    f32 lx, ly, lz, hx, hy, hz;
    low.eye_position(lx, ly, lz);
    high.eye_position(hx, hy, hz);
    CHECK_THAT(hy - ly, WithinAbs(20.0, 1e-4));
    CHECK_THAT(hx, WithinAbs(lx, 1e-4));
    CHECK_THAT(hz, WithinAbs(lz, 1e-4));
    // The focus is the middle of the screen.
    const auto c = to_screen(high, 32.0f, 20.0f, 32.0f);
    CHECK_THAT(c[0], WithinAbs(kW / 2, 0.01));
    CHECK_THAT(c[1], WithinAbs(kH / 2, 0.01));
}

TEST_CASE("A click picks the ground under the cursor (M217a)", "[renderer][camera]") {
    const map::Terrain terrain = flat_map(20.0f);
    const Camera cam = camera_at(32.0f, 32.0f, 20.0f, 40.0f);

    // The screen's middle is the focus.
    f32 x = 0, y = 0, z = 0;
    REQUIRE(cam.pick_ground(kW / 2, kH / 2, kW, kH, &terrain, x, y, z));
    CHECK_THAT(x, WithinAbs(32.0, 0.05));
    CHECK_THAT(y, WithinAbs(20.0, 0.05));
    CHECK_THAT(z, WithinAbs(32.0, 0.05));

    // A point off the middle is picked where it is drawn. The plane y = 0,
    // which clicks had met, lies well beyond it.
    const auto px = to_screen(cam, 40.0f, 20.0f, 26.0f);
    REQUIRE(cam.pick_ground(px[0], px[1], kW, kH, &terrain, x, y, z));
    CHECK_THAT(x, WithinAbs(40.0, 0.05));
    CHECK_THAT(z, WithinAbs(26.0, 0.05));
    f32 fx = 0, fz = 0;
    REQUIRE(cam.screen_to_world(px[0], px[1], kW, kH, 0.0f, fx, fz));
    CHECK(std::abs(fz - 26.0f) + std::abs(fx - 40.0f) > 10.0f);
}

TEST_CASE("A click picks a plateau higher than the camera's focus (M217a)", "[renderer][camera]") {
    // The focus is on the low ground; the cursor is over the plateau.
    const map::Terrain terrain = stepped_map(20.0f);
    const Camera cam = camera_at(24.0f, 40.0f, 0.0f, 50.0f);
    const auto px = to_screen(cam, 44.0f, 20.0f, 36.0f);
    f32 x = 0, y = 0, z = 0;
    REQUIRE(cam.pick_ground(px[0], px[1], kW, kH, &terrain, x, y, z));
    CHECK_THAT(x, WithinAbs(44.0, 0.05));
    CHECK_THAT(y, WithinAbs(20.0, 0.05));
    CHECK_THAT(z, WithinAbs(36.0, 0.05));
}

TEST_CASE("A click over water picks the water's surface (M217a)", "[renderer][camera]") {
    // The seabed at 2, the surface at 10; the camera's focus at 0, so the
    // surface is neither the focus's level nor the ground's.
    const map::Terrain terrain = flat_map(2.0f, 10.0f, true);
    const Camera cam = camera_at(32.0f, 32.0f, 0.0f, 40.0f);
    const auto px = to_screen(cam, 36.0f, 10.0f, 30.0f);
    f32 x = 0, y = 0, z = 0;
    REQUIRE(cam.pick_ground(px[0], px[1], kW, kH, &terrain, x, y, z));
    CHECK_THAT(y, WithinAbs(10.0, 0.05));
    CHECK_THAT(x, WithinAbs(36.0, 0.05));
    CHECK_THAT(z, WithinAbs(30.0, 0.05));
}

TEST_CASE("Off the map, a click meets the level of the camera's focus (M217a)",
          "[renderer][camera]") {
    const map::Terrain terrain = flat_map(20.0f);
    const Camera cam = camera_at(60.0f, 32.0f, 20.0f, 40.0f);
    // A point beyond the map's east edge (x > 64), at the focus's level.
    const auto px = to_screen(cam, 70.0f, 20.0f, 32.0f);
    f32 x = 0, y = 0, z = 0;
    REQUIRE(cam.pick_ground(px[0], px[1], kW, kH, &terrain, x, y, z));
    CHECK_THAT(x, WithinAbs(70.0, 0.05));
    CHECK_THAT(y, WithinAbs(20.0, 0.05));
    // And with no terrain at all.
    REQUIRE(cam.pick_ground(px[0], px[1], kW, kH, nullptr, x, y, z));
    CHECK_THAT(x, WithinAbs(70.0, 0.05));
}
