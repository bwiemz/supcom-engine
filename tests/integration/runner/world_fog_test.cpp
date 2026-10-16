// --world-fog-test: the fog of war over the world, drawn as Moho's
// RenderFogOfWar draws it (faf-re render/VisionRenderer.cpp; retail's
// vision.fx CastVision and frame.fx Vision): each of the focus army's vision
// circles cast into the stencil against the frame's depth, then what none
// holds darkened to 67%, colour only, after the meshes and effects.
//
// On dry ground 60 from every unit, ARMY_1's scout stands alone with its
// vision; the scene is drawn with the fog off and on.

#include "integration_tests.hpp"
#include "intel_probe.hpp"
#include "render_probe.hpp"

#include "map/terrain.hpp"
#include "renderer/renderer.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <cmath>
#include <optional>
#include <string>

namespace osc::test {

void test_world_fog(TestContext& ctx) {
    spdlog::info("=== World fog test: the fog of war as Moho draws it ===");
    Tally t;
    const map::Terrain* terrain = ctx.sim.terrain();
    const auto spot = quiet_spot(ctx.sim);
    if (!terrain || !spot) {
        t.check(false, "a map and dry ground 60 from every unit");
        return;
    }
    const f32 x = spot->x;
    const f32 z = spot->z;
    // A land scout: its vision, a circle round it on the ground
    const u32 scout = spawn_unit(ctx, "__osc_wf_scout", "uel0101", "ARMY_1", {x, z});
    ctx.sim.tick();
    const auto* e = ctx.sim.entity_registry().find(scout);
    const auto* u = e && e->is_unit() ? static_cast<const sim::Unit*>(e) : nullptr;
    f32 vision = 0.0f;
    if (u) {
        const auto it = u->intel_states().find("Vision");
        if (it != u->intel_states().end()) vision = std::floor(it->second.radius);
    }
    if (vision <= 4.0f) {
        t.check(false, fmt::format("the scout has its vision ({})", vision));
        return;
    }

    OffscreenShots shots(ctx);
    if (!shots.ok()) {
        t.check(false, "renderer init (no Vulkan?)");
        return;
    }
    renderer::Renderer& r = shots.renderer();
    const u32 errors_before = renderer::Renderer::validation_error_count();
    r.set_player_army(0);
    (void)shots.shoot(*terrain, x, z, 150.0f);
    const auto scene = [&](bool fog) {
        r.set_fog_enabled(fog);
        shots.recapture();
        renderer::Renderer::SceneImage image;
        r.request_scene_capture(
            [&](renderer::Renderer::SceneImage captured) { image = std::move(captured); });
        shots.redraw();
        return image;
    };
    const renderer::Renderer::SceneImage clear = scene(false);
    const renderer::Renderer::SceneImage fogged = scene(true);
    if (clear.rgba.empty() || fogged.rgba.size() != clear.rgba.size()) {
        t.check(false, "both scenes are captured");
        return;
    }
    // The scene's pixel at world (wx, wz) on the ground, fog on over fog off,
    // per colour channel (nullopt off the screen or on black)
    const auto ratio = [&](f32 wx, f32 wz) -> std::optional<f32> {
        const auto s = screen_of(r, {wx, terrain->get_terrain_height(wx, wz), wz});
        if (!s) return std::nullopt;
        const auto px = static_cast<u32>((*s)[0]);
        const auto py = static_cast<u32>((*s)[1]);
        if (px >= clear.width || py >= clear.height) return std::nullopt;
        const size_t i = (static_cast<size_t>(py) * clear.width + px) * 4;
        f32 sum = 0.0f;
        f32 sum_fog = 0.0f;
        for (size_t c = 0; c < 3; ++c) {
            sum += clear.rgba[i + c];
            sum_fog += fogged.rgba[i + c];
        }
        if (sum <= 0.001f) return std::nullopt;
        return sum_fog / sum;
    };
    // In the scout's vision: the ground as it was; beyond: at 67%
    int inside = 0;
    int inside_ok = 0;
    int outside = 0;
    int outside_ok = 0;
    for (int k = 0; k < 8; ++k) {
        const f32 a = static_cast<f32>(k) * 0.785398f;
        if (const auto q =
                ratio(x + vision * 0.5f * std::cos(a), z + vision * 0.5f * std::sin(a))) {
            ++inside;
            inside_ok += std::abs(*q - 1.0f) < 0.01f ? 1 : 0;
        }
        if (const auto q =
                ratio(x + (vision + 8.0f) * std::cos(a), z + (vision + 8.0f) * std::sin(a))) {
            ++outside;
            outside_ok += std::abs(*q - 0.67f) < 0.01f ? 1 : 0;
        }
    }
    t.check(inside > 4 && inside_ok == inside,
            fmt::format("in the scout's vision ({} round it) the ground is as it was: {} of {}",
                        vision, inside_ok, inside));
    t.check(outside > 4 && outside_ok == outside,
            fmt::format("beyond it the ground is darkened to 67%: {} of {}", outside_ok, outside));

    // An observer sees all: no fog drawn
    r.set_player_army(-1);
    const renderer::Renderer::SceneImage observer = scene(true);
    bool same = observer.rgba.size() == clear.rgba.size();
    for (size_t i = 0; same && i < clear.rgba.size(); ++i)
        same = std::abs(observer.rgba[i] - clear.rgba[i]) < 1e-4f;
    t.check(same && !r.world_vision().drawn(), "an observer's world has no fog");
    r.set_player_army(0);
    r.set_fog_enabled(false);
    t.check(renderer::Renderer::validation_error_count() == errors_before,
            "no Vulkan validation errors drawing it");
    spdlog::info("World fog test: {} passed, {} failed", t.pass, t.fail);
}

} // namespace osc::test
