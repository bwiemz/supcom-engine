// --feedback-render-test: an order's mark (AddCommandFeedbackBlip), drawn as
// mesh.fx's CommandFeedback over the ground for its duration, then gone.

#include "integration_tests.hpp"
#include "intel_probe.hpp"
#include "render_probe.hpp"

#include "renderer/renderer.hpp"
#include "sim/sim_state.hpp"
#include "ui/ui_control.hpp"

#include <spdlog/spdlog.h>

#include <cmath>
#include <string>

namespace osc::test {

namespace {

size_t changed_pixels(const Pixels& before, const Pixels& after) {
    size_t n = 0;
    for (size_t i = 0; i < before.size() && i < after.size(); ++i) {
        const f32 d = std::abs(after[i][0] - before[i][0]) + std::abs(after[i][1] - before[i][1]) +
                      std::abs(after[i][2] - before[i][2]);
        if (d >= 0.06f) ++n;
    }
    return n;
}

} // namespace

void test_feedback_render(TestContext& ctx) {
    spdlog::info("=== FEEDBACK TEST: an order's mark, drawn as CommandFeedback ===");
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

    const auto shot = [&](int frames) {
        for (int i = 0; i < frames; ++i) shots.redraw();
        return centre_pixels(shots.grab());
    };
    const Pixels bare = shot(2);

    // FA's default marks for a move (commandmode.lua AddDefaultCommandFeedbackBlips).
    renderer::FeedbackBlipSpec flag;
    flag.position = {spot->x, ctx.sim.terrain()->get_surface_height(spot->x, spot->z), spot->z};
    flag.mesh_name = "/meshes/game/flag02d_lod0.scm";
    flag.texture_name = "/meshes/game/flag02d_albedo.dds";
    flag.shader_name = "CommandFeedback";
    flag.uniform_scale = 0.5f;
    flag.duration = 30.0f; // long enough for the shots
    renderer::FeedbackBlipSpec crosshair = flag;
    crosshair.mesh_name = "/meshes/game/crosshair02d_lod0.scm";
    crosshair.texture_name = "/meshes/game/crosshair02d_albedo.dds";
    crosshair.shader_name = "CommandFeedback2";
    r.add_command_feedback_blip(flag);
    r.add_command_feedback_blip(crosshair);

    const Pixels marked = shot(2);
    const u32 flags = r.mesh_draws(renderer::MeshTechnique::CommandFeedback);
    const u32 crosses = r.mesh_draws(renderer::MeshTechnique::CommandFeedback2);
    t.check(flags == 1 && crosses == 1,
            fmt::format("Test 1: the flag draws as CommandFeedback ({}), the crosshair as "
                        "CommandFeedback2 ({})",
                        flags, crosses));
    const size_t changed = changed_pixels(bare, marked);
    t.check(changed > 50, fmt::format("Test 2: the marks show ({} pixels changed)", changed));

    // A structure's mark: its blueprint's mesh, with the flag's albedo.
    r.add_command_feedback_blip([&] {
        renderer::FeedbackBlipSpec site = flag;
        site.mesh_name.clear();
        site.blueprint_id = "ueb1101";
        site.uniform_scale = 1.0f;
        return site;
    }());
    (void)shot(1);
    const u32 with_site = r.mesh_draws(renderer::MeshTechnique::CommandFeedback);
    t.check(with_site == 2,
            fmt::format("Test 3: a mark by blueprint id draws its mesh ({} draws)", with_site));

    // Their duration over, they go: frames at 1/60 s.
    renderer::FeedbackBlipSpec brief = flag;
    brief.duration = 0.1f;
    const size_t before = r.feedback_blips().blips().size();
    r.add_command_feedback_blip(brief);
    (void)shot(8);
    const size_t after = r.feedback_blips().blips().size();
    t.check(before == 3 && after == 3,
            fmt::format("Test 4: a 0.1 s mark is gone after 8 frames ({} -> {})", before, after));

    ui::UIControlRegistry ui;
    ui::UIControl& rally = *ui.get(ui.create());
    ui::UIControl::WorldMesh flag_mesh;
    flag_mesh.mesh_name = "/meshes/game/Rally_lod0.scm";
    flag_mesh.texture_name = "/meshes/game/Rally_albedo.dds";
    flag_mesh.shader_name = "RallyPoint";
    flag_mesh.uniform_scale = 0.1f;
    flag_mesh.lifetime = 10.0f;
    flag_mesh.position = {flag.position.x, flag.position.y, flag.position.z};
    rally.world_mesh() = flag_mesh;
    shots.set_ui(&ui);
    const Pixels hidden_flag = shot(2);
    const u32 hidden_draws = r.mesh_draws(renderer::MeshTechnique::RallyPoint);
    rally.world_mesh()->hidden = false;
    const Pixels rally_shown = shot(2);
    const u32 rally_draws = r.mesh_draws(renderer::MeshTechnique::RallyPoint);
    const size_t rally_changed = changed_pixels(hidden_flag, rally_shown);
    t.check(hidden_draws == 0 && rally_draws == 1 && rally_changed > 20,
            fmt::format("Test 5: a WorldMesh draws as RallyPoint once shown ({} -> {} draws, {} "
                        "pixels changed)",
                        hidden_draws, rally_draws, rally_changed));
    shots.set_ui(nullptr);

    spdlog::info("Feedback test: {}/{} passed", t.pass, t.pass + t.fail);
}

} // namespace osc::test
