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
    t.check(std::abs(cam.target_y() - ground) < 0.01f,
            fmt::format("Test 1: the camera's focus is at height {:.2f}; the ground there is "
                        "{:.2f}",
                        cam.target_y(), ground));

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
            const f32 polled = shots.renderer().camera().target_y();
            (void)shots.shoot(*terrain, wx, wz, 60.0f);
            const f32 y = shots.renderer().camera().target_y();
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

    spdlog::info("Camera test: {}/{} passed", t.pass, t.pass + t.fail);
}

} // namespace osc::test
