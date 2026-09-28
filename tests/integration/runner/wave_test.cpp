// --wave-test (M213c): the shoreline's waves on SCMP_009.
//
// The map's 6,939 wave generators reach the renderer; at the shores in the
// map's middle the ones in view emit flat waves on the water, each drifting from its
// generator by its direction a tick; they show (the frame differs from the
// same frame without them); inland, none emit.

#include "core/test_status.hpp"
#include "integration_tests.hpp"
#include "map/terrain.hpp"
#include "render_probe.hpp"
#include "renderer/particle_system.hpp"
#include "renderer/renderer.hpp"
#include "renderer/wave_system.hpp"
#include "sim/sim_state.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <set>

namespace osc::test {

namespace {

/// The shores about the map's middle (960 generators within 150), where
/// no unit's effects are
constexpr f32 kShoreX = 512.0f;
constexpr f32 kShoreZ = 512.0f;
/// Inland: no generator within 150.
constexpr f32 kInlandX = 600.0f;
constexpr f32 kInlandZ = 700.0f;
constexpr f32 kDistance = 60.0f;
/// Frames (and ticks) the waves get to come and go.
constexpr int kFrames = 80;

/// The waves drawn last frame: the particles no effect made.
std::vector<renderer::ParticleSystem::Drawn> drawn_waves(const renderer::Renderer& r) {
    std::vector<renderer::ParticleSystem::Drawn> out;
    for (const auto& d : r.particle_system().drawn())
        if (d.effect_id == 0) out.push_back(d);
    return out;
}

void run(TestContext& ctx, OffscreenShots& shots, int frames) {
    for (int i = 0; i < frames; ++i) {
        ctx.sim.tick();
        shots.recapture();
        shots.redraw();
    }
}

} // namespace

void test_waves(TestContext& ctx) {
    spdlog::info("=== Wave test (M213c) ===");
    Tally t;
    const map::Terrain* terrain = ctx.sim.terrain();
    if (!terrain) {
        t.check(false, "the map");
        return;
    }
    OffscreenShots shots(ctx);
    if (!shots.ok()) {
        t.check(false, "renderer init (no Vulkan?)");
        return;
    }
    renderer::Renderer& r = shots.renderer();
    r.set_fixed_frame_dt(0.1f); // a tick a frame
    const f32 water = terrain->water_elevation();

    // Test 1: the map's generators reach the renderer
    (void)shots.shoot(*terrain, kShoreX, kShoreZ, kDistance);
    const renderer::WaveSystem& waves = r.wave_system();
    t.check(waves.generator_count() == 6939 && waves.generator_count() == terrain->waves().size(),
            fmt::format("Test 1: {} wave generators in the renderer (SCMP_009 has 6939)",
                        waves.generator_count()));

    // Test 2: at the shore those in view emit, and their waves lie flat on
    // the water, each where its generator's drift has taken it
    run(ctx, shots, kFrames);
    const auto drawn = drawn_waves(r);
    size_t placed = 0;
    for (const auto& d : drawn) {
        const bool flat = std::abs(d.axis_x.y) < 1e-6f && std::abs(d.axis_y.y) < 1e-6f &&
                          std::abs(d.center.y - water) < 1e-3f;
        bool from_one = false;
        for (const map::ScmapWaveGenerator& g : terrain->waves()) {
            const f32 x = g.position[0] + g.direction[0] * d.age;
            const f32 z = g.position[2] + g.direction[2] * d.age;
            if (std::abs(d.center.x - x) < 1e-2f && std::abs(d.center.z - z) < 1e-2f) {
                from_one = true;
                break;
            }
        }
        if (flat && from_one) ++placed;
    }
    // Born over the frames, on the system clock, not all at the first look
    std::set<f32> ages;
    for (const auto& d : drawn) ages.insert(d.age);
    const size_t in_view = waves.in_view_count();
    t.check(in_view > 0 && in_view < waves.generator_count() && !drawn.empty() &&
                placed == drawn.size() && ages.size() >= 10,
            fmt::format("Test 2: {} generators in view emit; {} waves drawn ({} ages), {} flat on "
                        "the water where their generators' drift puts them",
                        in_view, drawn.size(), ages.size(), placed));

    // Test 3: they show. The same frame (the same tick) drawn from a copy of
    // the map without them differs only where particles are; the frame had
    // no effect's particles, so the difference is the waves'
    size_t effects = 0;
    for (const auto& d : r.particle_system().drawn())
        if (d.effect_id != 0) ++effects;
    const ImageRGBA8 with = shots.grab();
    map::Terrain dry = *terrain;
    dry.set_waves({});
    (void)shots.shoot(dry, kShoreX, kShoreZ, kDistance);
    const ImageRGBA8 without = shots.grab();
    size_t changed = 0;
    if (with.width == without.width && with.height == without.height) {
        for (size_t i = 0; i + 3 < with.pixels.size(); i += 4) {
            int most = 0;
            for (size_t c = 0; c < 3; ++c)
                most = std::max(most, std::abs(static_cast<int>(with.pixels[i + c]) -
                                               static_cast<int>(without.pixels[i + c])));
            if (most > 4) ++changed;
        }
    }
    t.check(effects == 0 && changed > 200 && drawn_waves(r).empty(),
            fmt::format("Test 3: with the waves, {} pixels differ from the frame without them "
                        "({} effect particles either way)",
                        changed, effects));

    // Test 4: inland, no generator is in view and no wave comes
    (void)shots.shoot(*terrain, kInlandX, kInlandZ, kDistance);
    run(ctx, shots, 20);
    t.check(waves.in_view_count() == 0 && drawn_waves(r).empty(),
            fmt::format("Test 4: inland, {} generators in view, {} waves drawn",
                        waves.in_view_count(), drawn_waves(r).size()));

    spdlog::info("Wave test: {} passed, {} failed", t.pass, t.fail);
}

} // namespace osc::test
