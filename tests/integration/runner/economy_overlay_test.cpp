// --economy-overlay-test: RenderOverlayEconomy's per-unit readout, as Moho's
// CWldSession::DrawEconomyOverlay draws it (/lua/ui/game/econoverlayparams.lua).

#include "integration_tests.hpp"
#include "intel_probe.hpp"
#include "render_probe.hpp"

#include "renderer/economy_overlay_renderer.hpp"
#include "renderer/renderer.hpp"
#include "sim/sim_state.hpp"
#include "sim/world_snapshot.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>

namespace osc::test {

void test_economy_overlay(TestContext& ctx) {
    spdlog::info("=== ECONOMY OVERLAY TEST: RenderOverlayEconomy's readout ===");
    Tally t;

    const auto spot = quiet_spot(ctx.sim);
    if (!spot) {
        t.check(false, "dry ground 60 from every unit");
        return;
    }
    const f32 sx = spot->x;
    const f32 sz = spot->z;
    const u32 gen = spawn_unit(ctx, "__osc_eo_gen", "ueb1101", "ARMY_1", {sx, sz});
    const u32 tank = spawn_unit(ctx, "__osc_eo_tank", "uel0201", "ARMY_1", {sx + 16, sz});
    const u32 other = spawn_unit(ctx, "__osc_eo_other", "ueb1101", "ARMY_2", {sx - 16, sz});
    ctx.sim.tick();
    ctx.sim.tick();

    OffscreenShots shots(ctx);
    if (!shots.ok()) {
        t.check(false, "renderer init (no Vulkan?)");
        return;
    }
    renderer::Renderer& r = shots.renderer();
    r.set_player_army(0);
    const auto& overlay = r.economy_overlay_renderer();
    const auto readout = [&](u32 id) -> const renderer::EconomyReadout* {
        const auto& all = overlay.readouts();
        const auto it =
            std::find_if(all.begin(), all.end(),
                         [&](const renderer::EconomyReadout& e) { return e.unit_id == id; });
        return it == all.end() ? nullptr : &*it;
    };

    (void)shots.shoot(*ctx.sim.terrain(), sx, sz, 60.0f);
    shots.redraw();
    t.check(overlay.readouts().empty() && overlay.quad_count() == 0,
            "Test 1: RenderOverlayEconomy off: no readout");

    // UIFile resolves the textures under the game UI's skin; this state has none.
    run_lua(ctx, "local p = import('/lua/ui/game/econoverlayparams.lua').EconOverlayParams\n"
                 "local dir = '/textures/ui/common/game/economic-overlay/'\n"
                 "p.leftTexture = dir .. 'econ_bmp_l.dds'\n"
                 "p.midTexture = dir .. 'econ_bmp_m.dds'\n"
                 "p.rightTexture = dir .. 'econ_bmp_r.dds'\n");
    r.set_economy_overlay(true);
    shots.redraw();
    const renderer::EconomyReadout* g = readout(gen);
    t.check(g && g->energy == " +20 " && g->mass == "+0.0" && g->energy_color == 0xFF00D000u &&
                g->mass_color == 0xFF00D000u,
            fmt::format("Test 2: the power generator reads energy '{}' and mass '{}'",
                        g ? g->energy : "-", g ? g->mass : "-"));
    {
        const auto at =
            g ? screen_of(r, ctx.sim.entity_registry().find(gen)->position()) : std::nullopt;
        t.check(g && at && g->bar_height > 0.0f && g->bar_width > 0.0f &&
                    overlay.quad_count() > 3 &&
                    std::abs(g->bar_left + g->bar_width * 0.5f - (*at)[0]) <= 1.0f &&
                    std::abs(g->bar_top + g->bar_height * 0.5f - (*at)[1]) <= 1.0f,
                "Test 3: its bar and numbers are drawn, centred on the unit");
    }
    t.check(!readout(tank) && !readout(other),
            "Test 4: no readout for a unit without economy, nor for another army's");

    run_lua(ctx, "__osc_eo_gen:SetMaintenanceConsumptionActive()\n"
                 "__osc_eo_gen:SetConsumptionPerSecondEnergy(35)\n");
    ctx.sim.tick();
    shots.recapture();
    shots.redraw();
    g = readout(gen);
    t.check(g && g->energy == " -15 " && g->energy_color == 0xFFFF0000u,
            fmt::format("Test 5: drawing 35 of its 20 it reads '{}' in red", g ? g->energy : "-"));

    (void)shots.shoot(*ctx.sim.terrain(), sx, sz, 200.0f);
    r.set_economy_overlay(true);
    shots.redraw();
    t.check(!readout(gen), "Test 6: out past its mesh's IconFadeInZoom, no readout");

    spdlog::info("Economy overlay test: {}/{} passed", t.pass, t.pass + t.fail);
}

} // namespace osc::test
