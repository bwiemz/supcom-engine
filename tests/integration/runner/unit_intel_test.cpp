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
#include "render_probe.hpp"

#include "lua/lua_state.hpp"
#include "map/terrain.hpp"
#include "renderer/minimap_renderer.hpp"
#include "renderer/recon_view.hpp"
#include "sim/entity.hpp"
#include "sim/sim_state.hpp"
#include "sim/world_snapshot.hpp"

#include <lua.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace osc::test {

namespace {

/// A mesh the last frame drew: where (its model's translation), what
/// texture, and its pose's digest (empty without one).
struct Drawn {
    std::string texture;
    f32 x = 0, z = 0;
    std::string pose;
};

/// A 2D quad the last frame drew: its middle on screen, size and colour.
struct Quad {
    f32 x = 0, y = 0, w = 0, h = 0;
    f32 r = 0, g = 0, b = 0;
};

struct Frame {
    std::vector<Drawn> meshes;
    std::vector<Quad> icons;   ///< strategic icons
    std::vector<Quad> overlay; ///< health bars and the like
    std::vector<Quad> minimap; ///< the C++ HUD's minimap
};

/// The last frame's meshes, strategic icons, overlays and minimap, from its
/// render-state dump.
Frame drawn(renderer::Renderer& r) {
    std::ostringstream out;
    r.dump_frame(out);
    std::istringstream in(out.str());
    Frame frame;
    std::string section;
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line[0] == '[') {
            section = line.substr(0, line.find(']') + 1);
            continue;
        }
        std::vector<std::string> parts;
        for (size_t start = 0;;) {
            const size_t bar = line.find(" | ", start);
            parts.push_back(line.substr(start, bar == std::string::npos ? bar : bar - start));
            if (bar == std::string::npos) break;
            start = bar + 3;
        }
        if (section == "[units]" && line.rfind("mesh ", 0) == 0 && parts.size() >= 3) {
            Drawn d;
            d.texture = parts[0].substr(5);
            std::transform(d.texture.begin(), d.texture.end(), d.texture.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            std::istringstream model(parts[1]);
            std::array<f32, 16> m{};
            for (f32& v : m) model >> v;
            d.x = m[12];
            d.z = m[14];
            if (parts.size() >= 4) d.pose = parts[3];
            frame.meshes.push_back(std::move(d));
        } else if (parts.size() == 3 &&
                   (section == "[icons]" || section == "[overlay]" || section == "[minimap-hud]")) {
            Quad q;
            std::istringstream rect(parts[0]);
            rect >> q.x >> q.y >> q.w >> q.h;
            q.x += q.w * 0.5f;
            q.y += q.h * 0.5f;
            std::istringstream colour(parts[2]);
            colour >> q.r >> q.g >> q.b;
            (section == "[icons]"     ? frame.icons
             : section == "[overlay]" ? frame.overlay
                                      : frame.minimap)
                .push_back(q);
        }
    }
    return frame;
}

/// The mesh drawn at (x, z).
const Drawn* mesh_at(const Frame& frame, f32 x, f32 z) {
    for (const Drawn& d : frame.meshes)
        if (std::abs(d.x - x) < 1.0f && std::abs(d.z - z) < 1.0f) return &d;
    return nullptr;
}

/// How many meshes of texture `texture` were drawn within `radius` of x.
int meshes_near(const Frame& frame, const std::string& texture, f32 x, f32 radius) {
    return static_cast<int>(
        std::count_if(frame.meshes.begin(), frame.meshes.end(), [&](const Drawn& d) {
            return d.texture.find(texture) != std::string::npos && std::abs(d.x - x) < radius;
        }));
}

struct Spot {
    f32 x = 0, z = 0;
};

/// The spot, on a 16-unit lattice, farthest from every unit whose box
/// [x - 30, x + 50] x [z - 40, z + 70] is dry land; none within 60 of a
/// unit will do.
std::optional<Spot> quiet_spot(sim::SimState& sim) {
    const map::Terrain& t = *sim.terrain();
    const i32 w = static_cast<i32>(t.map_width());
    const i32 h = static_cast<i32>(t.map_height());
    std::vector<Spot> units;
    sim.entity_registry().for_each([&](const sim::Entity& e) {
        if (e.is_unit() && !e.destroyed()) units.push_back({e.position().x, e.position().z});
    });
    std::optional<Spot> best;
    f32 best_d2 = 60.0f * 60.0f;
    for (i32 gz = 80; gz < h - 80; gz += 16) {
        for (i32 gx = 80; gx < w - 80; gx += 16) {
            const f32 x = static_cast<f32>(gx);
            const f32 z = static_cast<f32>(gz);
            f32 near2 = std::numeric_limits<f32>::max();
            for (const Spot& u : units)
                near2 = std::min(near2, (u.x - x) * (u.x - x) + (u.z - z) * (u.z - z));
            if (near2 < best_d2) continue;
            bool dry = true;
            for (i32 dz = -40; dz <= 70 && dry; dz += 8)
                for (i32 dx = -30; dx <= 50 && dry; dx += 8)
                    dry = !t.has_water() ||
                          t.get_terrain_height(x + static_cast<f32>(dx), z + static_cast<f32>(dz)) >
                              t.water_elevation() + 0.5f;
            if (!dry) continue;
            best = Spot{x, z};
            best_d2 = near2;
        }
    }
    return best;
}

const char* name(renderer::Sight s) {
    switch (s) {
    case renderer::Sight::Hidden: return "hidden";
    case renderer::Sight::Blip: return "blip";
    case renderer::Sight::SeenBlip: return "seen blip";
    case renderer::Sight::Seen: return "seen";
    case renderer::Sight::Remembered: return "remembered";
    }
    return "?";
}

} // namespace

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

    const auto lua = [&](const std::string& code) {
        const auto ran = ctx.lua_state.do_string(code);
        if (!ran) spdlog::warn("unit intel test Lua: {}", ran.error().message);
    };
    const auto spawn = [&](const char* global, const char* bp, const char* army, Spot at) {
        lua(fmt::format("{0} = CreateUnitHPR('{1}', '{2}', {3}, {4}, {5}, 0, 0, 0)\n"
                        "__osc_intel_id = tonumber({0}:GetEntityId())\n",
                        global, bp, army, at.x, ctx.sim.terrain()->get_terrain_height(at.x, at.z),
                        at.z));
        lua_pushstring(ctx.L, "__osc_intel_id");
        lua_rawget(ctx.L, LUA_GLOBALSINDEX);
        const u32 id = lua_isnumber(ctx.L, -1) ? static_cast<u32>(lua_tonumber(ctx.L, -1)) : 0;
        lua_pop(ctx.L, 1);
        return id;
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
    // Where `p` is on screen, as the icons and overlays project it.
    const auto project = [&](const sim::Vector3& p) -> std::optional<std::array<f32, 2>> {
        const f32 sw = static_cast<f32>(r.width());
        const f32 sh = static_cast<f32>(r.height());
        const auto vp = r.camera().view_proj(sw / sh);
        const f32 cx = vp[0] * p.x + vp[4] * p.y + vp[8] * p.z + vp[12];
        const f32 cy = vp[1] * p.x + vp[5] * p.y + vp[9] * p.z + vp[13];
        const f32 cw = vp[3] * p.x + vp[7] * p.y + vp[11] * p.z + vp[15];
        if (cw <= 0.001f) return std::nullopt;
        return std::array<f32, 2>{(cx / cw + 1.0f) * 0.5f * sw, (cy / cw + 1.0f) * 0.5f * sh};
    };
    const auto on_screen = [&](u32 id) -> std::optional<std::array<f32, 2>> {
        const sim::EntityRecord* e = seen.cur().find(id);
        if (!e) return std::nullopt;
        return project(e->position);
    };
    const auto quad_at = [](const std::vector<Quad>& quads, f32 x, f32 y, f32 w,
                            f32 h) -> const Quad* {
        for (const Quad& q : quads)
            if (std::abs(q.x - x) < 1.0f && std::abs(q.y - y) < 1.0f && q.w == w && q.h == h)
                return &q;
        return nullptr;
    };
    // The icon drawn over unit `id` (centred on it), or null.
    const auto icon_of = [&](const Frame& frame, u32 id) -> const Quad* {
        const auto at = on_screen(id);
        if (!at) return nullptr;
        for (const Quad& q : frame.icons)
            if (std::abs(q.x - (*at)[0]) < 1.0f && std::abs(q.y - (*at)[1]) < 1.0f) return &q;
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
    (void)shots.shoot(*ctx.sim.terrain(), sx, sz + 20, 150.0f);
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

} // namespace osc::test
