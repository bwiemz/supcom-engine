// --skirt-render-test: terrain.fx's TTerrainSkirt.

#include "integration_tests.hpp"
#include "intel_probe.hpp"
#include "map/heightmap.hpp"
#include "map/terrain.hpp"
#include "plate_fixtures.hpp"
#include "render_probe.hpp"
#include "renderer/camera.hpp"
#include "renderer/renderer.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <vector>

namespace osc::test {

namespace {

constexpr u32 kSize = 64;
constexpr f32 kEdgeHeight = 20.0f;
constexpr f32 kPitch = 0.15f;
constexpr f32 kDistance = 100.0f;

std::array<f32, 4> pixel_at(renderer::Renderer& r, const renderer::Renderer::SceneImage& s,
                            const sim::Vector3& p) {
    const auto at = screen_of(r, p);
    if (!at || s.width == 0) {
        return {-1.0f, -1.0f, -1.0f, -1.0f};
    }
    const u32 x = std::min(static_cast<u32>((*at)[0]), s.width - 1);
    const u32 y = std::min(static_cast<u32>((*at)[1]), s.height - 1);
    const f32* q = &s.rgba[(static_cast<size_t>(y) * s.width + x) * 4];
    return {q[0], q[1], q[2], q[3]};
}

bool near(const std::array<f32, 4>& c, f32 rgb, f32 a) {
    return std::abs(c[0] - rgb) < 2e-3f && std::abs(c[1] - rgb) < 2e-3f &&
           std::abs(c[2] - rgb) < 2e-3f && std::abs(c[3] - a) < 2e-3f;
}

} // namespace

void test_skirt_render(TestContext& ctx) {
    spdlog::info("=== Skirt render test ===");
    Tally t;
    OffscreenShots shots(ctx);
    if (!shots.ok()) {
        t.check(false, "renderer init (no Vulkan?)");
        return;
    }
    renderer::Renderer& r = shots.renderer();
    r.set_clear_color(renderer::Renderer::kClearColor);
    r.video_options().skydome = false;

    std::vector<u16> heights(static_cast<size_t>(kSize + 1) * (kSize + 1),
                             static_cast<u16>(kEdgeHeight * 128.0f));
    heights[static_cast<size_t>(4) * (kSize + 1) + 4] = 0;
    map::Terrain ground(map::Heightmap(kSize, kSize, 1.0f / 128.0f, std::move(heights)), 0.0f,
                        false);
    ground.set_lighting(white_fill(), map::ScmapEnvironment{});
    std::vector<map::StratumInfo> strata(10);
    for (auto& st : strata) {
        st.albedo_scale = st.normal_scale = 4.0f;
    }
    strata[0].albedo_path = "/env/evergreen2/layers/eg_gravel005_albedo.dds";
    ground.set_strata(std::move(strata), {}, {});

    const sim::Vector3 wall{kSize * 0.5f, kEdgeHeight * 0.5f, static_cast<f32>(kSize)};
    const sim::Vector3 under{kSize * 0.5f, -10.0f, static_cast<f32>(kSize)};
    const auto shoot = [&](bool skirt) {
        r.video_options().skirt = skirt;
        r.camera().set_heading(std::numbers::pi_v<f32>);
        r.camera().set_pitch(kPitch);
        (void)shots.shoot(ground, kSize * 0.5f, static_cast<f32>(kSize), kDistance, false);
        renderer::Renderer::SceneImage image;
        r.request_scene_capture([&](renderer::Renderer::SceneImage s) { image = std::move(s); });
        (void)shots.grab();
        return std::array<std::array<f32, 4>, 2>{pixel_at(r, image, wall),
                                                 pixel_at(r, image, under)};
    };

    f32 ex = 0.0f;
    f32 ey = 0.0f;
    f32 ez = 0.0f;
    const auto on = shoot(true);
    r.camera().eye_position(ex, ey, ez);
    const auto off = shoot(false);

    t.check(ez > static_cast<f32>(kSize) && near(on[0], 0.1f, 0.0f) && near(on[1], 0.0f, 0.0f),
            fmt::format("Test 1: from outside the map (eye z {:.0f}), the wall under its edge is "
                        "({:.3f}, {:.3f}, {:.3f}, {:.3f}), and under the wall "
                        "({:.3f}, {:.3f}, {:.3f}, {:.3f})",
                        ez, on[0][0], on[0][1], on[0][2], on[0][3], on[1][0], on[1][1], on[1][2],
                        on[1][3]));
    t.check(near(off[0], 0.0f, 0.0f),
            fmt::format("Test 2: with ren_Skirt off, the clear: ({:.3f}, {:.3f}, {:.3f}, {:.3f})",
                        off[0][0], off[0][1], off[0][2], off[0][3]));

    spdlog::info("Skirt render test: {} passed, {} failed", t.pass, t.fail);
}

} // namespace osc::test
