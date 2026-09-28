// --camera-test (M217a): the camera focuses on the ground, and clicks pick it.
//
// The camera had orbited a point at height 0 and clicks met the plane
// y = 0. SCMP_009's ground at the first army's start is 18.7 up, so from the
// nearest zoom the eye sat 4 over the grass with the start out of view, and
// a click landed some 15 short of the cursor.

#include "integration_tests.hpp"
#include "render_probe.hpp"

#include "map/terrain.hpp"
#include "renderer/input_handler.hpp"
#include "sim/entity.hpp"
#include "sim/sim_state.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <sstream>
#include <string>

namespace osc::test {

void test_camera(TestContext& ctx) {
    spdlog::info("=== CAMERA TEST: the camera's focus and clicks on the ground (M217a) ===");
    Tally t;
    auto* acu = ctx.sim.entity_registry().find(army_acu_id(ctx.sim, 0));
    const map::Terrain* terrain = ctx.sim.terrain();
    if (!acu || !terrain) {
        t.check(false, "ARMY_1's ACU and the terrain");
        return;
    }
    const f32 ax = acu->position().x;
    const f32 az = acu->position().z;
    f32 ground = terrain->get_terrain_height(ax, az);
    if (terrain->has_water()) ground = std::max(ground, terrain->water_elevation());

    OffscreenShots shots(ctx);
    if (!shots.ok()) {
        t.check(false, "renderer init (no Vulkan?)");
        return;
    }
    constexpr f32 kNearest = 30.0f;
    (void)shots.shoot(*terrain, ax, az, kNearest);
    renderer::Renderer& r = shots.renderer();
    const renderer::Camera& cam = r.camera();

    // Test 1: the focus sits on the ground at the target.
    t.check(std::abs(cam.focus_y() - ground) < 0.01f,
            fmt::format("Test 1: the camera's focus is at height {:.2f}; the ground there is "
                        "{:.2f}",
                        cam.focus_y(), ground));

    // Test 2: the pixel where a point of the ground is drawn picks it back,
    // off the screen's middle.
    {
        const f32 px_x = ax + 5.0f;
        const f32 px_z = az - 4.0f;
        const f32 py = terrain->get_terrain_height(px_x, px_z);
        const f32 w = static_cast<f32>(r.width());
        const f32 h = static_cast<f32>(r.height());
        const auto m = cam.view_proj(w / h);
        const f32 cx = m[0] * px_x + m[4] * py + m[8] * px_z + m[12];
        const f32 cy = m[1] * px_x + m[5] * py + m[9] * px_z + m[13];
        const f32 cw = m[3] * px_x + m[7] * py + m[11] * px_z + m[15];
        const f32 sx = (cx / cw * 0.5f + 0.5f) * w;
        const f32 sy = (cy / cw * 0.5f + 0.5f) * h;
        // As a click there resolves.
        f32 x = 0, z = 0;
        const bool hit = renderer::InputHandler::world_at(r, ctx.sim, sx, sy, x, z);
        const f32 off = std::hypot(x - px_x, z - px_z);
        t.check(hit && off < 0.5f,
                fmt::format("Test 2: a click on the pixel of ({:.1f}, {:.1f}) lands at ({:.1f}, "
                            "{:.1f}), {:.2f} off",
                            px_x, px_z, x, z, off));
    }

    // Test 3: from the nearest zoom, the start is in view: the frame draws
    // the meshes around the ACU (its mass deposits, the trees).
    {
        std::ostringstream out;
        r.dump_frame(out);
        std::istringstream in(out.str());
        size_t meshes = 0;
        bool in_units = false;
        for (std::string line; std::getline(in, line);) {
            if (!line.empty() && line[0] == '[') in_units = line == "[units]";
            else if (in_units && line.rfind("mesh ", 0) == 0) ++meshes;
        }
        t.check(meshes >= 3,
                fmt::format("Test 3: from {:.0f} away, {} meshes are drawn around the start",
                            kNearest, meshes));
    }

    // Test 4: over water, the focus sits on the water's surface.
    if (terrain->has_water()) {
        const f32 water = terrain->water_elevation();
        f32 wx = -1, wz = -1;
        const int cells_x = static_cast<int>(terrain->map_width() / 32) - 2;
        const int cells_z = static_cast<int>(terrain->map_height() / 32) - 2;
        for (int iz = 2; iz < cells_z && wx < 0; ++iz) {
            for (int ix = 2; ix < cells_x && wx < 0; ++ix) {
                const f32 x = static_cast<f32>(ix) * 32.0f;
                const f32 z = static_cast<f32>(iz) * 32.0f;
                if (terrain->get_terrain_height(x, z) < water - 5.0f) {
                    wx = x;
                    wz = z;
                }
            }
        }
        if (wx >= 0) {
            // Test 5 (first): moved there and polled, before any frame is
            // drawn -- input picks right after the poll.
            shots.renderer().camera().set_target(wx, wz);
            shots.renderer().poll_events(0.016);
            const f32 polled = shots.renderer().camera().focus_y();
            (void)shots.shoot(*terrain, wx, wz, 60.0f);
            const f32 y = shots.renderer().camera().focus_y();
            t.check(std::abs(y - water) < 0.01f,
                    fmt::format("Test 4: over the sea at ({:.0f}, {:.0f}), the focus is at "
                                "{:.2f}; the surface at {:.2f}",
                                wx, wz, y, water));
            t.check(std::abs(polled - water) < 0.01f,
                    fmt::format("Test 5: moved over the sea, the focus is at {:.2f} after the "
                                "poll, before a frame",
                                polled));
        } else {
            t.check(false, "Test 4: open water on the map");
        }
    }

    // Moho's camera (M217f), driven as the world view drives it: not free,
    // its turn not held, from a reset
    renderer::Camera& live = r.camera();
    const f32 sw = static_cast<f32>(r.width());
    const f32 sh = static_cast<f32>(r.height());
    // As the scene was built: the farthest zoom took the window's shape
    const f32 built_max_zoom = live.max_zoom();
    live.set_free(false);
    live.set_input_enabled(true);
    live.set_mouse_enabled(true);
    live.set_viewport(sw, sh);
    live.reset();
    constexpr f32 kDeg = renderer::Camera::kPi / 180.0f;
    const f32 mw = static_cast<f32>(terrain->map_width());
    const f32 mh = static_cast<f32>(terrain->map_height());
    const f32 max_zoom = std::max(mw * 1.4f, mh * 1.4f * sw / sh);
    const auto zoom_pitch = [&](f32 zoom) {
        const f32 f = (std::log(zoom) - std::log(5.0f)) / (std::log(max_zoom) - std::log(5.0f));
        return (40.0f + f * 49.9f) * kDeg;
    };

    // Test 6: the reset: the whole map from above, its middle on the ground
    {
        const f32 ground_mid = terrain->get_surface_height(mw * 0.5f, mh * 0.5f);
        const bool ok = std::abs(built_max_zoom - max_zoom) < 0.01f &&
                        std::abs(live.zoom() - max_zoom) < 0.01f &&
                        std::abs(live.pitch() - 89.9f * kDeg) < 1e-4f &&
                        std::abs(live.heading() - renderer::Camera::kPi) < 1e-5f &&
                        std::abs(live.target_x() - mw * 0.5f) < 0.01f &&
                        std::abs(live.focus_y() - ground_mid) < 0.05f;
        t.check(ok, fmt::format("Test 6: reset, the zoom is {:.1f} ({:.1f}, the map's across at "
                                "1.4 and the window's shape; {:.1f} as the scene was built), "
                                "the pitch {:.2f} degrees, the focus at {:.2f} ({:.2f})",
                                live.zoom(), max_zoom, built_max_zoom, live.pitch() / kDeg,
                                live.focus_y(), ground_mid));
    }

    // Test 7: forty notches of the wheel ask a quarter of the zoom; it glides
    // there, tilting the view down to the zoom's pitch as it comes
    {
        live.set_pivot(sw * 0.5f, sh * 0.5f);
        live.zoom(40.0f);
        const f32 want = max_zoom * 0.25f;
        f32 prev_zoom = live.zoom();
        f32 prev_pitch = live.pitch();
        bool gliding = true;
        int frames = 0;
        for (; frames < 600 && std::abs(live.zoom() - want) > 0.01f; ++frames) {
            live.frame(1.0 / 60.0);
            gliding = gliding && live.zoom() < prev_zoom && live.pitch() < prev_pitch;
            prev_zoom = live.zoom();
            prev_pitch = live.pitch();
        }
        t.check(gliding && frames > 5 && std::abs(live.zoom() - want) <= 0.01f &&
                    std::abs(live.pitch() - zoom_pitch(want)) < 1e-4f,
                fmt::format("Test 7: the zoom glides to {:.1f} in {} frames, the pitch down to "
                            "{:.2f} degrees ({:.2f})",
                            live.zoom(), frames, live.pitch() / kDeg, zoom_pitch(want) / kDeg));
    }

    // Test 8: a middle-drag carries the ground at the focus with the cursor:
    // 40 pixels right, it is drawn 40 pixels right
    {
        live.set_zoom(300.0f);
        live.set_target(mw * 0.5f, mh * 0.5f);
        live.frame(0.0);
        const f32 fx = live.focus_x();
        const f32 fy = live.focus_y();
        const f32 fz = live.focus_z();
        renderer::CameraInput in;
        in.mouse_x = sw * 0.5f;
        in.mouse_y = sh * 0.5f;
        live.apply(in, 0.0);
        in.middle = true;
        live.apply(in, 0.0);
        in.mouse_x += 40.0f;
        live.apply(in, 0.0);
        in.middle = false;
        live.apply(in, 0.0);
        const auto m = live.view_proj(sw / sh);
        const f32 cx = m[0] * fx + m[4] * fy + m[8] * fz + m[12];
        const f32 cy = m[1] * fx + m[5] * fy + m[9] * fz + m[13];
        const f32 cw = m[3] * fx + m[7] * fy + m[11] * fz + m[15];
        const f32 px = (cx / cw * 0.5f + 0.5f) * sw;
        const f32 py = (cy / cw * 0.5f + 0.5f) * sh;
        t.check(std::abs(px - (sw * 0.5f + 40.0f)) < 1.0f && std::abs(py - sh * 0.5f) < 1.0f,
                fmt::format("Test 8: dragged 40 pixels right, the ground at the focus is drawn "
                            "at ({:.1f}, {:.1f}), from ({:.0f}, {:.0f})",
                            px, py, sw * 0.5f, sh * 0.5f));
    }

    // Test 9: near the ground, Space and the mouse tilt the view up to the
    // horizon (the top of the frame looks above it); let go, it turns back
    {
        live.set_zoom(60.0f);
        live.frame(0.0);
        renderer::CameraInput in;
        in.space = true;
        in.mouse_x = sw * 0.5f;
        in.mouse_y = sh * 0.5f;
        live.apply(in, 0.0);
        in.mouse_y -= 2.0f * sw; // far up the screen: to the floor of 0.1
        live.apply(in, 0.0);
        f32 o[3];
        f32 d[3];
        const bool ray = live.screen_ray(sw * 0.5f, 0.0f, sw, sh, o, d);
        const bool tilted = live.rotated() && std::abs(live.pitch() - 0.1f) < 1e-5f && ray &&
                            d[1] > 0.0f;
        in.space = false;
        int frames = 0;
        for (; frames < 120 && live.rotated(); ++frames) live.apply(in, 1.0 / 60.0);
        t.check(tilted && !live.rotated() &&
                    std::abs(live.pitch() - zoom_pitch(live.zoom())) < 1e-4f &&
                    std::abs(live.heading() - renderer::Camera::kPi) < 1e-5f,
                fmt::format("Test 9: spun to a pitch of 0.1 (the frame's top looking {} the "
                            "horizon), let go it turned back in {} frames to {:.2f} degrees",
                            ray && d[1] > 0.0f ? "above" : "below", frames,
                            live.pitch() / kDeg));
    }

    spdlog::info("Camera test: {}/{} passed", t.pass, t.pass + t.fail);
}

} // namespace osc::test
