// --scroll-render-test: tank treads scroll, as Moho's texture scrollers move
// a Scrolling LOD's texture (CTextureScroller, mesh.fx ComputeScrolledTexcoord).

#include "integration_tests.hpp"
#include "intel_probe.hpp"
#include "render_probe.hpp"

#include "map/terrain.hpp"
#include "renderer/renderer.hpp"
#include "renderer/unit_renderer.hpp"
#include "sim/entity.hpp"
#include "sim/sim_state.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>

namespace osc::test {

namespace {

const renderer::MeshInstance* instance_at(const renderer::Renderer& r, f32 x, f32 z) {
    const renderer::MeshInstance* all = r.unit_renderer().mesh_instances();
    for (const auto& g : r.unit_renderer().mesh_groups()) {
        for (u32 i = 0; all && i < g.instance_count; ++i) {
            const renderer::MeshInstance& m = all[g.instance_offset + i];
            if (std::abs(m.model[12] - x) < 0.1f && std::abs(m.model[14] - z) < 0.1f) {
                return &m;
            }
        }
    }
    return nullptr;
}

int pixels_changed(const Pixels& a, const Pixels& b) {
    int changed = 0;
    for (size_t i = 0; i < std::min(a.size(), b.size()); ++i) {
        for (int c = 0; c < 3; ++c) {
            if (std::abs(a[i][c] - b[i][c]) > 0.02f) {
                ++changed;
                break;
            }
        }
    }
    return changed;
}

} // namespace

void test_scroll_render(TestContext& ctx) {
    spdlog::info("=== Scroll render test: texture scrollers ===");
    Tally t;
    const map::Terrain* terrain = ctx.sim.terrain();
    const auto spot = terrain ? quiet_spot(ctx.sim) : std::nullopt;
    if (!spot) {
        t.check(false, "the map, and dry ground 60 from every unit");
        return;
    }

    const u32 still_id = spawn_unit(ctx, "__osc_scroll_still", "uel0201", "ARMY_1", *spot);
    const u32 mover_id =
        spawn_unit(ctx, "__osc_scroll_mover", "uel0201", "ARMY_1", {spot->x + 30, spot->z});
    for (int i = 0; i < 3; ++i) {
        ctx.sim.tick();
    }
    sim::Entity* still = ctx.sim.entity_registry().find(still_id);
    sim::Entity* mover = ctx.sim.entity_registry().find(mover_id);
    if (!still || !mover) {
        t.check(false, "two MA12 Strikers");
        return;
    }

    run_lua(ctx, fmt::format("IssueMove({{__osc_scroll_mover}}, {{{}, 0, {}}})", spot->x + 30,
                             spot->z + 40));
    for (int i = 0; i < 30; ++i) {
        ctx.sim.tick();
    }
    // Test 1: a tank that drives off gets its thread scroller from Unit.lua's
    // CreateTreads, with its blueprint's ScrollMultiplier
    const auto& made = mover->scroller();
    t.check(made && made->spec.type == sim::ScrollType::MotionDerived &&
                made->spec.side_dist == 1.0f && made->spec.scroll_mult == 0.75f,
            fmt::format("Test 1: the tank's scroller: {}, side {:.2f}, multiplier {:.2f}",
                        made ? static_cast<int>(made->spec.type) : -1,
                        made ? made->spec.side_dist : 0.0f, made ? made->spec.scroll_mult : 0.0f));
    // Test 2: driving forward scrolls both treads alike
    const sim::Scroll driven = mover->scroll_end();
    t.check(driven.u > 0.5f && std::abs(driven.u - driven.v) < 0.05f * driven.u,
            fmt::format("Test 2: after 3 s driving, the treads scrolled {:.3f} and {:.3f}",
                        driven.u, driven.v));

    OffscreenShots shots(ctx);
    if (!shots.ok()) {
        t.check(false, "renderer init (no Vulkan?)");
        return;
    }
    renderer::Renderer& r = shots.renderer();

    // Test 3: the moving tank's mesh instance carries its scroll
    (void)shots.shoot(*terrain, mover->position().x, mover->position().z, 40.0f);
    const renderer::MeshInstance* inst = instance_at(r, mover->position().x, mover->position().z);
    t.check(inst && inst->scroll_u > 0.0f && inst->scroll_u == mover->scroll_end().u &&
                inst->scroll_v == mover->scroll_end().v,
            fmt::format("Test 3: the mesh instance's scroll {:.3f}, {:.3f}; the sim's {:.3f}, "
                        "{:.3f}",
                        inst ? inst->scroll_u : -1.0f, inst ? inst->scroll_v : -1.0f,
                        mover->scroll_end().u, mover->scroll_end().v));

    // Test 4: a still tank's treads move only by its scroll
    const f32 sx = still->position().x;
    const f32 sz = still->position().z;
    const Pixels before = shots.shoot(*terrain, sx, sz, 10.0f);
    shots.recapture();
    const Pixels unscrolled = centre_pixels(shots.grab());
    run_lua(ctx, "__osc_scroll_still:AddManualScroller(0.25, 0.25)");
    still->tick_scroller();
    shots.recapture();
    const Pixels scrolled = centre_pixels(shots.grab());
    const int still_changed = pixels_changed(before, unscrolled);
    const int scroll_changed = pixels_changed(unscrolled, scrolled);
    t.check(still_changed == 0 && scroll_changed > 100,
            fmt::format("Test 4: {} pixels changed redrawn, {} once the treads scrolled a quarter",
                        still_changed, scroll_changed));

    spdlog::info("Scroll render test: {} passed, {} failed", t.pass, t.fail);
}

} // namespace osc::test
