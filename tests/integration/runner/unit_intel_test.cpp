// --unit-intel-test (M215a): units seen through the player's intel.
//
// FA shows another army's unit only as the player's recon sees it: in line
// of sight as itself, else as a blip while radar (or sonar, or omni)
// detects it; a structure, once seen, stays drawn as it was last seen; a
// projectile shows only in line of sight; allies share their eyes. On dry
// ground away from the starts stand ARMY_2's two engineers and power
// generator, ARMY_1's power generator as a radar the test turns on and off,
// and ARMY_3's engineer (made an ally) with one of ARMY_2's beside it. Sight
// is scrying (CreateVisibleAreaAtPoint) over the near engineer and the power
// generator: vision the terrain can't block.

#include "integration_tests.hpp"
#include "intel_probe.hpp"
#include "render_probe.hpp"

#include "lua/lua_state.hpp"
#include "map/terrain.hpp"
#include "renderer/beam_renderer.hpp"
#include "renderer/input_handler.hpp"
#include "renderer/minimap_renderer.hpp"
#include "renderer/particle_system.hpp"
#include "renderer/recon_view.hpp"
#include "sim/entity.hpp"
#include "sim/sim_state.hpp"
#include "sim/world_snapshot.hpp"
#include "vfs/directory_mount.hpp"
#include "vfs/virtual_file_system.hpp"

#include <lua.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace osc::test {

void test_unit_intel(TestContext& ctx) {
    spdlog::info("=== UNIT INTEL TEST: units seen through the player's intel (M215a) ===");
    Tally t;

    if (ctx.sim.army_count() < 3) {
        t.check(false, "the map has three armies");
        return;
    }
    const auto spot = quiet_spot(ctx.sim);
    if (!spot) {
        t.check(false, "dry ground 60 from every unit");
        return;
    }
    const f32 sx = spot->x;
    const f32 sz = spot->z;
    spdlog::info("Unit intel test: the spot ({:.0f}, {:.0f})", sx, sz);

    const auto lua = [&](const std::string& code) { run_lua(ctx, code); };
    const auto spawn = [&](const char* global, const char* bp, const char* army, Spot at) {
        return spawn_unit(ctx, global, bp, army, at);
    };

    // ARMY_2's engineers at the spot ("near") and 40 east ("far"), its power
    // generator 24 south; ARMY_1's radar 20 east and 40 north, reaching 90
    // (all three); ARMY_3's engineer 20 west and 60 north, and ARMY_2's
    // beside it ("shared"), 10 on. Scrying 12 across over "near" and the
    // power generator sees their cells, not "far"'s.
    const Spot near_at{sx, sz};
    const Spot far_at{sx + 40, sz};
    const Spot pgen_at{sx, sz - 24};
    const Spot radar_at{sx + 20, sz + 40};
    const Spot ally_at{sx - 20, sz + 60};
    const Spot shared_at{sx - 10, sz + 60};
    const u32 near_id = spawn("__osc_intel_near", "uel0105", "ARMY_2", near_at);
    const u32 far_id = spawn("__osc_intel_far", "uel0105", "ARMY_2", far_at);
    const u32 pgen_id = spawn("__osc_intel_pgen", "ueb1101", "ARMY_2", pgen_at);
    (void)spawn("__osc_intel_radar", "ueb1101", "ARMY_1", radar_at);
    const u32 ally_id = spawn("__osc_intel_ally", "uel0105", "ARMY_3", ally_at);
    const u32 shared_id = spawn("__osc_intel_shared", "uel0105", "ARMY_2", shared_at);
    lua("SetAlliance('ARMY_1', 'ARMY_3', 'Ally')\n"
        "local s = __osc_intel_radar\n"
        "s:DisableIntel('Vision') s:DisableIntel('Omni')\n"
        "s:InitIntel(1, 'Radar', 90) s:DisableIntel('Radar')\n"
        // Damaged, the engineers show health bars where they're seen.
        "for _, u in {__osc_intel_near, __osc_intel_far} do\n"
        "  u:SetHealth(u, u:GetMaxHealth() * 0.5)\n"
        "end\n");

    // A shell of `unit`'s army hanging 6 above it, 4 east or (dx, dz) off.
    lua("function __osc_intel_shell(unit, dx, dz)\n"
        "  local p = unit:CreateProjectile('/projectiles/TDFGauss01/TDFGauss01_proj.bp',"
        " dx or 4, 6, dz or 0, nil, nil, nil)\n"
        "  p:SetVelocity(0, 0, 0) p:SetBallisticAcceleration(0) p:SetLifetime(100)\n"
        "  p:SetCollision(false)\n"
        "end\n");

    bool vision = false;
    const auto senses = [&](bool sight, bool radar) {
        vision = sight;
        lua(fmt::format("__osc_intel_radar:{}Intel('Radar')\n", radar ? "Enable" : "Disable"));
    };

    OffscreenShots shots(ctx);
    if (!shots.ok()) {
        t.check(false, "renderer init (no Vulkan?)");
        return;
    }
    renderer::Renderer& r = shots.renderer();
    r.set_fog_enabled(true);
    r.set_player_army(0);

    // The world `ticks` on (scrying, if the sight is on, each tick), drawn
    // as the game's next frame draws it.
    sim::WorldHistory seen;
    const auto next = [&](int ticks = 1) {
        for (int i = 0; i < ticks; ++i) {
            if (vision)
                for (const Spot& at : {near_at, pgen_at})
                    lua(fmt::format("CreateVisibleAreaAtPoint(1, {}, 0, {}, 12, 0.1)\n", at.x,
                                    at.z));
            ctx.sim.tick();
        }
        shots.recapture();
        shots.redraw();
        seen.capture(ctx.sim);
        return drawn(r);
    };
    const auto sight = [&](u32 id) {
        const sim::EntityRecord* e = seen.cur().find(id);
        return e ? r.recon().sight(*e) : renderer::Sight::Hidden;
    };
    const auto project = [&](const sim::Vector3& p) { return screen_of(r, p); };
    const auto on_screen = [&](u32 id) -> std::optional<std::array<f32, 2>> {
        const sim::EntityRecord* e = seen.cur().find(id);
        if (!e) return std::nullopt;
        return project(e->position);
    };
    // The icon drawn over unit `id` (centred on it), or null.
    const auto icon_of = [&](const Frame& frame, u32 id) -> const Quad* {
        const auto at = on_screen(id);
        if (!at) return nullptr;
        for (const Quad& q : frame.icons)
            // Icons sit at the floored projection (RenderUnitIcon).
            if (std::abs(q.x - std::floor((*at)[0])) < 1.0f &&
                std::abs(q.y - std::floor((*at)[1])) < 1.0f)
                return &q;
        return nullptr;
    };
    // Whether unit `id` has a health bar (its 40 x 4 back, 20 above it).
    const auto has_bar = [&](const Frame& frame, u32 id) {
        const auto at = on_screen(id);
        return at && quad_at(frame.overlay, (*at)[0], (*at)[1] - 18.0f, 40.0f, 4.0f);
    };
    // The minimap's dot for unit `id`, or null.
    const auto dot_of = [&](const Frame& frame, u32 id) -> const Quad* {
        const sim::EntityRecord* e = seen.cur().find(id);
        if (!e) return nullptr;
        const f32 size = static_cast<f32>(renderer::MinimapRenderer::MINIMAP_SIZE);
        const f32 margin = static_cast<f32>(renderer::MinimapRenderer::MINIMAP_MARGIN);
        const f32 map_w = static_cast<f32>(ctx.sim.terrain()->map_width());
        const f32 map_h = static_cast<f32>(ctx.sim.terrain()->map_height());
        const renderer::MapArea area = renderer::fit_map_area(
            margin, static_cast<f32>(r.height()) - size - margin, size, size, map_w, map_h);
        return quad_at(frame.minimap, area.x + e->position.x / map_w * area.w,
                       area.y + e->position.z / map_h * area.h, 3.0f, 3.0f);
    };
    const auto unidentified = [&](const Quad* i) {
        const auto [ur, ug, ub] = r.recon().unidentified_rgb();
        return i && std::abs(i->r - ur) < 0.01f && std::abs(i->g - ug) < 0.01f &&
               std::abs(i->b - ub) < 0.01f;
    };
    const auto colour = [](const Quad* i) {
        return i ? fmt::format("{:.2f} {:.2f} {:.2f}", i->r, i->g, i->b) : std::string("none");
    };

    // A shell of ARMY_3's, out over the fog, clear of its engineer's sight.
    const Spot ally_shell_at{ally_at.x + 50, ally_at.z - 40};
    lua("__osc_intel_shell(__osc_intel_ally, 50, -40)\n");

    // Test 1: no sense on them: none of ARMY_2's there draws, mesh or icon.
    // (The camera stays inside the units' IconFadeInZoom, 130: a unit drawn
    // as itself has no icon there, a blip has.)
    (void)shots.shoot(*ctx.sim.terrain(), sx, sz + 20, 110.0f);
    Frame f = next();
    t.check(!mesh_at(f, near_at.x, near_at.z) && !mesh_at(f, far_at.x, far_at.z) &&
                !mesh_at(f, pgen_at.x, pgen_at.z) && !icon_of(f, near_id) && !icon_of(f, far_id) &&
                !icon_of(f, pgen_id) && !has_bar(f, near_id) && !has_bar(f, far_id) &&
                !dot_of(f, near_id) && !dot_of(f, far_id) && !dot_of(f, pgen_id),
            fmt::format("Test 1: ARMY_2's in the fog draw nothing: mesh, icon, health bar or "
                        "minimap dot (near {}, far {}, power generator {})",
                        name(sight(near_id)), name(sight(far_id)), name(sight(pgen_id))));

    // Test 2: ARMY_3's engineer draws, an ally's, and so, in its sight (the
    // sim shares an ally's), does ARMY_2's beside it. ARMY_3's shell draws
    // where no one sees: an ally's shows anywhere.
    t.check(mesh_at(f, ally_at.x, ally_at.z) && mesh_at(f, shared_at.x, shared_at.z) &&
                dot_of(f, ally_id) && dot_of(f, shared_id) &&
                mesh_at(f, ally_shell_at.x, ally_shell_at.z),
            fmt::format("Test 2: the ally's engineer and shell draw, and the enemy's engineer "
                        "in its sight (ally {}, shared {}, shell {})",
                        name(sight(ally_id)), name(sight(shared_id)),
                        mesh_at(f, ally_shell_at.x, ally_shell_at.z) ? "drawn" : "not drawn"));

    // Test 3: in sight, the near engineer and the power generator draw; the
    // far engineer, beyond it, doesn't.
    senses(true, false);
    f = next();
    const Drawn* pgen_seen = mesh_at(f, pgen_at.x, pgen_at.z);
    t.check(mesh_at(f, near_at.x, near_at.z) && pgen_seen && !mesh_at(f, far_at.x, far_at.z) &&
                !icon_of(f, near_id) && !icon_of(f, far_id) && has_bar(f, near_id) &&
                !has_bar(f, far_id) && dot_of(f, near_id) && !dot_of(f, far_id),
            fmt::format("Test 3: in sight, ARMY_2's draw, with the damaged engineer's health "
                        "bar and a minimap dot (near {}, power generator {}), beyond it not "
                        "(far {})",
                        name(sight(near_id)), name(sight(pgen_id)), name(sight(far_id))));

    // Test 4: a projectile of ARMY_2's draws in sight, not on radar alone.
    senses(true, true);
    lua("__osc_intel_shell(__osc_intel_near) __osc_intel_shell(__osc_intel_far)\n");
    f = next();
    const int near_shots = meshes_near(f, "tdfgauss01", near_at.x + 4, 8.0f);
    const int far_shots = meshes_near(f, "tdfgauss01", far_at.x + 4, 8.0f);
    t.check(near_shots == 1 && far_shots == 0,
            fmt::format("Test 4: ARMY_2's projectile draws in sight ({}), not on radar ({})",
                        near_shots, far_shots));
    // The power generator's pose the last tick it was in sight: it turns by
    // itself, so this one, not Test 3's.
    const Drawn* pgen_last = mesh_at(f, pgen_at.x, pgen_at.z);
    const std::string pose_seen = pgen_last ? pgen_last->pose : "";

    // Test 5: sight lost, radar on: the near engineer, seen, is a blip in
    // its army's colour; the far one, never seen, one in UnidentifiedColor.
    senses(false, true);
    f = next();
    {
        const Quad* near_icon = icon_of(f, near_id);
        const Quad* far_icon = icon_of(f, far_id);
        t.check(!mesh_at(f, near_at.x, near_at.z) && !mesh_at(f, far_at.x, far_at.z) && near_icon &&
                    !unidentified(near_icon) && unidentified(far_icon) && !has_bar(f, near_id) &&
                    !has_bar(f, far_id) && dot_of(f, near_id) &&
                    !unidentified(dot_of(f, near_id)) && unidentified(dot_of(f, far_id)),
                fmt::format("Test 5: on radar, blips, icon and minimap dot, without health "
                            "bars: the seen one in its colour ({}), the other unidentified ({})",
                            colour(near_icon), colour(far_icon)));
    }

    // Test 6: the power generator, seen, still draws, as it was seen, with
    // no icon.
    {
        const Drawn* pgen_radar = mesh_at(f, pgen_at.x, pgen_at.z);
        t.check(
            pgen_radar && sight(pgen_id) == renderer::Sight::Remembered &&
                pgen_radar->pose == pose_seen && !icon_of(f, pgen_id),
            fmt::format("Test 6: the power generator is remembered ({})", name(sight(pgen_id))));
    }

    // Test 7: out of every sense, the power generator turns: it draws as
    // it was seen. The near engineer is gone.
    senses(false, false);
    lua("local p = __osc_intel_pgen\n"
        "local bone = p:GetBoneCount() > 1 and p:GetBoneName(1) or 0\n"
        "__osc_intel_turn = CreateRotator(p, bone, 'y', 90, 1000, 1000)\n");
    f = next(5);
    {
        const Drawn* pgen_dark = mesh_at(f, pgen_at.x, pgen_at.z);
        t.check(pgen_dark && pgen_dark->pose == pose_seen &&
                    sight(pgen_id) == renderer::Sight::Remembered &&
                    !mesh_at(f, near_at.x, near_at.z) && !icon_of(f, near_id),
                fmt::format("Test 7: with no sense on them, the power generator draws as seen "
                            "({}), the engineer not at all ({})",
                            name(sight(pgen_id)), name(sight(near_id))));
    }

    // Test 8: on radar again, the engineer is a blip never seen: Moho drops
    // a mobile unit's blip, and what was seen of it, with the last sense.
    senses(false, true);
    f = next();
    t.check(sight(near_id) == renderer::Sight::Blip && unidentified(icon_of(f, near_id)),
            fmt::format("Test 8: found again, the engineer is unidentified ({}, icon {})",
                        name(sight(near_id)), colour(icon_of(f, near_id))));

    // Test 9: seen again, the power generator draws as it is now: turned.
    senses(true, false);
    f = next();
    {
        const Drawn* pgen_again = mesh_at(f, pgen_at.x, pgen_at.z);
        t.check(pgen_again && sight(pgen_id) == renderer::Sight::Seen &&
                    pgen_again->pose != pose_seen,
                fmt::format("Test 9: seen again, the power generator has turned ({})",
                            name(sight(pgen_id))));
    }

    // Test 10: an observer sees everything, projectiles too.
    senses(false, false);
    lua("__osc_intel_shell(__osc_intel_far)\n");
    r.set_player_army(-1);
    f = next();
    const int observed_shots = meshes_near(f, "tdfgauss01", far_at.x + 4, 8.0f);
    t.check(mesh_at(f, near_at.x, near_at.z) && mesh_at(f, far_at.x, far_at.z) &&
                mesh_at(f, pgen_at.x, pgen_at.z) && observed_shots >= 1 && !icon_of(f, near_id) &&
                !icon_of(f, far_id),
            fmt::format("Test 10: an observer sees everything ({} of the far engineer's "
                        "projectiles)",
                        observed_shots));
    r.set_player_army(0);

    // Test 11: with the fog of war off (--no-fog), everything draws too.
    r.set_fog_enabled(false);
    f = next();
    t.check(mesh_at(f, near_at.x, near_at.z) && mesh_at(f, far_at.x, far_at.z) &&
                !icon_of(f, far_id),
            fmt::format("Test 11: with the fog off, ARMY_2's engineers draw (near {}, far {})",
                        name(sight(near_id)), name(sight(far_id))));
    r.set_fog_enabled(true);

    // Test 12: a death's flash shows where the player's army sees: one at
    // the engineer beside the ally's flashes, one at the far engineer, in
    // the fog, doesn't. (The events are the test's: the sim clears its own
    // as its tick ends, before recapture() takes the world.)
    {
        const sim::EntityRecord* shared = seen.cur().find(shared_id);
        const sim::EntityRecord* far = seen.cur().find(far_id);
        if (!shared || !far) {
            t.check(false, "Test 12: the engineers are there");
        } else {
            for (const sim::EntityRecord* e : {shared, far})
                shots.events().deaths.push_back(
                    {e->position.x, e->position.y, e->position.z, 1.0f, e->army});
            shots.redraw();
            f = drawn(r);
            const auto flash_at = [&](const sim::Vector3& p) {
                const auto at = project(p);
                return at && std::any_of(f.overlay.begin(), f.overlay.end(), [&](const Quad& q) {
                           return std::abs(q.x - (*at)[0]) < 1.0f &&
                                  std::abs(q.y - (*at)[1]) < 1.0f && std::abs(q.r - 1.0f) < 0.01f &&
                                  std::abs(q.g - 0.8f) < 0.01f && std::abs(q.b - 0.3f) < 0.01f;
                       });
            };
            const bool in_sight = flash_at(shared->position);
            const bool in_fog = flash_at(far->position);
            t.check(in_sight && !in_fog,
                    fmt::format("Test 12: a death flashes in sight ({}), not in the fog ({})",
                                in_sight ? "flash" : "none", in_fog ? "flash" : "none"));
        }
    }

    spdlog::info("Unit intel test: {}/{} passed", t.pass, t.pass + t.fail);
}

// --effect-intel-test (M215b): effects, beams, shields and clicks through
// the player's intel.
//
// Moho shows an EmitIfVisible emitter's particles only while the focus
// army sees the emitter (CEfxEmitter::CanSeeCam: LOSNow there, whoever made
// it), makes a CreateIfVisible one only if it sees it made, draws a beam
// where it sees either end (CEfxBeam), and another army's shield through its
// intel. A click can't pick what the player's intel hides. On dry ground
// away from the starts stand ARMY_2's engineers ("seen", under scrying, and
// "fog" 40 east, with "fog2" beside it) and shield generator, and ARMY_1's
// engineer and power generator (a radar the test turns on).
void test_effect_intel(TestContext& ctx) {
    spdlog::info("=== EFFECT INTEL TEST: effects, beams, shields and clicks (M215b) ===");
    Tally t;

    const auto spot = quiet_spot(ctx.sim);
    if (!spot) {
        t.check(false, "dry ground 60 from every unit");
        return;
    }
    const f32 sx = spot->x;
    const f32 sz = spot->z;
    spdlog::info("Effect intel test: the spot ({:.0f}, {:.0f})", sx, sz);

    // Two copies of a steady emitter (aeon_build_01: a ring a tick, for
    // ever, EmitIfVisible), the second CreateIfVisible too.
    constexpr const char* kRoot = "/osc_effect_intel";
    const auto dir = std::filesystem::temp_directory_path() / "osc_effect_intel";
    std::filesystem::create_directories(dir);
    {
        const auto source = ctx.vfs.read_file("/effects/emitters/aeon_build_01_emit.bp");
        if (!source) {
            t.check(false, "aeon_build_01_emit.bp");
            return;
        }
        std::string text(source->begin(), source->end());
        std::ofstream(dir / "steady_emit.bp") << text;
        const std::string off = "CreateIfVisible = false";
        const size_t at = text.find(off);
        if (at == std::string::npos) {
            t.check(false, "aeon_build_01_emit.bp says CreateIfVisible");
            return;
        }
        text.replace(at, off.size(), "CreateIfVisible = true");
        std::ofstream(dir / "steady_create_emit.bp") << text;
    }
    ctx.vfs.mount(kRoot, std::make_unique<vfs::DirectoryMount>(dir));
    const std::string steady = std::string(kRoot) + "/steady_emit.bp";
    const std::string create = std::string(kRoot) + "/steady_create_emit.bp";

    const Spot seen_at{sx, sz};
    const Spot fog_at{sx + 40, sz};
    const Spot fog2_at{sx + 40, sz + 30};
    const Spot shield_at{sx, sz + 60};
    const Spot own_at{sx - 30, sz};
    const Spot radar_at{sx + 20, sz - 40};
    const u32 seen_id = spawn_unit(ctx, "__osc_fx_seen", "uel0105", "ARMY_2", seen_at);
    const u32 fog_id = spawn_unit(ctx, "__osc_fx_fog", "uel0105", "ARMY_2", fog_at);
    (void)spawn_unit(ctx, "__osc_fx_fog2", "uel0105", "ARMY_2", fog2_at);
    const u32 shield_id = spawn_unit(ctx, "__osc_fx_shield", "ueb4202", "ARMY_2", shield_at);
    const u32 own_id = spawn_unit(ctx, "__osc_fx_own", "uel0105", "ARMY_1", own_at);
    (void)spawn_unit(ctx, "__osc_fx_radar", "ueb1101", "ARMY_1", radar_at);
    run_lua(ctx, "local s = __osc_fx_radar\n"
                 "s:DisableIntel('Vision') s:DisableIntel('Omni')\n"
                 "s:InitIntel(1, 'Radar', 90) s:DisableIntel('Radar')\n"
                 "__osc_fx_own:DisableIntel('Vision')\n");
    // Its shield comes up once it's finished.
    for (int i = 0; i < 20; ++i) ctx.sim.tick();

    OffscreenShots shots(ctx);
    if (!shots.ok()) {
        t.check(false, "renderer init (no Vulkan?)");
        return;
    }
    renderer::Renderer& r = shots.renderer();
    r.set_fog_enabled(true);
    r.set_player_army(0);

    std::vector<Spot> scry = {seen_at};
    sim::WorldHistory seen;
    // The world `ticks` on, each drawn for its 6 frames (particles emit
    // once a tick, M214c).
    const auto next = [&](int ticks = 1) {
        for (int i = 0; i < ticks; ++i) {
            for (const Spot& at : scry)
                run_lua(ctx, fmt::format("CreateVisibleAreaAtPoint(1, {}, 0, {}, 12, 0.1)\n", at.x,
                                         at.z));
            ctx.sim.tick();
            shots.recapture();
            for (int frame = 0; frame < 6; ++frame) shots.redraw();
        }
        seen.capture(ctx.sim);
        return drawn(r);
    };
    // The effect id of the emitter the renderer made for unit `id`'s effect
    // of blueprint `bp`, or 0 (none made, or no such effect).
    const auto emitter_of = [&](u32 id, const std::string& bp) -> u32 {
        for (const sim::EffectRecord& fx : seen.cur().effects) {
            if (fx.entity_id != id || fx.blueprint_path != bp) continue;
            if (r.particle_system().draws_effect(fx.id)) return fx.id;
        }
        return 0;
    };
    const auto has_effect = [&](u32 id, const std::string& bp) {
        return std::any_of(seen.cur().effects.begin(), seen.cur().effects.end(),
                           [&](const sim::EffectRecord& fx) {
                               return fx.entity_id == id && fx.blueprint_path == bp;
                           });
    };
    // The particles effect `fx` has out this frame.
    const auto particles = [&](u32 fx) {
        const auto& all = r.particle_system().drawn();
        return fx ? static_cast<size_t>(
                        std::count_if(all.begin(), all.end(),
                                      [fx](const renderer::ParticleSystem::Drawn& d) {
                                          return d.effect_id == fx;
                                      }))
                  : size_t{0};
    };
    // The overlay's dot for an effect on unit `id` (4 across, where it is).
    const auto dot_on = [&](const Frame& f, u32 id) {
        const sim::EntityRecord* e = seen.cur().find(id);
        const auto at = e ? screen_of(r, e->position) : std::nullopt;
        return at && quad_at(f.overlay, (*at)[0], (*at)[1], 4.0f, 4.0f) != nullptr;
    };

    // Test 1: the steady emitter emits where the player's army sees it,
    // not in the fog; the CreateIfVisible one is made only in sight.
    // (And one of a blueprint it can't read, which the overlay marks.)
    run_lua(ctx, fmt::format("for _, u in {{__osc_fx_seen, __osc_fx_fog}} do\n"
                             "  CreateEmitterOnEntity(u, 2, '{}')\n"
                             "  CreateEmitterOnEntity(u, 2, '{}')\n"
                             "  CreateEmitterOnEntity(u, 2, '{}/no_such_emit.bp')\n"
                             "end\n",
                             steady, create, kRoot));
    (void)shots.shoot(*ctx.sim.terrain(), sx, sz + 20, 150.0f);
    // The emitters emit only within their LODCutoff (100) of the camera
    // (M214c): look from 80, between the two units.
    r.camera().set_target(sx + 20, sz);
    r.camera().set_eye_distance(80.0f);
    Frame f = next(3);
    const bool made = has_effect(seen_id, steady) && has_effect(fog_id, steady) &&
                      has_effect(seen_id, create) && has_effect(fog_id, create);
    t.check(made && particles(emitter_of(seen_id, steady)) > 0 && emitter_of(fog_id, steady) &&
                particles(emitter_of(fog_id, steady)) == 0,
            fmt::format("Test 1: the emitter emits in sight ({} particles), not in the fog ({})",
                        particles(emitter_of(seen_id, steady)),
                        particles(emitter_of(fog_id, steady))));
    t.check(made && emitter_of(seen_id, create) && !emitter_of(fog_id, create),
            fmt::format("Test 2: a CreateIfVisible emitter is made in sight ({}), not in the fog "
                        "({})",
                        emitter_of(seen_id, create) ? "made" : "not made",
                        emitter_of(fog_id, create) ? "made" : "not made"));
    t.check(dot_on(f, seen_id) && !dot_on(f, fog_id),
            fmt::format("Test 3: the overlay marks an effect it can't draw in sight ({}), not "
                        "in the fog ({})",
                        dot_on(f, seen_id) ? "marked" : "not",
                        dot_on(f, fog_id) ? "marked" : "not"));

    // Test 4: seen at last, the steady emitter emits; the CreateIfVisible
    // one, unseen when made, stays unmade.
    scry.push_back(fog_at);
    f = next(3);
    t.check(particles(emitter_of(fog_id, steady)) > 0 && !emitter_of(fog_id, create) &&
                has_effect(fog_id, create),
            fmt::format("Test 4: in sight, the fog's emitter emits ({}); the CreateIfVisible one "
                        "stays unmade ({})",
                        particles(emitter_of(fog_id, steady)),
                        emitter_of(fog_id, create) ? "made" : "not made"));
    scry.pop_back();
    r.camera().set_target(sx, sz + 20);
    r.camera().set_eye_distance(150.0f);

    // Test 5: a beam between two of ARMY_2's in the fog doesn't draw; one
    // with an end in sight does: as FA draws it (a BeamBlueprint's, M214a)
    // and as the overlay's placeholder (a blueprint it can't read).
    const auto placeholders = [&](const Frame& fr) {
        return std::count_if(fr.overlay.begin(), fr.overlay.end(),
                             [](const Quad& q) { return same_colour(q, 0.8f, 0.9f, 1.0f); });
    };
    const auto fa_beams = [&] {
        return std::count_if(r.beam_renderer().drawn().begin(), r.beam_renderer().drawn().end(),
                             [](const renderer::BeamRenderer::Drawn& b) {
                                 return b.blueprint == "/effects/emitters/build_beam_01_emit.bp";
                             });
    };
    run_lua(ctx, "CreateBeamEntityToEntity(__osc_fx_fog, -1, __osc_fx_fog2, -1, 2, "
                 "'/effects/emitters/build_beam_01_emit.bp')\n"
                 "CreateBeamEntityToEntity(__osc_fx_fog, -1, __osc_fx_fog2, -1, 2, "
                 "'/effects/emitters/no_such_beam.bp')\n");
    f = next();
    const auto fog_beams = fa_beams();
    const auto fog_placeholders = placeholders(f);
    run_lua(ctx, "CreateBeamEntityToEntity(__osc_fx_seen, -1, __osc_fx_fog, -1, 2, "
                 "'/effects/emitters/build_beam_01_emit.bp')\n"
                 "CreateBeamEntityToEntity(__osc_fx_seen, -1, __osc_fx_fog, -1, 2, "
                 "'/effects/emitters/no_such_beam.bp')\n");
    f = next();
    const auto half_seen_beams = fa_beams();
    const auto half_seen_placeholders = placeholders(f);
    t.check(fog_beams == 0 && half_seen_beams == 1 && fog_placeholders == 0 &&
                half_seen_placeholders == 1,
            fmt::format("Test 5: a beam draws with an end in sight ({} drawn, {} placeholders), "
                        "not in the fog ({}, {})",
                        half_seen_beams, half_seen_placeholders, fog_beams, fog_placeholders));

    // Test 6: a light, and a beam fixed to a
    // unit, show where the player's army sees them.
    const auto count_colour = [](const Frame& fr, f32 cr, f32 cg, f32 cb) {
        return std::count_if(fr.overlay.begin(), fr.overlay.end(),
                             [&](const Quad& q) { return same_colour(q, cr, cg, cb); });
    };
    run_lua(ctx, "CreateLightParticle(__osc_fx_fog, -1, 2, 4, 100, 'glow_03', 'ramp_flare_02')\n"
                 "CreateAttachedBeam(__osc_fx_fog, -1, 2, 5, 1, "
                 "'/effects/emitters/build_beam_01_emit.bp')\n");
    f = next();
    const auto fog_lights = count_colour(f, 1.0f, 0.9f, 0.5f);
    const auto fog_attached = count_colour(f, 0.6f, 0.8f, 1.0f);
    run_lua(ctx, "CreateLightParticle(__osc_fx_seen, -1, 2, 4, 100, 'glow_03', 'ramp_flare_02')\n"
                 "CreateAttachedBeam(__osc_fx_seen, -1, 2, 5, 1, "
                 "'/effects/emitters/build_beam_01_emit.bp')\n");
    f = next();
    const auto seen_lights = count_colour(f, 1.0f, 0.9f, 0.5f);
    const auto seen_attached = count_colour(f, 0.6f, 0.8f, 1.0f);
    t.check(fog_lights == 0 && fog_attached == 0 && seen_lights == 1 && seen_attached == 1,
            fmt::format("Test 6: a light and an attached beam draw in sight ({}, {}), not in the "
                        "fog ({}, {})",
                        seen_lights, seen_attached, fog_lights, fog_attached));

    // Test 7: ARMY_2's shield draws where the player's army sees it.
    const sim::EntityRecord* gen = seen.cur().find(shield_id);
    const bool shield_up =
        std::any_of(seen.cur().entities.begin(), seen.cur().entities.end(), [&](const auto& e) {
            return e.is_shield && e.shield_owner_id == shield_id && e.shield_on;
        });
    // The ring's colour: the army's, or the overlay's blue without one.
    const sim::ArmyRecord* army2 = seen.cur().army(1);
    const bool coloured = army2 && army2->has_color;
    const f32 ring_r = coloured ? army2->r / 255.0f : 0.5f;
    const f32 ring_g = coloured ? army2->g / 255.0f : 0.5f;
    const f32 ring_b = coloured ? army2->b / 255.0f : 0.8f;
    const auto rings = [&](const Frame& fr) {
        return static_cast<long>(
            std::count_if(fr.overlay.begin(), fr.overlay.end(), [&](const Quad& q) {
                return same_colour(q, ring_r, ring_g, ring_b) && q.w > 4.0f;
            }));
    };
    const long fog_rings = rings(f);
    scry.push_back(shield_at);
    f = next();
    const long seen_rings = rings(f);
    scry.pop_back();
    t.check(gen && shield_up && fog_rings == 0 && seen_rings >= 8,
            fmt::format("Test 7: the shield draws in sight ({} segments), not in the fog ({}); "
                        "shield {}",
                        seen_rings, fog_rings, shield_up ? "up" : "down"));

    // Test 8: a click can't pick the engineer in the fog; on radar, a blip,
    // it can.
    renderer::InputHandler input;
    input.set_player_army(0);
    input.set_recon(&r.recon());
    const auto click = [&](bool order_mode) -> u32 {
        input.set_frame_view(sim::FrameView(&seen.prev(), &seen.cur(), 1.0f));
        input.set_selected({own_id});
        if (order_mode) {
            const auto issued = input.click_in_command_mode(
                ctx.sim, renderer::CommandMode{"order", "RULEUCC_Capture"}, fog_at.x, fog_at.z,
                false);
            return issued ? issued->target_id : 0;
        }
        for (const auto& c : input.right_click_at(ctx.sim, fog_at.x, fog_at.z, false))
            if (c.target_id != 0) return c.target_id;
        return 0;
    };
    f = next();
    const u32 fog_right = click(false);
    const u32 fog_order = click(true);
    run_lua(ctx, "__osc_fx_radar:EnableIntel('Radar')\n");
    f = next();
    const u32 blip_right = click(false);
    const u32 blip_order = click(true);
    // (In the fog a right-click may still land on a rock, to reclaim.)
    t.check(fog_right != fog_id && fog_order == 0 && blip_right == fog_id && blip_order == fog_id,
            fmt::format("Test 8: clicks pick the engineer on radar (right-click {}, order {}), "
                        "not in the fog ({}, {})",
                        blip_right, blip_order, fog_right, fog_order));

    spdlog::info("Effect intel test: {}/{} passed", t.pass, t.pass + t.fail);
}

} // namespace osc::test
