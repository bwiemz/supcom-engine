// --counter-intel-test (M215d): what counters the player's intel.
//
// Moho counts, before a sense shows the player another army's unit, what
// counters it: a cloak defeats vision (omni defeats the cloak), stealth
// defeats radar (omni defeats that too), radar reaches no unit under the
// water and sonar none out of it. And a structure the player's army
// remembers that dies out of its sight stays drawn, maybe dead, its icon
// darkened, until the army sees the spot. On dry ground away from the
// starts stand ARMY_2's engineers and power generator, and ARMY_1's power
// generator with radar, sonar and omni the test turns on and off; sight is
// scrying.

#include "integration_tests.hpp"
#include "intel_probe.hpp"
#include "render_probe.hpp"

#include "renderer/army_colors.hpp"
#include "renderer/renderer.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/world_snapshot.hpp"

#include <spdlog/spdlog.h>

#include <array>
#include <cmath>
#include <string>
#include <vector>

namespace osc::test {

void test_counter_intel(TestContext& ctx) {
    spdlog::info("=== COUNTER INTEL TEST: cloak, stealth, water and maybe-dead (M215d) ===");
    Tally t;

    const auto spot = quiet_spot(ctx.sim);
    if (!spot) {
        t.check(false, "dry ground 60 from every unit");
        return;
    }
    const f32 sx = spot->x;
    const f32 sz = spot->z;
    spdlog::info("Counter intel test: the spot ({:.0f}, {:.0f})", sx, sz);

    // ARMY_2's: a cloaked engineer, a radar-stealthed one, one said to be on
    // the seabed, one with nothing to hide, and a power generator.
    const Spot cloak_at{sx, sz};
    const Spot stealth_at{sx + 30, sz};
    const Spot diver_at{sx - 30, sz};
    const Spot plain_at{sx, sz + 30};
    const Spot gen_at{sx + 30, sz + 30};
    const u32 cloaked = spawn_unit(ctx, "__osc_ci_cloak", "uel0105", "ARMY_2", cloak_at);
    const u32 stealthy = spawn_unit(ctx, "__osc_ci_stealth", "uel0105", "ARMY_2", stealth_at);
    const u32 diver = spawn_unit(ctx, "__osc_ci_diver", "uel0105", "ARMY_2", diver_at);
    const u32 plain = spawn_unit(ctx, "__osc_ci_plain", "uel0105", "ARMY_2", plain_at);
    const u32 gen = spawn_unit(ctx, "__osc_ci_gen", "ueb1101", "ARMY_2", gen_at);
    (void)spawn_unit(ctx, "__osc_ci_sensor", "ueb1101", "ARMY_1", {sx, sz - 40});
    run_lua(ctx,
            "__osc_ci_cloak:InitIntel(2, 'Cloak', 1)\n"
            "__osc_ci_stealth:InitIntel(2, 'RadarStealth', 1)\n"
            "local s = __osc_ci_sensor\n"
            "s:DisableIntel('Vision') s:DisableIntel('Omni')\n"
            "s:InitIntel(1, 'Radar', 90) s:InitIntel(1, 'Sonar', 90) s:InitIntel(1, 'Omni', 90)\n"
            "s:DisableIntel('Radar') s:DisableIntel('Sonar') s:DisableIntel('Omni')\n");

    OffscreenShots shots(ctx);
    if (!shots.ok()) {
        t.check(false, "renderer init (no Vulkan?)");
        return;
    }
    renderer::Renderer& r = shots.renderer();
    r.set_fog_enabled(true);
    r.set_player_army(0);

    std::vector<Spot> scry;
    const auto senses = [&](bool radar, bool sonar, bool omni) {
        run_lua(ctx, fmt::format("local s = __osc_ci_sensor\n"
                                 "s:{}Intel('Radar') s:{}Intel('Sonar') s:{}Intel('Omni')\n",
                                 radar ? "Enable" : "Disable", sonar ? "Enable" : "Disable",
                                 omni ? "Enable" : "Disable"));
    };
    sim::WorldHistory seen;
    const auto next = [&] {
        for (const Spot& at : scry)
            run_lua(ctx,
                    fmt::format("CreateVisibleAreaAtPoint(1, {}, 0, {}, 12, 0.1)\n", at.x, at.z));
        ctx.sim.tick();
        // The diver keeps to the seabed (the layer's the unit's: radar
        // can't reach it, sonar can).
        if (auto* e = ctx.sim.entity_registry().find(diver); e && e->is_unit())
            static_cast<sim::Unit*>(e)->set_layer("Seabed");
        shots.recapture();
        shots.redraw();
        seen.capture(ctx.sim);
        return drawn(r);
    };
    const auto sight = [&](u32 id) {
        const sim::EntityRecord* e = seen.cur().find(id);
        return e ? r.recon().sight(*e) : renderer::Sight::Hidden;
    };
    const auto drawn_at = [&](const Frame& f, const Spot& at) {
        return mesh_at(f, at.x, at.z) != nullptr;
    };

    // Test 1: in sight, the cloaked engineer isn't drawn; the plain one is.
    scry = {cloak_at, plain_at};
    (void)shots.shoot(*ctx.sim.terrain(), sx, sz + 10, 110.0f);
    Frame f = next();
    t.check(!drawn_at(f, cloak_at) && drawn_at(f, plain_at) &&
                sight(cloaked) == renderer::Sight::Hidden,
            fmt::format("Test 1: in sight, the cloaked engineer is hidden ({}), the plain one "
                        "drawn ({})",
                        name(sight(cloaked)), name(sight(plain))));

    // Test 2: on radar, the cloaked engineer is a blip, never identified.
    senses(true, false, false);
    f = next();
    t.check(!drawn_at(f, cloak_at) && sight(cloaked) == renderer::Sight::Blip,
            fmt::format("Test 2: cloaked, on radar: a blip ({})", name(sight(cloaked))));

    // Test 3: omni defeats the cloak: drawn.
    senses(false, false, true);
    f = next();
    t.check(drawn_at(f, cloak_at) && sight(cloaked) == renderer::Sight::Seen,
            fmt::format("Test 3: cloaked, under omni: drawn ({})", name(sight(cloaked))));

    // Test 4: radar alone misses the stealthed engineer (the plain one, out
    // of sight now, is a blip); omni finds it.
    scry.clear();
    senses(true, false, false);
    f = next();
    const renderer::Sight stealth_radar = sight(stealthy);
    const renderer::Sight plain_radar = sight(plain);
    senses(false, false, true);
    f = next();
    const renderer::Sight stealth_omni = sight(stealthy);
    t.check(stealth_radar == renderer::Sight::Hidden && plain_radar == renderer::Sight::SeenBlip &&
                stealth_omni == renderer::Sight::Blip,
            fmt::format("Test 4: stealthed: on radar {}, under omni {} (plain on radar: {})",
                        name(stealth_radar), name(stealth_omni), name(plain_radar)));

    // Test 5: radar can't reach the seabed; sonar can.
    senses(true, false, false);
    f = next();
    const renderer::Sight diver_radar = sight(diver);
    senses(false, true, false);
    f = next();
    const renderer::Sight diver_sonar = sight(diver);
    const renderer::Sight plain_sonar = sight(plain); // on land: out of sonar's reach
    t.check(diver_radar == renderer::Sight::Hidden && diver_sonar == renderer::Sight::Blip &&
                plain_sonar == renderer::Sight::Hidden,
            fmt::format("Test 5: on the seabed: on radar {}, on sonar {} (on land, on sonar: {})",
                        name(diver_radar), name(diver_sonar), name(plain_sonar)));

    // Test 6: the power generator, seen, then out of sight, dies unseen: it
    // stays drawn, maybe dead, its icon (zoomed out) darkened.
    senses(false, false, false);
    scry = {gen_at};
    f = next();
    scry.clear();
    f = next();
    run_lua(ctx, "__osc_ci_gen:Destroy()\n");
    f = next();
    const bool gone_from_world = seen.cur().find(gen) == nullptr;
    const bool still_drawn = drawn_at(f, gen_at);
    const sim::Vector3 gen_pos{gen_at.x, ctx.sim.terrain()->get_terrain_height(gen_at.x, gen_at.z),
                               gen_at.z};
    r.camera().set_eye_distance(300.0f);
    f = next();
    // Its icon and minimap dot in ARMY_2's colour halved.
    const sim::ArmyRecord* a = seen.cur().army(1);
    const std::array<f32, 3> army =
        a && a->has_color
            ? std::array<f32, 3>{a->r / 255.0f, a->g / 255.0f, a->b / 255.0f}
            : std::array<f32, 3>{renderer::ARMY_COLORS[1][0], renderer::ARMY_COLORS[1][1],
                                 renderer::ARMY_COLORS[1][2]};
    bool dark_icon = false;
    if (const auto at = screen_of(r, gen_pos))
        for (const Quad& q : f.icons)
            if (std::abs(q.x - std::floor((*at)[0])) < 1.5f &&
                std::abs(q.y - std::floor((*at)[1])) < 1.5f &&
                same_colour(q, army[0] * 0.5f, army[1] * 0.5f, army[2] * 0.5f))
                dark_icon = true;
    const Quad* dot = minimap_dot(f, r, static_cast<f32>(ctx.sim.terrain()->map_width()),
                                  static_cast<f32>(ctx.sim.terrain()->map_height()), gen_pos);
    const bool dark_dot = dot && same_colour(*dot, army[0] * 0.5f, army[1] * 0.5f, army[2] * 0.5f);
    t.check(gone_from_world && still_drawn && dark_icon && dark_dot && r.recon().maybe_dead(gen),
            fmt::format("Test 6: dead unseen, the power generator stays (gone from the world: "
                        "{}, drawn: {}, darkened icon: {}, darkened dot: {})",
                        gone_from_world, still_drawn, dark_icon, dark_dot));

    // Test 7: seen, the spot is empty: it's gone.
    r.camera().set_eye_distance(110.0f);
    scry = {gen_at};
    f = next();
    t.check(!drawn_at(f, gen_at) && !r.recon().maybe_dead(gen),
            "Test 7: the spot seen, the power generator is gone");

    spdlog::info("Counter intel test: {}/{} passed", t.pass, t.pass + t.fail);
}

} // namespace osc::test
