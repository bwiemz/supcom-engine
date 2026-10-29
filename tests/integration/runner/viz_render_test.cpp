// --viz-render-test: what a script hides with SetVizToFocusPlayer,
// ...Allies, ...Enemies and ...Neutrals('Never') isn't drawn (Ryan,
// 2026-10-16: a white box fell with each of his aircraft shot down; FAF's
// ShieldCollider, the projectile that follows a crashing aircraft to hit
// shields, hides itself so and the engine drew it). Three of the player's
// tanks: one as made, one hidden from everyone, one hidden from enemies
// only (the player still sees it).

#include "integration_tests.hpp"
#include "intel_probe.hpp"
#include "render_probe.hpp"
#include "map/terrain.hpp"
#include "renderer/mesh_cache.hpp"
#include "renderer/renderer.hpp"
#include "renderer/unit_renderer.hpp"
#include "sim/entity_registry.hpp"
#include "sim/sim_state.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

namespace osc::test {

namespace {

/// Mesh instances drawn within 1.5 of `at` (flat), as the last frame left them.
int drawn_at(const renderer::Renderer& r, const sim::Vector3& at) {
    int n = 0;
    const renderer::MeshInstance* all = r.unit_renderer().mesh_instances();
    for (const auto& g : r.unit_renderer().mesh_groups()) {
        for (u32 i = 0; i < g.instance_count; ++i) {
            const renderer::MeshInstance& m = all[g.instance_offset + i];
            const f32 dx = m.model[12] - at.x, dz = m.model[14] - at.z;
            if (dx * dx + dz * dz <= 1.5f * 1.5f) ++n;
        }
    }
    return n;
}

} // namespace

void test_viz_render(TestContext& ctx) {
    spdlog::info("=== Viz render test: SetVizTo...('Never') hides an entity ===");
    Tally t;
    const map::Terrain* terrain = ctx.sim.terrain();
    const auto spot = terrain ? quiet_spot(ctx.sim) : std::nullopt;
    if (!spot) {
        t.check(false, "the map, and dry ground 60 from every unit");
        return;
    }
    const sim::EntityId shown = spawn_unit(ctx, "__osc_viz_shown", "uel0201", "ARMY_1", *spot);
    const sim::EntityId hidden =
        spawn_unit(ctx, "__osc_viz_hidden", "uel0201", "ARMY_1", Spot{spot->x + 8, spot->z});
    const sim::EntityId enemies_only =
        spawn_unit(ctx, "__osc_viz_mine", "uel0201", "ARMY_1", Spot{spot->x + 16, spot->z});
    run_lua(
        ctx,
        "for _, f in {'SetVizToFocusPlayer', 'SetVizToAllies', 'SetVizToEnemies',\n"
        "              'SetVizToNeutrals'} do __osc_viz_hidden[f](__osc_viz_hidden, 'Never') end\n"
        "__osc_viz_mine:SetVizToEnemies('Never')\n");
    for (int i = 0; i < 3; ++i) ctx.sim.tick();

    OffscreenShots shots(ctx);
    if (!shots.ok()) {
        t.check(false, "renderer init (no Vulkan?)");
        return;
    }
    shots.recapture();
    (void)shots.shoot_frame(*terrain, spot->x + 8, spot->z, 40.0f);
    const auto& registry = ctx.sim.entity_registry();
    const auto at = [&](sim::EntityId id) { return registry.find(id)->position(); };
    const int a = drawn_at(shots.renderer(), at(shown));
    const int b = drawn_at(shots.renderer(), at(hidden));
    const int c = drawn_at(shots.renderer(), at(enemies_only));
    t.check(a == 1 && b == 0 && c == 1,
            fmt::format("drawn: as made {}, hidden from all {}, hidden from enemies {} "
                        "(1, 0, 1 wanted)",
                        a, b, c));
    spdlog::info("Viz render test: {} passed, {} failed", t.pass, t.fail);
}

} // namespace osc::test
