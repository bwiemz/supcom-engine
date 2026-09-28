#include "render_probe.hpp"

#include "core/test_status.hpp"
#include "integration_tests.hpp"
#include "map/terrain.hpp"
#include "sim/sim_state.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace osc::test {

void Tally::check(bool ok, const std::string& what) {
    if (ok) {
        ++pass;
        spdlog::info("[PASS] {}", what);
    } else {
        ++fail;
        osc::test_status::fail("[FAIL] {}", what);
    }
}

Pixels centre_pixels(const ImageRGBA8& image) {
    Pixels out;
    const u32 x0 = image.width * 35 / 100;
    const u32 x1 = image.width * 65 / 100;
    const u32 y0 = image.height * 35 / 100;
    const u32 y1 = image.height * 65 / 100;
    for (u32 y = y0; y < y1; ++y) {
        for (u32 x = x0; x < x1; ++x) {
            const size_t i = (static_cast<size_t>(y) * image.width + x) * 4;
            out.push_back({image.pixels[i] / 255.0f, image.pixels[i + 1] / 255.0f,
                           image.pixels[i + 2] / 255.0f});
        }
    }
    return out;
}

f32 median(std::vector<f32> values) {
    if (values.empty()) return 0.0f;
    const auto mid = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
    std::nth_element(values.begin(), mid, values.end());
    return *mid;
}

f32 median_ratio(const Pixels& above, const Pixels& below, int channel) {
    std::vector<f32> ratios;
    for (size_t i = 0; i < above.size() && i < below.size(); ++i) {
        if (below[i][channel] >= 0.06f) ratios.push_back(above[i][channel] / below[i][channel]);
    }
    return median(ratios);
}

f32 mean_abs_diff(const Pixels& a, const Pixels& b) {
    if (a.empty() || a.size() != b.size()) return 1.0f;
    f32 sum = 0.0f;
    for (size_t i = 0; i < a.size(); ++i) {
        for (int c = 0; c < 3; ++c) sum += std::abs(a[i][c] - b[i][c]);
    }
    return sum / static_cast<f32>(a.size() * 3);
}

OffscreenShots::OffscreenShots(TestContext& ctx) : ctx_(ctx) {
    ok_ = renderer_.init(800, 600, "Render Test", /*offscreen=*/true);
    if (!ok_) return;
    renderer_.set_bloom_enabled(false);
    renderer_.set_fog_enabled(false); // the fog of war: all visible
    renderer_.set_decals_enabled(false);
    renderer_.set_fixed_frame_dt(1.0f / 60.0f);
    // The tests' views (M217f): free (their targets may stand off their
    // ground) and held at 50 degrees, as the engine's camera sat before it
    // followed Moho's; a test may pitch it otherwise
    renderer_.camera().set_free(true);
    renderer_.camera().set_pitch(kPitch);
    // The backdrop the tests' scenery stands against, past its ground: the
    // blue-grey the engine cleared to until the sky dome (M210b), with no glow
    renderer_.set_clear_color(kBackdrop);
    history_.capture(ctx_.sim);
    history_.capture(ctx_.sim);
}

void OffscreenShots::recapture() {
    history_.capture(ctx_.sim);
    history_.capture(ctx_.sim);
}

void OffscreenShots::redraw(const std::unordered_set<u32>* selected) {
    renderer_.render(sim::FrameView(&history_.prev(), &history_.cur(), 1.0f), history_.events(),
                     nullptr, ctx_.L, nullptr, selected);
    renderer_.poll_events(0.016);
}

ImageRGBA8 OffscreenShots::grab(const std::unordered_set<u32>* selected) {
    ImageRGBA8 shot;
    renderer_.request_capture([&](ImageRGBA8 image) { shot = std::move(image); });
    redraw(selected);
    return shot;
}

OffscreenShots::~OffscreenShots() {
    if (ok_) renderer_.shutdown();
}

Pixels OffscreenShots::shoot(const map::Terrain& terrain, f32 x, f32 z, f32 distance,
                             bool with_world) {
    if (!ok_) return {};
    return centre_pixels(capture(terrain, x, z, distance, with_world));
}

ImageRGBA8 OffscreenShots::shoot_frame(const map::Terrain& terrain, f32 x, f32 z, f32 distance) {
    if (!ok_) return {};
    return capture(terrain, x, z, distance, true);
}

ImageRGBA8 OffscreenShots::capture(const map::Terrain& terrain, f32 x, f32 z, f32 distance,
                                   bool with_world) {
    sim::WorldHistory& world = with_world ? history_ : empty_;
    renderer::Camera& cam = renderer_.camera();
    const f32 heading = cam.heading();
    const f32 pitch = cam.pitch();
    const bool held = cam.rotated();
    renderer_.clear_scene();
    renderer_.build_scene(&terrain, ctx_.sim.blueprint_store(),
                          with_world ? sim::world_blueprints(ctx_.sim) : std::vector<std::string>{},
                          &ctx_.vfs, ctx_.L);
    // The scene's CameraReset drops the test's view: its held turn comes back
    if (held) {
        cam.set_heading(heading);
        cam.set_pitch(pitch);
    }
    cam.set_input_enabled(false);
    cam.set_eye_distance(distance);
    cam.set_target(x, z);
    // Draw until every texture the view asks for has loaded (they load as
    // they are first drawn), then capture one more frame.
    const auto draw = [&] {
        renderer_.render(sim::FrameView(&world.prev(), &world.cur(), 1.0f), world.events(), nullptr,
                         ctx_.L);
        renderer_.poll_events(0.016);
    };
    int settled = 0;
    for (int f = 0; f < 600 && settled < 3; ++f) {
        draw();
        settled = renderer_.texture_cache().loading() == 0 ? settled + 1 : 0;
    }
    ImageRGBA8 shot;
    renderer_.request_capture([&](ImageRGBA8 image) { shot = std::move(image); });
    draw();
    return shot;
}

} // namespace osc::test
