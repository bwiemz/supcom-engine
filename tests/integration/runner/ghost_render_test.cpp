// --ghost-render-test: a structure being placed draws as mesh.fx's UnitPlace,
// tinted by its colour: green where it can be built, red where it can't.

#include "integration_tests.hpp"
#include "intel_probe.hpp"
#include "render_probe.hpp"

#include "renderer/renderer.hpp"
#include "sim/sim_state.hpp"

#include <spdlog/spdlog.h>

#include <cmath>
#include <string>

namespace osc::test {

namespace {

struct Tint {
    f32 r = 0, g = 0, b = 0;
    size_t pixels = 0;
};

Tint changed_tint(const Pixels& before, const Pixels& after) {
    Tint t;
    for (size_t i = 0; i < before.size() && i < after.size(); ++i) {
        const f32 d = std::abs(after[i][0] - before[i][0]) + std::abs(after[i][1] - before[i][1]) +
                      std::abs(after[i][2] - before[i][2]);
        if (d < 0.06f) {
            continue;
        }
        t.r += after[i][0] - before[i][0];
        t.g += after[i][1] - before[i][1];
        t.b += after[i][2] - before[i][2];
        ++t.pixels;
    }
    if (t.pixels > 0) {
        const auto n = static_cast<f32>(t.pixels);
        t.r /= n;
        t.g /= n;
        t.b /= n;
    }
    return t;
}

} // namespace

void test_ghost_render(TestContext& ctx) {
    spdlog::info("=== GHOST TEST: a structure being placed, drawn as UnitPlace ===");
    Tally t;

    const auto spot = quiet_spot(ctx.sim);
    if (!spot) {
        t.check(false, "dry ground 60 from every unit");
        return;
    }
    OffscreenShots shots(ctx);
    if (!shots.ok()) {
        t.check(false, "renderer init (no Vulkan?)");
        return;
    }
    renderer::Renderer& r = shots.renderer();
    (void)shots.shoot(*ctx.sim.terrain(), spot->x, spot->z, 20.0f);

    renderer::BuildGhost ghost;
    ghost.blueprint_id = "ueb1101";
    ghost.x = spot->x;
    ghost.z = spot->z;
    ghost.y = ctx.sim.terrain()->get_terrain_height(spot->x, spot->z);

    const auto shot = [&](const renderer::BuildGhost* g) {
        for (int i = 0; i < 10; ++i) {
            shots.redraw(nullptr, g);
        }
        return centre_pixels(shots.grab(nullptr, g));
    };
    const Pixels bare = shot(nullptr);

    ghost.valid = true;
    const Pixels green = shot(&ghost);
    const u32 drawn = r.mesh_draws(renderer::MeshTechnique::UnitPlace);
    t.check(drawn == 1, fmt::format("Test 1: the ghost draws once as UnitPlace ({})", drawn));

    const Tint g = changed_tint(bare, green);
    t.check(
        g.pixels > 100 && g.g > g.r + 0.05f && g.g > g.b + 0.05f,
        fmt::format("Test 2: a buildable ghost tints green: {} pixels, change r {:.3f} g {:.3f} "
                    "b {:.3f}",
                    g.pixels, g.r, g.g, g.b));

    ghost.valid = false;
    const Tint red = changed_tint(bare, shot(&ghost));
    t.check(red.pixels > 100 && red.r > red.g + 0.05f && red.r > red.b + 0.05f,
            fmt::format("Test 3: an unbuildable ghost tints red: {} pixels, change r {:.3f} g "
                        "{:.3f} b {:.3f}",
                        red.pixels, red.r, red.g, red.b));

    // A build template's ghosts: each structure drawn as its own blueprint,
    // not as the first's. A factory beside the generator draws otherwise
    // than a second generator there would.
    ghost.valid = true;
    renderer::BuildGhost beside = ghost;
    beside.x = spot->x + 3.0f;
    beside.y = ctx.sim.terrain()->get_terrain_height(beside.x, spot->z);
    renderer::BuildGhost two_gens = ghost;
    two_gens.line = {beside};
    beside.blueprint_id = "ueb0101";
    renderer::BuildGhost gen_and_factory = ghost;
    gen_and_factory.line = {beside};
    const Pixels gens = shot(&two_gens);
    const Tint factory = changed_tint(gens, shot(&gen_and_factory));
    // (Two of one mesh are one instanced draw; two meshes, two.)
    const u32 both = r.mesh_draws(renderer::MeshTechnique::UnitPlace);
    t.check(both == 2 && factory.pixels > 100,
            fmt::format("Test 4: a template's ghosts each draw their own blueprint ({} draws, a "
                        "factory for a generator changes {} pixels)",
                        both, factory.pixels));

    spdlog::info("Ghost test: {}/{} passed", t.pass, t.pass + t.fail);
}

} // namespace osc::test
