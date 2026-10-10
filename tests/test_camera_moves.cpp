#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "map/heightmap.hpp"
#include "renderer/camera.hpp"

#include <cmath>
#include <map>
#include <vector>

using namespace osc;
using namespace osc::renderer;
using Catch::Matchers::WithinAbs;

namespace {

constexpr f32 kW = 1024.0f;
constexpr f32 kH = 768.0f;
constexpr f32 kGround = 10.0f;
constexpr f32 kPi = Camera::kPi;

map::Heightmap flat() {
    return {256, 256, 1.0f / 128.0f,
            std::vector<u16>(257 * 257, static_cast<u16>(kGround * 128.0f))};
}

/// A camera over the flat map, at a zoom of 100 over (128, 128), its clock
/// at 0.
Camera camera_over(const map::Heightmap& ground) {
    Camera cam;
    cam.set_viewport(kW, kH);
    cam.set_ground(&ground, false, 0.0f);
    cam.init(256.0f, 256.0f);
    cam.set_zoom(100.0f);
    cam.set_target(128.0f, 128.0f);
    cam.set_clocks(0.0, 0.0);
    cam.frame(0.0);
    return cam;
}

/// 3t^2 - 2t^3: the eased cubic (the Hermite with no tangents)
f32 eased(f32 t) {
    return t * t * (3.0f - 2.0f * t);
}

} // namespace

TEST_CASE("A timed TargetManual eases there and signals (M217g)", "[camera][moves]") {
    const map::Heightmap ground = flat();
    Camera cam = camera_over(ground);
    const f32 zoom0 = cam.zoom();
    const f32 pitch0 = cam.pitch();
    int signals = 0;
    cam.target_manual(160.0f, kGround, 96.0f, kPi - 0.4f, 1.0f, 40.0f, 2.0f);
    cam.on_signal([&] { ++signals; });
    CHECK(cam.moving());
    CHECK_FALSE(cam.signaled());
    CHECK(cam.target_type() == CameraTarget::Hermite);
    // A quarter of the way (0.5 of 2 seconds): the eased cubic of 0.25
    cam.set_clocks(0.5, 0.0);
    cam.frame(1.0 / 60.0);
    const f32 k = eased(0.25f);
    CHECK_THAT(cam.zoom(), WithinAbs(zoom0 + (40.0f - zoom0) * k, 1e-3));
    CHECK_THAT(cam.pitch(), WithinAbs(pitch0 + (1.0f - pitch0) * k, 1e-4));
    CHECK_THAT(cam.heading(), WithinAbs(kPi - 0.4f * k, 1e-4));
    CHECK_THAT(cam.focus_x(), WithinAbs(128.0f + 32.0f * k, 1e-2));
    CHECK_THAT(cam.focus_z(), WithinAbs(128.0f - 32.0f * k, 1e-2));
    CHECK(signals == 0);
    // Half way: 0.5
    cam.set_clocks(1.0, 0.0);
    cam.frame(1.0 / 60.0);
    CHECK_THAT(cam.zoom(), WithinAbs(zoom0 + (40.0f - zoom0) * 0.5f, 1e-3));
    // There: held at the end, signalled once, a place again
    cam.set_clocks(2.0, 0.0);
    cam.frame(1.0 / 60.0);
    CHECK_FALSE(cam.moving());
    CHECK(cam.signaled());
    CHECK(signals == 1);
    CHECK(cam.rotated());
    CHECK(cam.target_type() == CameraTarget::Location);
    CHECK_THAT(cam.zoom(), WithinAbs(40.0, 1e-4));
    CHECK_THAT(cam.pitch(), WithinAbs(1.0, 1e-5));
    CHECK_THAT(cam.heading(), WithinAbs(kPi - 0.4f, 1e-5));
    CHECK_THAT(cam.focus_x(), WithinAbs(160.0, 1e-2));
    // And stays there
    cam.set_clocks(3.0, 0.0);
    cam.frame(1.0 / 60.0);
    CHECK_THAT(cam.pitch(), WithinAbs(1.0, 1e-5));
    CHECK(signals == 1);
}

TEST_CASE("Chained moves across +-pi turn the short way (M217g)", "[camera][moves]") {
    const map::Heightmap ground = flat();
    Camera cam = camera_over(ground);
    // From pi to -pi + 0.2: unwrapped past pi, it ends at pi + 0.2
    cam.target_manual(128.0f, kGround, 128.0f, -kPi + 0.2f, 1.0f, 60.0f, 1.0f);
    cam.set_clocks(1.0, 0.0);
    cam.frame(1.0 / 60.0);
    REQUIRE_THAT(cam.heading(), WithinAbs(kPi + 0.2f, 1e-4));
    // Then on to pi - 0.2: 0.4 back, not the long way round
    cam.target_manual(128.0f, kGround, 128.0f, kPi - 0.2f, 1.0f, 60.0f, 1.0f);
    for (int i = 1; i <= 10; ++i) {
        cam.set_clocks(1.0 + 0.1 * i, 0.0);
        cam.frame(1.0 / 60.0);
        const f32 wrapped = std::remainder(cam.heading() - kPi, 2.0f * kPi);
        CHECK(std::abs(wrapped) <= 0.2f + 1e-4f);
    }
    CHECK_THAT(std::remainder(cam.heading() - (kPi - 0.2f), 2.0f * kPi), WithinAbs(0.0, 1e-4));
}

TEST_CASE("Without ease-in-out a move is a straight line (M217g)", "[camera][moves]") {
    const map::Heightmap ground = flat();
    Camera cam = camera_over(ground);
    const f32 zoom0 = cam.zoom();
    cam.set_ease_in_out(false);
    cam.target_manual(128.0f, kGround, 128.0f, kPi, 1.0f, 40.0f, 1.0f);
    cam.set_clocks(0.25, 0.0);
    cam.frame(0.0);
    CHECK_THAT(cam.zoom(), WithinAbs(zoom0 + (40.0f - zoom0) * 0.25f, 1e-3));
    cam.set_clocks(1.0, 0.0);
    cam.frame(0.0); // there (a reset would leave a move running, as Moho's)
    // A reset turns it back on
    cam.reset();
    cam.set_zoom(100.0f);
    cam.frame(0.0);
    cam.set_clocks(1.0, 0.0);
    cam.target_manual(128.0f, kGround, 128.0f, kPi, 1.0f, 40.0f, 1.0f);
    cam.set_clocks(1.25, 0.0);
    cam.frame(0.0);
    // (the tangents it last had stay: Moho's)
    const f32 t = 0.25f;
    const f32 h00 = 2 * t * t * t - 3 * t * t + 1;
    const f32 h10 = t * t * t - 2 * t * t + t;
    const f32 h01 = t * t * t - t * t;
    const f32 h11 = -2 * t * t * t + 3 * t * t;
    CHECK_THAT(cam.zoom(),
               WithinAbs(h00 * 100.0f + (h10 + h01) * (40.0f - zoom0) + h11 * 40.0f, 1e-2));
}

TEST_CASE("SetAccMode shapes a move's progress (M217g)", "[camera][moves]") {
    const map::Heightmap ground = flat();
    CHECK_FALSE(Camera().set_acc_mode("Bouncy"));
    for (const auto& [name, shaped] : std::vector<std::pair<const char*, f32>>{
             {"LINEAR", 0.25f},
             {"fastinslowout", std::sin(0.25f * kPi * 0.5f)},
             {"SlowInOut", (1.0f - std::cos(0.25f * kPi)) * 0.5f}}) {
        Camera cam = camera_over(ground);
        const f32 zoom0 = cam.zoom();
        REQUIRE(cam.set_acc_mode(name));
        cam.set_ease_in_out(false);
        cam.target_manual(128.0f, kGround, 128.0f, kPi, 1.0f, 40.0f, 1.0f);
        cam.set_clocks(0.25, 0.0);
        cam.frame(0.0);
        CHECK_THAT(cam.zoom(), WithinAbs(zoom0 + (40.0f - zoom0) * shaped, 1e-3));
    }
    // SlowInOut's second half
    Camera cam = camera_over(ground);
    const f32 zoom0 = cam.zoom();
    REQUIRE(cam.set_acc_mode("SlowInOut"));
    cam.set_ease_in_out(false);
    cam.target_manual(128.0f, kGround, 128.0f, kPi, 1.0f, 40.0f, 1.0f);
    cam.set_clocks(0.75, 0.0);
    cam.frame(0.0);
    CHECK_THAT(cam.zoom(),
               WithinAbs(zoom0 + (40.0f - zoom0) * (std::sin(0.25f * kPi) * 0.5f + 0.5f), 1e-3));
}

TEST_CASE("On the game clock a move runs on game time (M217g)", "[camera][moves]") {
    const map::Heightmap ground = flat();
    Camera cam = camera_over(ground);
    cam.use_game_clock(true);
    cam.set_clocks(5.0, 2.0);
    cam.frame(0.0);
    const f32 zoom0 = cam.zoom();
    cam.set_ease_in_out(false);
    cam.target_manual(128.0f, kGround, 128.0f, kPi, 1.0f, 40.0f, 1.0f);
    // The system clock runs on; the game's stands still (a pause)
    cam.set_clocks(9.0, 2.0);
    cam.frame(1.0 / 60.0);
    CHECK_THAT(cam.zoom(), WithinAbs(zoom0, 1e-4));
    cam.set_clocks(9.0, 2.5);
    cam.frame(1.0 / 60.0);
    CHECK_THAT(cam.zoom(), WithinAbs(zoom0 + (40.0f - zoom0) * 0.5f, 1e-3));
    // A spin's rates run on it too: a quarter turn a second, half a game
    // second in a sixtieth of the system's
    cam.set_clocks(9.0, 3.5);
    cam.frame(1.0 / 60.0);
    const f32 heading0 = cam.heading();
    cam.spin_rates(0.25f, 0.0f);
    cam.set_clocks(9.0 + 1.0 / 60.0, 4.0);
    cam.frame(1.0 / 60.0);
    CHECK_THAT(cam.heading() - heading0, WithinAbs(0.25 * 0.5 * 2.0 * kPi, 1e-4));
}

TEST_CASE("TargetBox frames a box's centre at its wider side (M217g)", "[camera][moves]") {
    const map::Heightmap ground = flat();
    Camera cam = camera_over(ground);
    cam.target_box({60.0f, kGround, 100.0f}, {140.0f, kGround, 130.0f}, 0.0f);
    CHECK(cam.target_type() == CameraTarget::Box);
    CHECK_FALSE(cam.moving());
    CHECK_THAT(cam.zoom(), WithinAbs(80.0, 1e-4));
    CHECK_THAT(cam.target_x(), WithinAbs(100.0, 1e-3));
    CHECK_THAT(cam.target_z(), WithinAbs(115.0, 1e-3));
    // Over seconds, a box move that ends a place, its pitch the zoom's all
    // the way (as a still camera's at that zoom)
    cam.target_box({100.0f, kGround, 100.0f}, {130.0f, kGround, 110.0f}, 1.0f);
    CHECK(cam.moving());
    cam.set_clocks(0.5, 0.0);
    cam.frame(0.0);
    Camera still = camera_over(ground);
    still.set_zoom(cam.zoom());
    CHECK(cam.zoom() < 79.0f);
    CHECK_THAT(cam.pitch(), WithinAbs(still.pitch(), 1e-5));
    cam.set_clocks(1.0, 0.0);
    cam.frame(0.0);
    CHECK_FALSE(cam.moving());
    CHECK(cam.target_type() == CameraTarget::Location);
    CHECK_THAT(cam.zoom(), WithinAbs(30.0, 1e-4));
    CHECK_FALSE(cam.rotated());
}

TEST_CASE("A tracked entity is followed, and let go when gone (M217g)", "[camera][moves]") {
    const map::Heightmap ground = flat();
    Camera cam = camera_over(ground);
    std::map<u32, CameraEntityPose> world;
    world[7].pos = {90.0f, kGround, 90.0f};
    world[8].pos = {150.0f, kGround, 150.0f};
    cam.set_entity_lookup([&](u32 id, CameraEntityPose& out) {
        const auto it = world.find(id);
        if (it == world.end()) return false;
        out = it->second;
        return true;
    });
    cam.target_entities({7, 8}, true, 50.0f, 0.0f);
    CHECK(cam.target_type() == CameraTarget::Entity);
    CHECK(cam.target_entity() == 7); // GetTargetEntity (UI_TrackUnit asks it)
    CHECK_THAT(cam.requested_zoom(), WithinAbs(50.0, 1e-5));
    cam.frame(1.0 / 60.0);
    CHECK_THAT(cam.target_x(), WithinAbs(90.0, 1e-3));
    world[7].pos = {100.0f, kGround, 95.0f};
    cam.frame(1.0 / 60.0);
    CHECK_THAT(cam.target_x(), WithinAbs(100.0, 1e-3));
    CHECK_THAT(cam.target_z(), WithinAbs(95.0, 1e-3));
    // Gone: a place; next frame, the next in the list
    world.erase(7);
    cam.frame(1.0 / 60.0);
    CHECK(cam.target_type() == CameraTarget::Location);
    cam.frame(1.0 / 60.0);
    CHECK(cam.target_type() == CameraTarget::Entity);
    cam.frame(1.0 / 60.0);
    CHECK_THAT(cam.target_x(), WithinAbs(150.0, 1e-3));
    CHECK(cam.target_entity() == 8);
    // With the next gone too, on to the one after it, not past it
    world[7].pos = {90.0f, kGround, 90.0f};
    world[9].pos = {170.0f, kGround, 60.0f};
    world[10].pos = {60.0f, kGround, 170.0f};
    cam.target_entities({7, 12, 9, 10}, true, 50.0f, 0.0f); // 12: never there
    world.erase(7);
    for (int i = 0; i < 3; ++i) cam.frame(1.0 / 60.0);
    CHECK(cam.target_type() == CameraTarget::Entity);
    CHECK_THAT(cam.target_x(), WithinAbs(170.0, 1e-3));
    // Untracked, it goes there once
    Camera once = camera_over(ground);
    once.set_entity_lookup([&](u32 id, CameraEntityPose& out) {
        const auto it = world.find(id);
        if (it == world.end()) return false;
        out = it->second;
        return true;
    });
    once.target_entities({8}, false, 50.0f, 0.0f);
    CHECK(once.target_type() == CameraTarget::Location);
    CHECK_THAT(once.target_x(), WithinAbs(150.0, 1e-3));
    CHECK(once.target_entity() == 0); // followed by none
    cam.target_nothing();
    CHECK(cam.target_entity() == 0);
}

TEST_CASE("The nose camera looks along its entity (M217g)", "[camera][moves]") {
    const map::Heightmap ground = flat();
    Camera cam = camera_over(ground);
    CameraEntityPose plane;
    plane.pos = {120.0f, 40.0f, 110.0f};
    const f32 yaw = 0.7f; // about y
    plane.orient = {0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f)};
    cam.set_entity_lookup([&](u32 id, CameraEntityPose& out) {
        if (id != 3) return false;
        out = plane;
        return true;
    });
    cam.target_nose_cam({3}, 0.3f, 25.0f, 0.0f, 0.0f);
    CHECK(cam.target_type() == CameraTarget::NoseCam);
    CHECK_THAT(cam.heading(), WithinAbs(yaw, 1e-4));
    CHECK_THAT(cam.pitch(), WithinAbs(0.3, 1e-3)); // its level pitch is 0, plus 0.3
    CHECK_THAT(cam.zoom(), WithinAbs(25.0, 1e-4));
    // It turns with the entity
    const f32 turned = -1.2f;
    plane.orient = {0.0f, std::sin(turned * 0.5f), 0.0f, std::cos(turned * 0.5f)};
    cam.frame(1.0 / 60.0);
    CHECK_THAT(cam.heading(), WithinAbs(turned, 1e-4));
}

TEST_CASE("Spin turns and zooms at its rates (M217g)", "[camera][moves]") {
    const map::Heightmap ground = flat();
    Camera cam = camera_over(ground);
    const f32 zoom0 = cam.requested_zoom();
    cam.spin_rates(0.25f, 10.0f);
    CHECK(cam.rotated());
    CHECK(cam.target_type() == CameraTarget::Hermite);
    cam.frame(1.0);
    // A quarter turn a second from pi, and 10 more zoom asked a second
    CHECK_THAT(cam.heading(), WithinAbs(kPi + kPi * 0.5f, 1e-4));
    CHECK_THAT(cam.requested_zoom(), WithinAbs(zoom0 + 10.0f, 1e-3));
}

TEST_CASE("RevertRotation makes the target a place, unless it follows one (M217g)",
          "[camera][moves]") {
    const map::Heightmap ground = flat();
    Camera cam = camera_over(ground);
    cam.target_box({100.0f, kGround, 100.0f}, {140.0f, kGround, 140.0f}, 0.0f);
    cam.spin(10.0f, 0.0f);
    cam.revert_rotation();
    CHECK(cam.target_type() == CameraTarget::Location);
    // Following an entity, it keeps following
    CameraEntityPose pose;
    pose.pos = {120.0f, kGround, 120.0f};
    cam.set_entity_lookup([&](u32, CameraEntityPose& out) {
        out = pose;
        return true;
    });
    cam.target_entities({3}, true, 60.0f, 0.0f);
    cam.spin(10.0f, 0.0f);
    REQUIRE(cam.rotated());
    cam.revert_rotation();
    CHECK(cam.target_type() == CameraTarget::Entity);
}

TEST_CASE("A camera following a launcher goes on to its shot, and back after the timeout",
          "[camera][moves]") {
    // Moho's CameraImpl::CameraFollow, then UpdateTargets' countdown
    const map::Heightmap ground = flat();
    Camera cam = camera_over(ground);
    std::map<u32, CameraEntityPose> world;
    world[7].pos = {90.0f, kGround, 90.0f};
    world[8].pos = {150.0f, kGround, 150.0f};
    world[20].pos = {100.0f, kGround, 100.0f};
    cam.set_entity_lookup([&](u32 id, CameraEntityPose& out) {
        const auto it = world.find(id);
        if (it == world.end()) return false;
        out = it->second;
        return true;
    });
    cam.target_entities({7}, true, 50.0f, 0.0f);
    cam.camera_follow(8, 20, 2.0f);
    CHECK(cam.target_entity() == 7);

    cam.camera_follow(7, 20, 2.0f);
    CHECK(cam.target_entity() == 20);
    world[20].pos = {120.0f, kGround, 110.0f};
    cam.frame(0.5);
    CHECK_THAT(cam.target_x(), WithinAbs(120.0, 1e-3));

    world.erase(20);
    cam.frame(0.5);
    CHECK(cam.target_type() == CameraTarget::Location);
    cam.frame(1.0);
    CHECK(cam.target_type() == CameraTarget::Location);
    cam.frame(1.0);
    CHECK(cam.target_entity() == 7);
    cam.frame(0.5);
    CHECK_THAT(cam.target_x(), WithinAbs(90.0, 1e-3));
}
