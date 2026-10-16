// A cartographic world view's camera (CameraImpl::CameraSetOrtho, faf-re
// render/camera/CameraImpl.cpp's UpdateCoords): from straight above the
// focus, -z at the top, through Moho's orthographic projection the zoom
// wide (its D3D9 half-texel shift kept); no shake; picking straight down.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "map/heightmap.hpp"
#include "renderer/camera.hpp"

#include <array>
#include <cmath>
#include <vector>

using namespace osc;
using namespace osc::renderer;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

constexpr f32 kW = 1024.0f;
constexpr f32 kH = 768.0f;
constexpr f32 kAspect = kW / kH;
constexpr f32 kGround = 10.0f;

map::Heightmap flat() {
    return {256, 256, 1.0f / 128.0f,
            std::vector<u16>(257 * 257, static_cast<u16>(kGround * 128.0f))};
}

/// A camera over the flat map, its view held at (x, z), `zoom` wide.
Camera camera_at(const map::Heightmap& ground, f32 x, f32 z, f32 zoom) {
    Camera cam;
    cam.set_viewport(kW, kH);
    cam.set_ground(&ground, false, 0.0f);
    cam.init(256.0f, 256.0f);
    cam.set_view(x, z, zoom, Camera::kPi, 1.2f);
    cam.frame(0.0);
    return cam;
}

/// A world point through view_proj: normalized device x, y (Vulkan's, down)
/// and depth.
std::array<f32, 3> ndc(const std::array<f32, 16>& m, f32 x, f32 y, f32 z) {
    const f32 cx = m[0] * x + m[4] * y + m[8] * z + m[12];
    const f32 cy = m[1] * x + m[5] * y + m[9] * z + m[13];
    const f32 cz = m[2] * x + m[6] * y + m[10] * z + m[14];
    const f32 cw = m[3] * x + m[7] * y + m[11] * z + m[15];
    return {cx / cw, cy / cw, cz / cw};
}

} // namespace

TEST_CASE("Moho's orthographic projection, flipped for Vulkan, with its shift", "[camera]") {
    // UpdateCoords: r0.x = 2/zoom, r1.y = 2/extent, r2.z = 1/(far - near)
    // and r3.z = near/(near - far) with both negated, r3.x = -1/int(zoom),
    // r3.y = +1/int(extent), r3.w = 1
    const f32 zoom = 100.7f;
    const f32 extent = zoom / kAspect; // 75.5
    const auto p = math::ortho_projection(zoom, kAspect, 1.0f, 1001.0f);
    CHECK_THAT(p[0], WithinRel(2.0f / zoom, 1e-6f));
    CHECK_THAT(p[5], WithinRel(-2.0f / extent, 1e-6f));
    CHECK_THAT(p[10], WithinRel(-1.0f / 1000.0f, 1e-6f));
    CHECK_THAT(p[12], WithinRel(-1.0f / 100.0f, 1e-6f)); // int(100.7)
    CHECK_THAT(p[13], WithinRel(-1.0f / 75.0f, 1e-6f));  // flipped +1/int(75.5)
    CHECK_THAT(p[14], WithinRel(-1.0f / 1000.0f, 1e-6f));
    CHECK(p[15] == 1.0f);
    CHECK(p[3] == 0.0f);
    CHECK(p[7] == 0.0f);
    CHECK(p[11] == 0.0f);
    // A view under a unit high takes a whole unit for its shift
    const auto thin = math::ortho_projection(5.0f, 8.0f, 1.0f, 2.0f);
    CHECK(std::isfinite(thin[13]));
    CHECK_THAT(thin[13], WithinAbs(-1.0f, 1e-6f));
}

TEST_CASE("Ortho, the view is from straight above the focus, -z at the top", "[camera]") {
    const auto ground = flat();
    Camera cam = camera_at(ground, 128.0f, 128.0f, 80.0f);
    const std::array<f32, 3> focus = {cam.focus_x(), cam.focus_y(), cam.focus_z()};
    const auto before = cam.view_proj(kAspect);
    cam.set_ortho(true);
    REQUIRE(cam.ortho());
    // The same focus: it is found along the heading and pitch as before
    cam.frame(0.0);
    CHECK_THAT(cam.focus_x(), WithinAbs(focus[0], 1e-4f));
    CHECK_THAT(cam.focus_z(), WithinAbs(focus[2], 1e-4f));

    f32 ex = 0;
    f32 ey = 0;
    f32 ez = 0;
    cam.eye_position(ex, ey, ez);
    CHECK_THAT(ex, WithinAbs(cam.focus_x(), 1e-4f));
    CHECK_THAT(ez, WithinAbs(cam.focus_z(), 1e-4f));
    CHECK_THAT(ey - cam.focus_y(), WithinRel(cam.eye_distance(), 1e-5f));
    const auto d = cam.direction();
    CHECK(d[0] == 0.0f);
    CHECK(d[1] == -1.0f);
    CHECK(d[2] == 0.0f);

    const auto vp = cam.view_proj(kAspect);
    CHECK(vp != before);
    const f32 zoom = cam.zoom();
    const f32 extent = zoom / kAspect;
    const f32 shift_x = 1.0f / static_cast<f32>(static_cast<int>(zoom));
    const f32 shift_y = 1.0f / static_cast<f32>(static_cast<int>(extent));
    // The focus at the centre, less the shift
    const auto c = ndc(vp, cam.focus_x(), kGround, cam.focus_z());
    CHECK_THAT(c[0], WithinAbs(-shift_x, 1e-5f));
    CHECK_THAT(c[1], WithinAbs(-shift_y, 1e-5f));
    // East is right, north (-z) up the screen; the zoom spans the width
    const auto east = ndc(vp, cam.focus_x() + zoom * 0.25f, kGround, cam.focus_z());
    CHECK_THAT(east[0] - c[0], WithinAbs(0.5f, 1e-5f));
    CHECK_THAT(east[1] - c[1], WithinAbs(0.0f, 1e-5f));
    const auto north = ndc(vp, cam.focus_x(), kGround, cam.focus_z() - extent * 0.25f);
    CHECK_THAT(north[1] - c[1], WithinAbs(-0.5f, 1e-5f));
    CHECK_THAT(north[0] - c[0], WithinAbs(0.0f, 1e-5f));
    // Orthographic: a point's height moves it on the screen not at all, but
    // nearer the eye it is nearer in depth
    const auto high = ndc(vp, cam.focus_x() + 7.0f, kGround + 40.0f, cam.focus_z() - 3.0f);
    const auto low = ndc(vp, cam.focus_x() + 7.0f, kGround, cam.focus_z() - 3.0f);
    CHECK_THAT(high[0], WithinAbs(low[0], 1e-5f));
    CHECK_THAT(high[1], WithinAbs(low[1], 1e-5f));
    CHECK(high[2] < low[2]);
    CHECK(low[2] > 0.0f);
    CHECK(low[2] < 1.0f);

    // Back to perspective, the view as it was
    cam.set_ortho(false);
    CHECK(cam.view_proj(kAspect) == before);
}

TEST_CASE("Ortho, the view doesn't shake (CanShake)", "[camera]") {
    const auto ground = flat();
    Camera cam = camera_at(ground, 100.0f, 140.0f, 60.0f);
    cam.set_ortho(true);
    const auto still = cam.view_proj(kAspect);
    cam.apply_shake(3.0f);
    CHECK(cam.view_proj(kAspect) == still);
    cam.set_ortho(false);
    const auto steady = camera_at(ground, 100.0f, 140.0f, 60.0f).view_proj(kAspect);
    CHECK(cam.view_proj(kAspect) != steady); // the shake shows in perspective
}

TEST_CASE("Ortho, a pixel's ray is straight down from over the point it shows", "[camera]") {
    const auto ground = flat();
    Camera cam = camera_at(ground, 120.0f, 90.0f, 70.0f);
    cam.set_ortho(true);
    const auto vp = cam.view_proj(kAspect);
    for (const auto& [sx, sy] : std::array<std::array<f32, 2>, 4>{
             {{512.0f, 384.0f}, {0.0f, 0.0f}, {1000.0f, 700.0f}, {200.5f, 650.25f}}}) {
        f32 o[3];
        f32 d[3];
        REQUIRE(cam.screen_ray(sx, sy, kW, kH, o, d));
        CHECK(d[0] == 0.0f);
        CHECK(d[1] == -1.0f);
        CHECK(d[2] == 0.0f);
        // The point under it is drawn at that pixel
        f32 gx = 0;
        f32 gz = 0;
        REQUIRE(cam.screen_to_world(sx, sy, kW, kH, kGround, gx, gz));
        CHECK_THAT(gx, WithinAbs(o[0], 1e-4f));
        CHECK_THAT(gz, WithinAbs(o[2], 1e-4f));
        const auto n = ndc(vp, gx, kGround, gz);
        CHECK_THAT((n[0] + 1.0f) * 0.5f * kW, WithinAbs(sx, 1e-2f));
        CHECK_THAT((n[1] + 1.0f) * 0.5f * kH, WithinAbs(sy, 1e-2f));
        f32 px = 0;
        f32 py = 0;
        f32 pz = 0;
        REQUIRE(cam.pick_ground(sx, sy, kW, kH, nullptr, px, py, pz));
        CHECK_THAT(px, WithinAbs(gx, 1e-3f));
        CHECK_THAT(pz, WithinAbs(gz, 1e-3f));
    }
}

TEST_CASE("Ortho, zoom and pan go on as in perspective", "[camera]") {
    const auto ground = flat();
    Camera cam = camera_at(ground, 128.0f, 128.0f, 100.0f);
    Camera ortho = camera_at(ground, 128.0f, 128.0f, 100.0f);
    ortho.set_ortho(true);
    cam.zoom(-3.0f);
    ortho.zoom(-3.0f);
    cam.frame(0.25);
    ortho.frame(0.25);
    CHECK_THAT(ortho.zoom(), WithinRel(cam.zoom(), 1e-6f));
    // A drag right moves the target west, a drag down north, by the zoom
    // over the view's width a pixel
    const f32 x0 = ortho.target_x();
    const f32 z0 = ortho.target_z();
    ortho.pan(10.0f, 0.0f);
    CHECK_THAT(ortho.target_x() - x0, WithinRel(-10.0f * ortho.zoom() / kW, 1e-4f));
    CHECK_THAT(ortho.target_z(), WithinAbs(z0, 1e-4f));
    ortho.pan(0.0f, 10.0f);
    CHECK_THAT(ortho.target_z() - z0, WithinRel(-10.0f * ortho.zoom() / kW, 1e-4f));
}
