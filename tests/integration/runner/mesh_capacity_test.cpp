// --mesh-capacity-test: a frame draws every mesh in view, however many.
//
// The unit renderer's per-frame buffers held a fixed 8,192 mesh instances;
// a whole map in view (a big game's units and the map's props) has more,
// and the rest went undrawn. Each frame's instance buffer and bone SSBO now
// grow to hold what it draws, and the bone SSBO's descriptor set follows it
// to its new buffer (the validation layer, on in Debug builds, fails the run
// if a draw binds the old one). At strategic zoom, which draws icons in the
// meshes' place, the frame builds no instances at all.

#include "integration_tests.hpp"
#include "intel_probe.hpp"
#include "render_probe.hpp"

#include "map/terrain.hpp"
#include "renderer/renderer.hpp"
#include "renderer/unit_renderer.hpp"
#include "sim/sim_state.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>

namespace osc::test {

namespace {

using renderer::UnitRenderer;

/// This frame's mesh instances, and the bone matrices its groups take.
struct FrameLoad {
    u64 instances = 0;
    u64 bones = 0;
};

FrameLoad drawn_now(const renderer::Renderer& r) {
    FrameLoad d;
    for (const auto& g : r.unit_renderer().mesh_groups()) {
        d.instances += g.instance_count;
        d.bones += u64{g.instance_count} * g.bones_per_instance;
    }
    return d;
}

// Rocks, 96 by 96 a unit apart, and UEF T3 air factories (32 bones each),
// 16 by 10: within one view at the tests' pitch, short of strategic zoom.
constexpr int kPropSide = 96;
constexpr int kFactoryColumns = 16;
constexpr int kFactoryRows = 10;
// More than the 8,192 mesh instances the buffers held
static_assert(kPropSide * kPropSide > 8192);
constexpr f32 kViewDistance = 220.0f;
// Past StrategicIconRenderer::ZOOM_THRESHOLD
constexpr f32 kStrategicDistance = 320.0f;

/// The mesh instance drawn at (x, z) this frame, or null.
const renderer::MeshInstance* instance_at(const renderer::Renderer& r, f32 x, f32 z) {
    const renderer::MeshInstance* all = r.unit_renderer().mesh_instances();
    for (const auto& g : r.unit_renderer().mesh_groups())
        for (u32 i = 0; all && i < g.instance_count; ++i) {
            const renderer::MeshInstance& m = all[g.instance_offset + i];
            if (std::abs(m.model[12] - x) < 0.1f && std::abs(m.model[14] - z) < 0.1f) return &m;
        }
    return nullptr;
}

} // namespace

void test_mesh_capacity(TestContext& ctx) {
    spdlog::info("=== Mesh capacity test ===");
    Tally t;
    const map::Terrain* terrain = ctx.sim.terrain();
    const auto spot = terrain ? quiet_spot(ctx.sim) : std::nullopt;
    if (!spot) {
        t.check(false, "the map, and dry ground 60 from every unit");
        return;
    }

    run_lua(ctx, fmt::format("local cx, cz = {}, {}\n"
                             "for i = 0, {} - 1 do for j = 0, {} - 1 do\n"
                             "  local x, z = cx - {} / 2 + i, cz - {} / 2 + j\n"
                             "  CreateProp({{x, GetTerrainHeight(x, z), z}},\n"
                             "             '/env/evergreen/props/rocks/rock01_prop.bp')\n"
                             "end end\n"
                             "for i = 0, {} - 1 do for j = 0, {} - 1 do\n"
                             "  local x, z = cx - 88 + i * 11, cz - 55 + j * 11\n"
                             "  CreateUnitHPR('ueb0302', 'ARMY_1', x, GetTerrainHeight(x, z), z,"
                             " 0, 0, 0)\n"
                             "end end\n",
                             spot->x, spot->z, kPropSide, kPropSide, kPropSide, kPropSide,
                             kFactoryColumns, kFactoryRows));
    for (int i = 0; i < 2; ++i) ctx.sim.tick();
    const u64 spawned = u64{kPropSide} * kPropSide + u64{kFactoryColumns} * kFactoryRows;

    OffscreenShots shots(ctx);
    if (!shots.ok()) {
        t.check(false, "renderer init (no Vulkan?)");
        return;
    }
    renderer::Renderer& r = shots.renderer();
    const u32 errors_before = renderer::Renderer::validation_error_count();

    // Test 1: every mesh in view is drawn, past the 8,192 the buffers held,
    // and a slot's instance buffer grew to hold them
    (void)shots.shoot(*terrain, spot->x, spot->z, kViewDistance);
    const FrameLoad first = drawn_now(r);
    const UnitRenderer& units = r.unit_renderer();
    u32 most_meshes = 0;
    u32 most_bones = 0;
    for (u32 fi = 0; fi < UnitRenderer::FRAMES_IN_FLIGHT; ++fi) {
        most_meshes = std::max(most_meshes, units.mesh_capacity(fi));
        most_bones = std::max(most_bones, units.bone_capacity(fi));
    }
    t.check(first.instances >= spawned && most_meshes > first.instances,
            fmt::format("Test 1: {} mesh instances drawn of the {} spawned (and the map's in "
                        "view), in a buffer of {}",
                        first.instances, spawned, most_meshes));

    // Test 2: the factories' bones overflow the bone SSBO's first size; it
    // grew to hold them all
    t.check(first.bones > UnitRenderer::kInitialBones && most_bones >= first.bones,
            fmt::format("Test 2: {} bone matrices (the SSBO starts at {}), in an SSBO of {}",
                        first.bones, UnitRenderer::kInitialBones, most_bones));

    // Test 3: the next frames, in each slot, draw the same, both slots grown
    // and their bone sets moved to the new buffers (no validation errors)
    shots.redraw();
    shots.redraw();
    const FrameLoad again = drawn_now(r);
    u32 least_meshes = UINT32_MAX;
    u32 least_bones = UINT32_MAX;
    for (u32 fi = 0; fi < UnitRenderer::FRAMES_IN_FLIGHT; ++fi) {
        least_meshes = std::min(least_meshes, units.mesh_capacity(fi));
        least_bones = std::min(least_bones, units.bone_capacity(fi));
    }
    const u32 errors = renderer::Renderer::validation_error_count() - errors_before;
    t.check(again.instances == first.instances && least_meshes > again.instances &&
                least_bones >= again.bones && r.bone_sets_current() && errors == 0,
            fmt::format("Test 3: the next frames draw {} instances; each slot holds at least {} "
                        "and {} bones, its bone set {}; {} validation error(s)",
                        again.instances, least_meshes, least_bones,
                        r.bone_sets_current() ? "current" : "stale", errors));

    // Test 4: strategic zoom builds no instances, but keeps each mesh's
    // birth: a tank appearing out there has the tick it appeared as its
    // mesh's age once the view comes in (FA makes a mesh instance as its
    // entity appears; material.x)
    {
        r.camera().set_eye_distance(kStrategicDistance);
        shots.redraw();
        const u64 far = drawn_now(r).instances;
        const Spot at{spot->x, spot->z + 70};
        const u32 tank = spawn_unit(ctx, "__osc_mc_tank", "uel0201", "ARMY_1", at);
        shots.recapture();
        shots.redraw();
        const u32 born = ctx.sim.tick_count();
        for (int i = 0; i < 20; ++i) {
            ctx.sim.tick();
            shots.recapture();
            shots.redraw();
        }
        r.camera().set_eye_distance(kViewDistance);
        shots.redraw();
        const sim::Entity* e = ctx.sim.entity_registry().find(tank);
        const renderer::MeshInstance* inst =
            e ? instance_at(r, e->position().x, e->position().z) : nullptr;
        // Made within a tick of the tank's creation, not as the view came in
        t.check(far == 0 && inst && std::abs(inst->shader_time - static_cast<f32>(born)) <= 1.0f,
                fmt::format("Test 4: {} instances at strategic zoom; the tank's mesh, drawn "
                            "again close in, {}made at tick {:.0f} (created at {}, now {})",
                            far, inst ? "" : "not found, ", inst ? inst->shader_time : -1.0f, born,
                            ctx.sim.tick_count()));
    }

    spdlog::info("Mesh capacity test: {} passed, {} failed", t.pass, t.fail);
}

} // namespace osc::test
