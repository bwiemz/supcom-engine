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
constexpr f32 kDeg = Camera::kPi / 180.0f;

/// A flat 256 x 256 map at height 10.
map::Heightmap flat() {
    return {256, 256, 1.0f / 128.0f,
            std::vector<u16>(257 * 257, static_cast<u16>(kGround * 128.0f))};
}

/// A camera over that map, reset (CameraReset).
Camera camera_over(const map::Heightmap& ground) {
    Camera cam;
    cam.set_viewport(kW, kH);
    cam.set_ground(&ground, false, 0.0f);
    cam.init(256.0f, 256.0f);
    return cam;
}

f32 max_zoom() {
    return 256.0f * 1.4f * kAspect;
}

/// Moho's log-zoom blend: 0 at the nearest zoom, 1 at the farthest.
f32 blend(f32 zoom) {
    return (std::log(zoom) - std::log(5.0f)) / (std::log(max_zoom()) - std::log(5.0f));
}

/// A world point's normalized device coordinates through the camera.
std::array<f32, 2> ndc(const Camera& cam, f32 x, f32 y, f32 z) {
    const auto m = cam.view_proj(kAspect);
    const f32 cx = m[0] * x + m[4] * y + m[8] * z + m[12];
    const f32 cy = m[1] * x + m[5] * y + m[9] * z + m[13];
    const f32 cw = m[3] * x + m[7] * y + m[11] * z + m[15];
    return {cx / cw, cy / cw};
}

} // namespace

TEST_CASE("The farthest zoom is the rect's extent times 1.4, at the view's aspect (M217f)",
          "[camera]") {
    const map::Heightmap ground = flat();
    Camera cam = camera_over(ground);
    CHECK_THAT(cam.max_zoom(), WithinRel(max_zoom(), 1e-5f));
    // A playable rect narrows it; SetMaxZoomMult scales it
    cam.set_playable_rect(64.0f, 64.0f, 192.0f, 160.0f);
    CHECK_THAT(cam.max_zoom(), WithinRel(std::max(128.0f * 1.4f, 96.0f * 1.4f * kAspect), 1e-5f));
    cam.set_max_zoom_mult(2.0f);
    CHECK_THAT(cam.max_zoom(), WithinRel(std::max(128.0f * 2.0f, 96.0f * 2.0f * kAspect), 1e-5f));
}

TEST_CASE("CameraReset: the whole map from above (M217f)", "[camera]") {
    const map::Heightmap ground = flat();
    const Camera cam = camera_over(ground);
    CHECK_THAT(cam.heading(), WithinAbs(Camera::kPi, 1e-6));
    CHECK_THAT(cam.pitch(), WithinAbs(89.9f * kDeg, 1e-6));
    CHECK_THAT(cam.fov(), WithinAbs(60.0f * kDeg, 1e-6));
    CHECK_THAT(cam.zoom(), WithinRel(max_zoom(), 1e-5f));
    CHECK_THAT(cam.requested_zoom(), WithinRel(max_zoom(), 1e-5f));
    CHECK_THAT(cam.target_x(), WithinAbs(128.0, 1e-4));
    CHECK_THAT(cam.target_z(), WithinAbs(128.0, 1e-4));
    CHECK_THAT(cam.focus_y(), WithinAbs(kGround, 1e-3));
    CHECK_THAT(cam.eye_distance(), WithinRel(max_zoom() / std::tan(30.0f * kDeg) / 2.0f, 1e-5f));
    CHECK_FALSE(cam.rotated());
}

TEST_CASE("A wheel notch zooms 2^-0.05 and the zoom glides in log space (M217f)", "[camera]") {
    const map::Heightmap ground = flat();
    Camera cam = camera_over(ground);
    const f32 start = cam.zoom();
    cam.zoom(1.0f);
    CHECK_THAT(cam.requested_zoom(), WithinRel(start * std::exp2(-0.05f), 1e-5f));
    CHECK_THAT(cam.zoom(), WithinRel(start, 1e-6f)); // not until a frame
    cam.zoom(-3.0f);
    CHECK_THAT(cam.requested_zoom(), WithinRel(start, 1e-5f)); // clamped at the farthest
    cam.zoom(1000.0f);
    CHECK_THAT(cam.requested_zoom(), WithinAbs(5.0, 1e-5)); // and the nearest

    // A frame's step: (|log2 delta| * 8 + 1) * dt in log2
    const f32 delta = std::log2(5.0f) - std::log2(start);
    const f32 dt = 1.0f / 60.0f;
    cam.frame(dt);
    const f32 step = (std::abs(delta) * 8.0f + 1.0f) * dt;
    CHECK_THAT(cam.zoom(), WithinRel(std::exp2(std::log2(start) - step), 1e-4f));
    // It settles on the zoom asked for
    for (int i = 0; i < 600; ++i) cam.frame(dt);
    CHECK_THAT(cam.zoom(), WithinAbs(5.0, 1e-3));
}

TEST_CASE("The pitch and FOV follow the log of the zoom (M217f)", "[camera]") {
    const map::Heightmap ground = flat();
    Camera cam = camera_over(ground);
    cam.set_zoom(5.0f);
    CHECK_THAT(cam.pitch(), WithinAbs(40.0f * kDeg, 1e-5));
    CHECK_THAT(cam.fov(), WithinAbs(65.0f * kDeg, 1e-5));
    cam.set_zoom(max_zoom());
    CHECK_THAT(cam.pitch(), WithinAbs(89.9f * kDeg, 1e-5));
    CHECK_THAT(cam.fov(), WithinAbs(60.0f * kDeg, 1e-5));
    for (const f32 z : {12.0f, 60.0f, 200.0f}) {
        cam.set_zoom(z);
        cam.frame(0.0);
        CHECK_THAT(cam.pitch(), WithinAbs((40.0f + blend(z) * 49.9f) * kDeg, 1e-4));
        CHECK_THAT(cam.fov(), WithinAbs((65.0f - blend(z) * 5.0f) * kDeg, 1e-4));
    }
}

TEST_CASE("The eye sits zoom / tan(fov/2) / 2 back along the view (M217f)", "[camera]") {
    const map::Heightmap ground = flat();
    Camera cam = camera_over(ground);
    cam.set_zoom(80.0f);
    cam.set_target(100.0f, 120.0f);
    cam.frame(0.0);
    const f32 d = 80.0f / std::tan(cam.fov() * 0.5f) / 2.0f;
    CHECK_THAT(cam.eye_distance(), WithinRel(d, 1e-5f));
    f32 ex = 0;
    f32 ey = 0;
    f32 ez = 0;
    cam.eye_position(ex, ey, ez);
    const f32 p = cam.pitch();
    // Heading pi looks toward -z: the eye is south of the focus, above it
    CHECK_THAT(ex, WithinAbs(100.0, 1e-3));
    CHECK_THAT(ey, WithinAbs(kGround + d * std::sin(p), 1e-3));
    CHECK_THAT(ez, WithinAbs(120.0 + d * std::cos(p), 1e-3));
    // The clip planes
    CHECK_THAT(cam.near_clip(), WithinRel(0.01f * d, 1e-5f));
    CHECK_THAT(cam.far_clip(), WithinRel(d + 17000.0f, 1e-5f));
}

TEST_CASE("The zoom is the world's extent across the view at the focus (M217f)", "[camera]") {
    const map::Heightmap ground = flat();
    Camera cam = camera_over(ground);
    cam.set_zoom(100.0f);
    cam.set_target(128.0f, 128.0f);
    cam.frame(0.0);
    // Half the zoom along the view's right is the screen's edge
    const auto v = cam.view();
    const f32 rx = v[0];
    const f32 rz = v[8];
    const auto edge =
        ndc(cam, cam.focus_x() + rx * 50.0f, cam.focus_y(), cam.focus_z() + rz * 50.0f);
    CHECK_THAT(edge[0], WithinAbs(1.0, 1e-4));
    CHECK_THAT(edge[1], WithinAbs(0.0, 1e-4));
    // The FOV is the horizontal one: the vertical half is tan(fov/2) / aspect
    CHECK_THAT(cam.tan_half_fov_y(kAspect), WithinRel(std::tan(cam.fov() * 0.5f) / kAspect, 1e-6f));
}

TEST_CASE("The target keeps to the rect, less the zoom's share (M217f)", "[camera]") {
    const map::Heightmap ground = flat();
    Camera cam = camera_over(ground);
    cam.set_zoom(max_zoom() * 0.5f);
    cam.set_target(0.0f, 256.0f);
    cam.frame(0.0);
    // (zoom / max) * half the extent in from each side: 64
    CHECK_THAT(cam.target_x(), WithinAbs(64.0, 1e-3));
    CHECK_THAT(cam.target_z(), WithinAbs(192.0, 1e-3));
    // At the farthest zoom, the middle
    cam.set_zoom(max_zoom());
    cam.frame(0.0);
    CHECK_THAT(cam.target_x(), WithinAbs(128.0, 1e-3));
    // Free (cam_Free), it isn't kept
    cam.set_free(true);
    cam.set_zoom(20.0f);
    cam.set_target(-40.0f, 300.0f);
    cam.frame(0.0);
    CHECK_THAT(cam.target_x(), WithinAbs(-40.0, 1e-3));
}

TEST_CASE("A pan moves the target a zoom's width per view width (M217f)", "[camera]") {
    const map::Heightmap ground = flat();
    Camera cam = camera_over(ground);
    cam.set_zoom(64.0f);
    cam.set_target(128.0f, 128.0f);
    cam.frame(0.0);
    // Heading pi: the view's right is +x, its up flattened is -z (north)
    cam.pan(16.0f, 0.0f);
    CHECK_THAT(cam.target_x(), WithinAbs(128.0 - 16.0 * 64.0 / kW, 1e-3));
    CHECK_THAT(cam.target_z(), WithinAbs(128.0, 1e-3));
    cam.pan(0.0f, 16.0f);
    CHECK_THAT(cam.target_z(), WithinAbs(128.0 - 16.0 * 64.0 / kW, 1e-3));
}

TEST_CASE("A spin turns and tilts, and a revert glides it back (M217f)", "[camera]") {
    const map::Heightmap ground = flat();
    Camera cam = camera_over(ground);
    cam.set_zoom(40.0f);
    cam.frame(0.0);
    const f32 zoom_pitch = cam.pitch();
    // 360 degrees across the view's width: a quarter of it is 90 degrees
    cam.spin(kW / 4.0f, 0.0f);
    CHECK(cam.rotated());
    CHECK_THAT(cam.heading(), WithinAbs(Camera::kPi / 2.0f, 1e-4));
    cam.spin(0.0f, -kW); // tilts up, to the floor of 0.1
    CHECK_THAT(cam.pitch(), WithinAbs(0.1, 1e-6));
    cam.spin(0.0f, 2.0f * kW); // down, to the ceiling
    CHECK_THAT(cam.pitch(), WithinAbs(1.5607964, 1e-6));
    // Held: a frame keeps it
    cam.frame(1.0 / 60.0);
    CHECK_THAT(cam.pitch(), WithinAbs(1.5607964, 1e-6));
    // Reverted: 0.1 a frame toward heading pi and the zoom's pitch
    cam.revert_rotation();
    cam.frame(1.0 / 60.0);
    CHECK_THAT(cam.heading(), WithinAbs(Camera::kPi / 2.0f + 0.1f, 1e-4));
    CHECK_THAT(cam.pitch(), WithinAbs(1.5607964 - 0.1, 1e-4));
    for (int i = 0; i < 60; ++i) cam.frame(1.0 / 60.0);
    CHECK_FALSE(cam.rotated());
    CHECK_THAT(cam.heading(), WithinAbs(Camera::kPi, 1e-6));
    CHECK_THAT(cam.pitch(), WithinAbs(zoom_pitch, 1e-4));
}

TEST_CASE("Closing in, the target is drawn toward the ground under the pivot (M217f)", "[camera]") {
    const map::Heightmap ground = flat();
    Camera cam = camera_over(ground);
    cam.set_zoom(120.0f);
    cam.set_target(128.0f, 128.0f);
    cam.frame(0.0);
    // The ground under a point right of and above the middle
    const f32 px = kW * 0.75f;
    const f32 py = kH * 0.3f;
    f32 o[3];
    f32 d[3];
    REQUIRE(cam.screen_ray(px, py, kW, kH, o, d));
    const f32 t = (kGround - o[1]) / d[1];
    const f32 hx = o[0] + d[0] * t;
    const f32 hz = o[2] + d[2] * t;
    const f32 before = cam.zoom();
    cam.set_pivot(px, py);
    cam.zoom(4.0f);
    cam.frame(1.0 / 60.0);
    const f32 ratio = cam.zoom() / before;
    REQUIRE(ratio < 1.0f);
    CHECK_THAT(cam.target_x(), WithinAbs(hx + (128.0f - hx) * ratio, 1e-2));
    CHECK_THAT(cam.target_z(), WithinAbs(hz + (128.0f - hz) * ratio, 1e-2));
}

TEST_CASE("TargetManual sets the view at once, held; SetTargetZoom only the goal (M217f)",
          "[camera]") {
    const map::Heightmap ground = flat();
    Camera cam = camera_over(ground);
    // The heading unwraps nearest the current (pi)
    cam.target_manual(50.0f, kGround, 60.0f, -Camera::kPi + 0.2f, 0.5f, 900.0f);
    CHECK(cam.rotated());
    CHECK_THAT(cam.heading(), WithinAbs(Camera::kPi + 0.2f, 1e-5));
    CHECK_THAT(cam.pitch(), WithinAbs(0.5, 1e-6));
    CHECK_THAT(cam.zoom(), WithinAbs(900.0, 1e-3)); // unclamped
    CHECK_THAT(cam.target_x(), WithinAbs(50.0, 1e-4));
    cam.set_requested_zoom(33.0f);
    CHECK_THAT(cam.requested_zoom(), WithinAbs(33.0, 1e-6));
    CHECK_THAT(cam.zoom(), WithinAbs(900.0, 1e-3));
}

TEST_CASE("A view framed by its eye's distance puts the eye there (M217f)", "[camera]") {
    const map::Heightmap ground = flat();
    Camera cam = camera_over(ground);
    for (const f32 d : {20.0f, 150.0f, 300.0f}) {
        cam.set_eye_distance(d);
        CHECK_THAT(cam.eye_distance(), WithinRel(d, 1e-4f));
    }
}

TEST_CASE("The keys pan 90 pixels a frame, Ctrl four times; not with Alt (M217f)", "[camera]") {
    const map::Heightmap ground = flat();
    Camera cam = camera_over(ground);
    cam.set_zoom(64.0f);
    cam.set_target(128.0f, 128.0f);
    cam.apply({}, 0.0);
    const f32 px = 64.0f / kW; // a pixel's world units at this zoom
    CameraInput in;
    in.left = true; // +dx: against the view's right, west
    cam.apply(in, 0.0);
    CHECK_THAT(cam.target_x(), WithinAbs(128.0 - 90.0 * px, 1e-3));
    in.left = false;
    in.up = true; // +dy: forward, north
    in.control = true;
    cam.apply(in, 0.0);
    CHECK_THAT(cam.target_z(), WithinAbs(128.0 - 360.0 * px, 1e-3));
    // The screen's edges pan as the keys do; Alt stops both
    const f32 x = cam.target_x();
    CameraInput edge;
    edge.at_right = true;
    edge.alt = true;
    cam.apply(edge, 0.0);
    CHECK_THAT(cam.target_x(), WithinAbs(x, 1e-4));
    edge.alt = false;
    cam.apply(edge, 0.0);
    CHECK_THAT(cam.target_x(), WithinAbs(x + 90.0 * px, 1e-3));
}

TEST_CASE("Insert and Delete turn the view 10 pixels a frame, Ctrl twice (M217f)", "[camera]") {
    const map::Heightmap ground = flat();
    Camera cam = camera_over(ground);
    CameraInput in;
    in.insert = true;
    cam.apply(in, 0.0);
    const f32 step = 10.0f * 360.0f / kW * kDeg;
    CHECK(cam.rotated());
    CHECK_THAT(cam.heading(), WithinAbs(Camera::kPi - step, 1e-4));
    in.insert = false;
    in.del = true;
    in.control = true;
    cam.apply(in, 0.0);
    // pi + step, wrapped into [-pi, pi]
    CHECK_THAT(cam.heading(), WithinAbs(-Camera::kPi + step, 1e-4));
}

TEST_CASE("Space spins the view near the ground; let go, it turns back (M217f)", "[camera]") {
    const map::Heightmap ground = flat();
    Camera cam = camera_over(ground);
    cam.set_zoom(60.0f); // nearer than 125
    const f32 zoom_pitch = cam.pitch();
    CameraInput in;
    in.space = true;
    in.mouse_x = 100.0f;
    cam.apply(in, 0.0); // the first motion arms it
    CHECK_FALSE(cam.rotated());
    in.mouse_x = 100.0f + kW / 8.0f;
    in.mouse_y = -40.0f;
    cam.apply(in, 0.0);
    CHECK(cam.rotated());
    CHECK_THAT(cam.heading(), WithinAbs(Camera::kPi - Camera::kPi / 4.0f, 1e-4));
    // Up the screen tilts it up: 40 pixels of 360 degrees a view width
    CHECK_THAT(cam.pitch(), WithinAbs(zoom_pitch - 40.0f * 360.0f / kW * kDeg, 1e-4));
    // Released: it glides back
    in.space = false;
    for (int i = 0; i < 60; ++i) cam.apply(in, 1.0 / 60.0);
    CHECK_FALSE(cam.rotated());
    CHECK_THAT(cam.heading(), WithinAbs(Camera::kPi, 1e-6));

    // Out past 125 it doesn't spin
    Camera far = camera_over(ground);
    far.set_zoom(200.0f);
    CameraInput spin;
    spin.space = true;
    far.apply(spin, 0.0);
    spin.mouse_x = 300.0f;
    far.apply(spin, 0.0);
    CHECK_FALSE(far.rotated());
    // Over a UI control it doesn't either
    Camera covered = camera_over(ground);
    covered.set_zoom(60.0f);
    covered.set_mouse_enabled(false);
    covered.apply(spin, 0.0);
    spin.mouse_x = 600.0f;
    covered.apply(spin, 0.0);
    CHECK_FALSE(covered.rotated());
}

TEST_CASE("The middle button drags the ground; let go, a rotation reverts (M217f)", "[camera]") {
    const map::Heightmap ground = flat();
    Camera cam = camera_over(ground);
    cam.set_zoom(64.0f);
    cam.set_target(128.0f, 128.0f);
    CameraInput in;
    in.mouse_x = 500.0f;
    in.mouse_y = 400.0f;
    cam.apply(in, 0.0);
    in.middle = true;
    cam.apply(in, 0.0); // pressed: no move yet
    CHECK_THAT(cam.target_x(), WithinAbs(128.0, 1e-4));
    in.mouse_x = 532.0f; // dragged right: the ground follows, the view goes west
    in.mouse_y = 416.0f; // and down: the view goes north
    cam.apply(in, 0.0);
    CHECK_THAT(cam.target_x(), WithinAbs(128.0 - 32.0 * 64.0 / kW, 1e-3));
    CHECK_THAT(cam.target_z(), WithinAbs(128.0 - 16.0 * 64.0 / kW, 1e-3));
    // A drag begun over a UI control isn't the world's
    Camera covered = camera_over(ground);
    covered.set_zoom(64.0f);
    covered.set_target(128.0f, 128.0f);
    covered.set_mouse_enabled(false);
    CameraInput press;
    press.middle = true;
    covered.apply(press, 0.0);
    press.mouse_x = 300.0f;
    covered.apply(press, 0.0);
    CHECK_THAT(covered.target_x(), WithinAbs(128.0, 1e-4));
    // Let go of a drag with the view turned: it turns back
    cam.spin(40.0f, 0.0f);
    in.middle = false;
    cam.apply(in, 1.0 / 60.0);
    for (int i = 0; i < 30; ++i) cam.apply(in, 1.0 / 60.0);
    CHECK_FALSE(cam.rotated());
}

TEST_CASE("After a pan onto higher ground, the focus is on it (M217f)", "[camera]") {
    // Low (0) west of x = 128, a plateau at 20 east of it
    std::vector<u16> raw(257 * 257, 0);
    for (u32 z = 0; z <= 256; ++z)
        for (u32 x = 129; x <= 256; ++x) raw[z * 257 + x] = static_cast<u16>(20.0f * 128.0f);
    const map::Heightmap ground(256, 256, 1.0f / 128.0f, std::move(raw));
    Camera cam = camera_over(ground);
    cam.set_zoom(64.0f);
    cam.set_target(96.0f, 128.0f);
    cam.frame(0.0);
    CHECK_THAT(cam.focus_y(), WithinAbs(0.0, 1e-3));
    // East onto the plateau: the view's line through the target (still at
    // the low ground's height) meets the plateau (ClampFocusPos)
    cam.pan(-(96.0f * kW / 64.0f), 0.0f);
    cam.frame(0.0);
    CHECK_THAT(cam.target_x(), WithinAbs(192.0, 1e-2));
    CHECK_THAT(cam.focus_y(), WithinAbs(20.0, 1e-3));
    CHECK(cam.focus_x() > 128.0f);
}

TEST_CASE("The options' variables set the camera's speeds and switches (M217i)", "[camera]") {
    const map::Heightmap ground = flat();
    Camera cam = camera_over(ground);
    // cam_ZoomAmount: a wheel notch's zoom (FA's default option: 0.4)
    const f32 start = cam.zoom();
    cam.set_zoom_amount(0.4f);
    cam.zoom(1.0f);
    CHECK_THAT(cam.requested_zoom(), WithinRel(start * std::exp2(-0.4f), 1e-5f));

    cam.set_zoom(64.0f);
    cam.set_target(128.0f, 128.0f);
    cam.apply({}, 0.0);
    const f32 px = 64.0f / kW;
    // ui_KeyboardPanSpeed and its Ctrl multiplier
    cam.set_keyboard_pan_speed(50.0f);
    cam.set_keyboard_pan_accelerate(3.0f);
    CameraInput in;
    in.left = true;
    cam.apply(in, 0.0);
    CHECK_THAT(cam.target_x(), WithinAbs(128.0 - 50.0 * px, 1e-3));
    in.control = true;
    cam.apply(in, 0.0);
    CHECK_THAT(cam.target_x(), WithinAbs(128.0 - 200.0 * px, 1e-3));
    // ui_ArrowKeysScrollView off: the arrows don't pan; the edges still do
    cam.set_arrow_scroll(false);
    const f32 x = cam.target_x();
    cam.apply(in, 0.0);
    CHECK_THAT(cam.target_x(), WithinAbs(x, 1e-5));
    CameraInput edge;
    edge.at_right = true;
    cam.apply(edge, 0.0);
    CHECK_THAT(cam.target_x(), WithinAbs(x + 50.0 * px, 1e-3));
    // ui_ScreenEdgeScrollView off: nor the edges
    cam.set_edge_scroll(false);
    const f32 x2 = cam.target_x();
    cam.apply(edge, 0.0);
    CHECK_THAT(cam.target_x(), WithinAbs(x2, 1e-5));

    // ui_KeyboardRotateSpeed and its Ctrl multiplier
    Camera turn = camera_over(ground);
    turn.set_keyboard_rotate_speed(20.0f);
    turn.set_keyboard_rotate_accelerate(4.0f);
    CameraInput ins;
    ins.insert = true;
    ins.control = true;
    turn.apply(ins, 0.0);
    CHECK_THAT(turn.heading(), WithinAbs(Camera::kPi - 80.0f * 360.0f / kW * kDeg, 1e-4));
}
