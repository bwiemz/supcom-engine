// The shoreline's waves (M213c), as Moho's WaveSystem runs them: each
// generator in view emits a flat, ramp-coloured particle once its interval
// has passed on the system clock; which are in view is looked at every fifth
// tick; the particles join the world's, born at the next frame.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "lua/lua_state.hpp"
#include "map/scmap_parser.hpp"
#include "renderer/camera.hpp"
#include "renderer/emitter_blueprint.hpp"
#include "renderer/frustum.hpp"
#include "renderer/particle_system.hpp"
#include "renderer/wave_system.hpp"
#include "sim/world_snapshot.hpp"

#include <cmath>
#include <set>
#include <vector>

using namespace osc;
using namespace osc::renderer;
using Catch::Matchers::WithinAbs;

namespace {

/// Looking straight down on the origin from 100 up, 45 degrees across: the
/// ground within 41 of it is in view.
Frustum view_of_origin() {
    const auto view = math::look_at(0, 100, 0, 0, 0, 0, 0, 0, -1);
    const auto proj = math::perspective(3.14159f / 4.0f, 1.0f, 1.0f, 500.0f);
    return Frustum(math::mat4_mul(proj, view));
}

map::ScmapWaveGenerator generator(f32 x, f32 z) {
    map::ScmapWaveGenerator g;
    g.texture = "/env/common/decals/shoreline/turbulance02_albedo.dds";
    g.ramp = "/env/common/decals/shoreline/waveramptest.dds";
    g.position[0] = x;
    g.position[1] = 17.5f;
    g.position[2] = z;
    g.angle = 0.5f;
    g.direction[0] = 0.03f;
    g.direction[2] = 0.04f; // 0.05 a tick
    g.lifetime[0] = 40.0f;
    g.lifetime[1] = 40.0f;
    g.interval[0] = 2.0f;
    g.interval[1] = 2.0f;
    g.begin_size = 3.0f;
    g.end_size = 6.0f;
    g.frame_count = 4.0f;
    g.frame_rate[0] = 0.25f;
    g.frame_rate[1] = 0.5f;
    g.strip_count = 3.0f;
    return g;
}

} // namespace

TEST_CASE("A generator's bounds reach as far as its waves drift (M213c)", "[renderer][waves]") {
    WaveSystem waves;
    waves.load({generator(10.0f, 20.0f)}, 0.0);
    // 0.05 a tick for its 40-tick life, plus half its larger size (6)
    const auto lo = waves.bounds_min(0);
    const auto hi = waves.bounds_max(0);
    CHECK_THAT(lo[0], WithinAbs(10.0 - 5.0, 1e-4));
    CHECK_THAT(hi[0], WithinAbs(10.0 + 5.0, 1e-4));
    CHECK_THAT(lo[2], WithinAbs(20.0 - 5.0, 1e-4));
    CHECK_THAT(hi[2], WithinAbs(20.0 + 5.0, 1e-4));
    CHECK_THAT(lo[1], WithinAbs(17.4, 1e-4));
    CHECK_THAT(hi[1], WithinAbs(17.6, 1e-4));
}

TEST_CASE("Generators in view emit on their intervals, looked for every fifth tick (M213c)",
          "[renderer][waves]") {
    const Frustum frustum = view_of_origin();
    WaveSystem waves;
    // One in view, one far out of it
    waves.load({generator(0.0f, 0.0f), generator(300.0f, 0.0f)}, 10.0);
    REQUIRE(waves.generator_count() == 2);
    std::vector<WaveParticle> out;
    // Ticks 1-4: not looked for yet, so none emits
    for (u32 tick = 1; tick <= 4; ++tick)
        waves.update(frustum, 0.25f, tick, 10.0 + tick * 5.0, out);
    CHECK(waves.in_view_count() == 0);
    CHECK(out.empty());

    // Tick 5 onward (every 0.25 s): only the one in view, first within its
    // interval (it starts out of step), then every 2.25 s (on the first step
    // past 2)
    std::vector<f64> times;
    for (int i = 0; i <= 40; ++i) {
        const f64 now = 30.0 + 0.25 * i;
        const size_t before = out.size();
        waves.update(frustum, 0.25f, 5 + static_cast<u32>(i), now, out);
        if (out.size() > before) times.push_back(now);
    }
    CHECK(waves.in_view_count() == 1);
    REQUIRE(times.size() >= 4);
    for (const WaveParticle& p : out) CHECK(p.position.x == 0.0f); // never the far one
    for (size_t i = 1; i < times.size(); ++i)
        CHECK_THAT(times[i] - times[i - 1], WithinAbs(2.25, 1e-9));

    // A frame of over 200 seconds emits nothing
    out.clear();
    waves.update(frustum, 250.0f, 50, 100.0, out);
    CHECK(out.empty());
    waves.update(frustum, 0.25f, 51, 100.0, out);
    CHECK(out.size() == 1);
}

TEST_CASE("Generators start out of step (M213c)", "[renderer][waves]") {
    // Five alike, in view, each emitting every 2 s: their first emissions
    // spread over that first interval, not all at once
    const Frustum frustum = view_of_origin();
    WaveSystem waves;
    waves.load({generator(0.0f, 0.0f), generator(2.0f, 0.0f), generator(4.0f, 0.0f),
                generator(0.0f, 2.0f), generator(0.0f, 4.0f)},
               0.0);
    std::vector<WaveParticle> out;
    std::set<int> first_steps;
    std::set<f32> seen;
    for (int i = 0; i <= 12; ++i) {
        out.clear();
        waves.update(frustum, 0.25f, 5 * static_cast<u32>(i), 0.25 * i, out);
        for (const WaveParticle& p : out) {
            const f32 key = p.position.x * 10.0f + p.position.z;
            if (seen.insert(key).second) first_steps.insert(i);
        }
    }
    CHECK(seen.size() == 5);
    CHECK(first_steps.size() >= 3);
}

TEST_CASE("A wave takes its generator's place, drift, sizes and turn (M213c)",
          "[renderer][waves]") {
    const Frustum frustum = view_of_origin();
    map::ScmapWaveGenerator g = generator(5.0f, -5.0f);
    g.lifetime[0] = 30.0f;
    g.lifetime[1] = 60.0f;
    WaveSystem waves;
    waves.load({g, generator(-5.0f, 5.0f)}, 0.0);
    std::vector<WaveParticle> out;
    for (int i = 0; i < 400; ++i)
        waves.update(frustum, 0.25f, 5 * static_cast<u32>(i), 0.25 * i, out);
    REQUIRE(out.size() > 50);
    std::set<f32> strips;
    const EmitterBlueprintData* bp = out.front().bp;
    for (const WaveParticle& p : out) {
        CHECK(p.bp == bp); // one blueprint for one texture, ramp and layout
        if (p.position.x != 5.0f) continue;
        CHECK(p.position.y == 17.5f);
        CHECK(p.position.z == -5.0f);
        CHECK(p.velocity.x == 0.03f);
        CHECK(p.velocity.z == 0.04f);
        CHECK(p.lifetime >= 30.0f);
        CHECK(p.lifetime < 60.0f);
        CHECK(p.begin_size == 3.0f);
        CHECK(p.end_size == 6.0f);
        CHECK(p.angle == 0.5f);
        CHECK(p.framerate >= 0.25f);
        CHECK(p.framerate < 0.5f);
        strips.insert(p.texture_selection);
    }
    // Its strip: one of three, from the top
    CHECK(strips.size() == 3);
    for (f32 s : strips) {
        const f32 k = s * 3.0f;
        CHECK_THAT(k, WithinAbs(std::round(k), 1e-5));
        CHECK(s < 1.0f);
    }
    // Its draw: its textures, flat, alpha-blended, its frames and strips
    CHECK(bp->texture == g.texture);
    CHECK(bp->ramp_texture == g.ramp);
    CHECK(bp->flat);
    CHECK(bp->blendmode == 0);
    CHECK(bp->frame_count == 4.0f);
    CHECK(bp->strip_count == 3.0f);
    // Another texture draws apart
    map::ScmapWaveGenerator other = generator(0.0f, 0.0f);
    other.texture = "/env/common/decals/shoreline/turbulance03_albedo.dds";
    waves.load({g, other}, 0.0);
    out.clear();
    for (int i = 0; i < 40; ++i)
        waves.update(frustum, 0.25f, 5 * static_cast<u32>(i), 0.25 * i, out);
    std::set<const EmitterBlueprintData*> bps;
    for (const WaveParticle& p : out) bps.insert(p.bp);
    CHECK(bps.size() == 2);
}

TEST_CASE("A wave joins the world's particles at the next frame (M213c)", "[renderer][waves]") {
    WaveSystem waves;
    waves.load({generator(0.0f, 0.0f)}, 0.0);
    std::vector<WaveParticle> out;
    waves.update(view_of_origin(), 0.25f, 0, 10.0, out);
    REQUIRE(out.size() == 1);
    WaveParticle w = out.front();
    w.framerate = 0.5f;
    w.texture_selection = 1.0f / 3.0f;

    lua::LuaState lua;
    EmitterBlueprintCache cache;
    Camera camera;
    ParticleSystem ps;
    std::vector<sim::WorldSnapshot> ticks(4);
    for (u32 t = 0; t < ticks.size(); ++t) ticks[t].tick = t + 1;
    ps.add_wave(w);
    CHECK(ps.drawn().empty()); // not until a frame
    ps.update(sim::FrameView(&ticks[0], &ticks[1], 0.0f), camera, nullptr, cache, lua.raw(),
              nullptr);
    REQUIRE(ps.drawn().size() == 1);
    CHECK(ps.drawn()[0].age == 0.0f); // born this frame
    CHECK(ps.drawn()[0].effect_id == 0);
    // Born at tick 2 (its frame's interpolant 0); drawn at tick 4 with the
    // interpolant 1, three ticks on: drifted, grown, turned, its frame and
    // strip
    ps.update(sim::FrameView(&ticks[2], &ticks[3], 1.0f), camera, nullptr, cache, lua.raw(),
              nullptr);
    REQUIRE(ps.drawn().size() == 1);
    const auto& d = ps.drawn()[0];
    CHECK_THAT(d.age, WithinAbs(3.0, 1e-5));
    CHECK_THAT(d.center.x, WithinAbs(0.03 * 3, 1e-5));
    CHECK_THAT(d.center.y, WithinAbs(17.5, 1e-5));
    CHECK_THAT(d.center.z, WithinAbs(0.04 * 3, 1e-5));
    const f32 size = 3.0f + (6.0f - 3.0f) / w.lifetime * 3.0f;
    CHECK_THAT(d.axis_x.x, WithinAbs(std::cos(0.5f) * size, 1e-4));
    CHECK_THAT(d.axis_x.z, WithinAbs(std::sin(0.5f) * size, 1e-4));
    CHECK(d.axis_x.y == 0.0f);
    CHECK(d.axis_y.y == 0.0f);
    // A quarter of the texture across (4 frames), frame floor(0.5 * 3) = 1;
    // the second of three strips
    CHECK_THAT(d.uv[0], WithinAbs(0.25 * 1, 1e-5));
    CHECK_THAT(d.uv[1], WithinAbs(0.25, 1e-5));
    CHECK_THAT(d.uv[2], WithinAbs(1.0 / 3.0, 1e-5));
    CHECK_THAT(d.uv[3], WithinAbs(1.0 / 3.0, 1e-5));
    CHECK(d.blendmode == 0);
}
