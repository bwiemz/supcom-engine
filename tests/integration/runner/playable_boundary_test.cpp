// --playable-boundary-test: the playable area's boundary (Moho's
// RenderPlayableBoundary; vision.fx's CastBoundaryCW/CCW and frame.fx's
// Boundary, ren_PlayableBoundary on): with a playable rect smaller than the
// map (a campaign's SetPlayableArea), the world outside it is black, and
// inside as it was; a rect the map's size, or the convar off, draws none.
// A cartographic main view's frame is blacked out the same way.

#include "integration_tests.hpp"
#include "intel_probe.hpp"
#include "render_probe.hpp"

#include "map/terrain.hpp"
#include "renderer/playable_rect.hpp"
#include "renderer/renderer.hpp"
#include "sim/sim_state.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

namespace osc::test {

void test_playable_boundary(TestContext& ctx) {
    spdlog::info("=== Playable boundary test: outside the playable rect, black ===");
    Tally t;
    const map::Terrain* terrain = ctx.sim.terrain();
    if (!terrain) {
        t.check(false, "a map is loaded");
        return;
    }
    OffscreenShots shots(ctx);
    if (!shots.ok()) {
        t.check(false, "renderer init (no Vulkan?)");
        return;
    }
    renderer::Renderer& r = shots.renderer();
    const u32 errors_before = renderer::Renderer::validation_error_count();
    const auto w = static_cast<f32>(terrain->map_width());
    const auto h = static_cast<f32>(terrain->map_height());
    const f32 cx = w * 0.5f;
    const f32 cz = h * 0.5f;
    (void)shots.shoot(*terrain, cx, cz, 0.6f * w);
    const auto scene = [&]() {
        renderer::Renderer::SceneImage image;
        r.request_scene_capture(
            [&](renderer::Renderer::SceneImage captured) { image = std::move(captured); });
        shots.redraw();
        return image;
    };
    // The scene's colour at a point on the ground (nullopt off the screen)
    const auto colour_at = [&](const renderer::Renderer::SceneImage& img, f32 x,
                               f32 z) -> std::optional<std::array<f32, 3>> {
        const auto s = screen_of(r, {x, terrain->get_terrain_height(x, z), z});
        if (!s || (*s)[0] < 1.0f || (*s)[1] < 1.0f) return std::nullopt;
        const auto px = static_cast<u32>((*s)[0]);
        const auto py = static_cast<u32>((*s)[1]);
        if (px + 1 >= img.width || py + 1 >= img.height) return std::nullopt;
        const size_t i = (static_cast<size_t>(py) * img.width + px) * 4;
        return std::array<f32, 3>{img.rgba[i], img.rgba[i + 1], img.rgba[i + 2]};
    };
    const renderer::Renderer::SceneImage before = scene();
    t.check(!r.world_boundary().drawn(), "no playable rect: no boundary");

    // A quarter of the map in its middle, as SetPlayableArea leaves it
    const renderer::PlayableRect inner{static_cast<i32>(w * 0.25f), static_cast<i32>(h * 0.25f),
                                       static_cast<i32>(w * 0.75f), static_cast<i32>(h * 0.75f)};
    r.playable_rect().sync(inner);
    const renderer::Renderer::SceneImage after = scene();
    t.check(r.world_boundary().drawn(), "a smaller playable rect: the boundary is drawn");
    // Points on a ring round the middle: inside the rect, and outside it on
    // the map
    int inside = 0;
    int inside_kept = 0;
    int outside = 0;
    int outside_black = 0;
    for (int k = 0; k < 16; ++k) {
        const f32 a = static_cast<f32>(k) * 0.392699f;
        const f32 ix = cx + 0.18f * w * std::cos(a);
        const f32 iz = cz + 0.18f * h * std::sin(a);
        const auto was = colour_at(before, ix, iz);
        const auto now = colour_at(after, ix, iz);
        if (was && now) {
            ++inside;
            bool same = true;
            for (size_t c = 0; c < 3; ++c) same = same && std::abs((*was)[c] - (*now)[c]) < 1e-3f;
            inside_kept += same ? 1 : 0;
        }
        const f32 ox = cx + 0.4f * w * std::cos(a);
        const f32 oz = cz + 0.4f * h * std::sin(a);
        const auto lit = colour_at(before, ox, oz);
        const auto dark = colour_at(after, ox, oz);
        if (lit && dark && (*lit)[0] + (*lit)[1] + (*lit)[2] > 0.05f) {
            ++outside;
            outside_black += (*dark)[0] + (*dark)[1] + (*dark)[2] < 1e-3f ? 1 : 0;
        }
    }
    t.check(
        inside >= 8 && inside_kept == inside,
        fmt::format("inside the rect, the world as it was: {} of {} points", inside_kept, inside));
    t.check(outside >= 4 && outside_black == outside,
            fmt::format("outside it, black: {} of {} points", outside_black, outside));

    // A rect the map's size, or the convar off: none
    r.playable_rect().sync({0, 0, static_cast<i32>(w), static_cast<i32>(h)});
    const renderer::Renderer::SceneImage whole = scene();
    size_t changed = 0;
    for (size_t i = 0; whole.rgba.size() == before.rgba.size() && i < before.rgba.size(); ++i)
        changed += std::abs(whole.rgba[i] - before.rgba[i]) < 1e-4f ? 0 : 1;
    // (but for what moves with the frames: the trees' sway)
    t.check(!r.world_boundary().drawn() && whole.rgba.size() == before.rgba.size() &&
                changed * 10000 < before.rgba.size(),
            fmt::format("a playable rect the map's size: no boundary ({}), the world as it was "
                        "({} channels changed)",
                        r.world_boundary().drawn(), changed));
    r.playable_rect().sync(inner);
    r.set_playable_boundary(false);
    (void)scene();
    t.check(!r.world_boundary().drawn(), "ren_PlayableBoundary off: none");
    r.set_playable_boundary(true);

    // A cartographic main view's frame takes it too (Cartographic::Render's
    // RenderPlayableBoundary)
    r.camera().set_ortho(true);
    const renderer::Renderer::SceneImage map = scene();
    int map_outside = 0;
    int map_black = 0;
    int map_inside = 0;
    int map_lit = 0;
    // (the view narrower now: outside points nearer the rect's edges)
    constexpr std::array<std::array<f32, 2>, 8> kOutside = {{{0.3f, 0.0f},
                                                             {-0.3f, 0.0f},
                                                             {0.0f, 0.3f},
                                                             {0.0f, -0.3f},
                                                             {0.3f, 0.3f},
                                                             {-0.3f, 0.3f},
                                                             {0.3f, -0.3f},
                                                             {-0.3f, -0.3f}}};
    for (const auto& [dx, dz] : kOutside) {
        if (const auto c = colour_at(map, cx + dx * w, cz + dz * h)) {
            ++map_outside;
            map_black += (*c)[0] + (*c)[1] + (*c)[2] < 1e-3f ? 1 : 0;
        }
    }
    for (int k = 0; k < 16; ++k) {
        const f32 a = static_cast<f32>(k) * 0.392699f;
        if (const auto c =
                colour_at(map, cx + 0.18f * w * std::cos(a), cz + 0.18f * h * std::sin(a))) {
            ++map_inside;
            map_lit += (*c)[0] + (*c)[1] + (*c)[2] > 0.05f ? 1 : 0;
        }
    }
    t.check(r.cartographic_main() && map_outside >= 2 && map_black == map_outside &&
                map_inside >= 8 && map_lit == map_inside,
            fmt::format("cartographic, outside the rect black ({} of {}), inside the map's "
                        "colours ({} of {})",
                        map_black, map_outside, map_lit, map_inside));
    r.camera().set_ortho(false);
    r.playable_rect().sync({0, 0, static_cast<i32>(w), static_cast<i32>(h)});

    t.check(renderer::Renderer::validation_error_count() == errors_before,
            "no Vulkan validation errors drawing it");
    spdlog::info("Playable boundary test: {} passed, {} failed", t.pass, t.fail);
}

} // namespace osc::test
