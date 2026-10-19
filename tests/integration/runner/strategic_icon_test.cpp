// --strategic-icon-test (M215c): FA's strategic icons.
//
// Moho draws each unit's blueprint icon (StrategicIconName's rest or
// selected texture, at the texture's own size, tinted by its army's colour)
// once the camera is out past its mesh's IconFadeInZoom, and a blip's at any
// zoom: generic (structure, land, naval, air) in UnidentifiedColor until the
// unit has been seen. The runs draw ground, air, high-priority, selected; a
// stunned unit's badge goes over its icon, as the pause badge over a
// friendly unit's paused or switched off, which zoomed in shows alone; a
// unit being built has none. On dry ground away from the starts stand
// ARMY_1's units, all senses off but a radar, and beyond them ARMY_2's, on
// that radar.

#include "integration_tests.hpp"
#include "intel_probe.hpp"
#include "render_probe.hpp"

#include "renderer/army_colors.hpp"
#include "renderer/dds_decode.hpp"
#include "renderer/dds_parser.hpp"
#include "renderer/renderer.hpp"
#include "renderer/strategic_icon_renderer.hpp"
#include "renderer/texture_cache.hpp"
#include "sim/game_colors.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/world_snapshot.hpp"
#include "vfs/virtual_file_system.hpp"

#include <lua.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace osc::test {

namespace {

/// A blueprint's icon texture in `state` (rest, selected), from its
/// StrategicIconName in the sim's __blueprints.
std::string icon_texture(TestContext& ctx, const char* bp, const char* state) {
    run_lua(ctx, fmt::format("__osc_icon_name = __blueprints['{}'].StrategicIconName or ''\n", bp));
    lua_pushstring(ctx.L, "__osc_icon_name");
    lua_rawget(ctx.L, LUA_GLOBALSINDEX);
    std::string name = lua_type(ctx.L, -1) == LUA_TSTRING ? lua_tostring(ctx.L, -1) : "";
    lua_pop(ctx.L, 1);
    for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return fmt::format("{}{}_{}.dds", renderer::StrategicIconRenderer::kIconDirectory, name, state);
}

} // namespace

void test_strategic_icons(TestContext& ctx) {
    spdlog::info("=== STRATEGIC ICON TEST: FA's strategic icons (M215c) ===");
    Tally t;

    const auto spot = quiet_spot(ctx.sim);
    if (!spot) {
        t.check(false, "dry ground 60 from every unit");
        return;
    }
    const f32 sx = spot->x;
    const f32 sz = spot->z;
    spdlog::info("Strategic icon test: the spot ({:.0f}, {:.0f})", sx, sz);

    // ARMY_1's: a tank (ground), an air scout (air), a support commander
    // (high-priority), a power generator being built, a tank to select, a
    // tank to stun, and a power generator as a radar.
    const u32 tank = spawn_unit(ctx, "__osc_ic_tank", "uel0201", "ARMY_1", {sx, sz});
    const u32 scout = spawn_unit(ctx, "__osc_ic_scout", "uea0101", "ARMY_1", {sx + 24, sz});
    const u32 sacu = spawn_unit(ctx, "__osc_ic_sacu", "uel0301", "ARMY_1", {sx - 24, sz});
    const u32 unbuilt = spawn_unit(ctx, "__osc_ic_unbuilt", "ueb1101", "ARMY_1", {sx, sz + 24});
    const u32 picked = spawn_unit(ctx, "__osc_ic_picked", "uel0201", "ARMY_1", {sx, sz - 24});
    const u32 dazed = spawn_unit(ctx, "__osc_ic_dazed", "uel0201", "ARMY_1", {sx + 24, sz - 24});
    const u32 dying = spawn_unit(ctx, "__osc_ic_dying", "uel0201", "ARMY_1", {sx - 24, sz - 24});
    const u32 off = spawn_unit(ctx, "__osc_ic_off", "uel0201", "ARMY_1", {sx + 24, sz + 24});
    const u32 paused = spawn_unit(ctx, "__osc_ic_paused", "uel0105", "ARMY_1", {sx - 24, sz + 24});
    (void)spawn_unit(ctx, "__osc_ic_radar", "ueb1101", "ARMY_1", {sx, sz + 60});
    // ARMY_2's, 24 beyond the radar (it reaches 60): an engineer, a power
    // generator, an air scout (none of them ever seen), and a tank to be
    // seen once.
    const u32 e_eng = spawn_unit(ctx, "__osc_ic_eeng", "uel0105", "ARMY_2", {sx - 20, sz + 84});
    const u32 e_gen = spawn_unit(ctx, "__osc_ic_egen", "ueb1101", "ARMY_2", {sx, sz + 84});
    const u32 e_air = spawn_unit(ctx, "__osc_ic_eair", "uea0101", "ARMY_2", {sx + 20, sz + 84});
    const u32 e_tank = spawn_unit(ctx, "__osc_ic_etank", "uel0201", "ARMY_2", {sx + 40, sz + 84});
    run_lua(ctx, "for _, u in {__osc_ic_tank, __osc_ic_scout, __osc_ic_sacu, __osc_ic_unbuilt,\n"
                 "              __osc_ic_picked, __osc_ic_dazed, __osc_ic_dying, __osc_ic_radar,\n"
                 "              __osc_ic_off, __osc_ic_paused} do\n"
                 "  u:DisableIntel('Vision') u:DisableIntel('Radar') u:DisableIntel('Omni')\n"
                 "end\n"
                 "__osc_ic_radar:InitIntel(1, 'Radar', 60)\n"
                 "__osc_ic_radar:EnableIntel('Radar')\n"
                 "__osc_ic_dazed:SetStunned(100)\n"
                 "for _, u in {__osc_ic_off, __osc_ic_etank} do\n"
                 "  u:AddToggleCap('RULEUTC_StealthToggle')\n"
                 "  u:SetScriptBit('RULEUTC_StealthToggle', true)\n"
                 "end\n"
                 "__osc_ic_paused:SetPaused(true)\n");
    if (auto* e = ctx.sim.entity_registry().find(unbuilt); e && e->is_unit())
        static_cast<sim::Unit*>(e)->set_is_being_built(true);
    if (auto* e = ctx.sim.entity_registry().find(dying); e && e->is_unit()) {
        static_cast<sim::Unit*>(e)->begin_dying();
    }
    ctx.sim.tick();

    OffscreenShots shots(ctx);
    if (!shots.ok()) {
        t.check(false, "renderer init (no Vulkan?)");
        return;
    }
    renderer::Renderer& r = shots.renderer();
    r.set_fog_enabled(true);
    r.set_player_army(0);

    const std::unordered_set<u32> selection = {picked};
    std::vector<Spot> scry;
    sim::WorldHistory seen;
    const auto next = [&](bool recon = true) {
        do {
            for (const Spot& at : scry) {
                run_lua(ctx, fmt::format("CreateVisibleAreaAtPoint(1, {}, 0, {}, 12, 0.1)\n", at.x,
                                         at.z));
            }
            ctx.sim.tick();
        } while (recon && ctx.sim.recon_army() != 0);
        shots.recapture();
        shots.redraw(&selection);
        seen.capture(ctx.sim);
        return drawn(r);
    };
    // The icons at unit `id`, in draw order (its icon, then any badge).
    const auto icons_at = [&](const Frame& f, u32 id) {
        std::vector<size_t> found;
        const sim::EntityRecord* e = seen.cur().find(id);
        const auto at = e ? screen_of(r, e->position) : std::nullopt;
        if (!at) return found;
        for (size_t i = 0; i < f.icons.size(); ++i)
            if (std::abs(f.icons[i].x - std::floor((*at)[0])) < 1.0f &&
                std::abs(f.icons[i].y - std::floor((*at)[1])) < 1.0f)
                found.push_back(i);
        return found;
    };
    // Whether unit `id`'s icon is `texture`, at its size, tinted (r, g, b).
    const auto shows = [&](const Frame& f, u32 id, const std::string& texture,
                           const std::array<f32, 3>& tint) {
        const auto at = icons_at(f, id);
        if (at.empty()) return false;
        const Quad& q = f.icons[at.front()];
        const renderer::GPUTexture* tex = r.texture_cache().get(texture);
        return q.texture == texture && tex && q.w == static_cast<f32>(tex->width & ~1u) &&
               q.h == static_cast<f32>(tex->height & ~1u) &&
               same_colour(q, tint[0], tint[1], tint[2]);
    };
    const auto army_tint = [&](i32 army) -> std::array<f32, 3> {
        const sim::ArmyRecord* a = seen.cur().army(army);
        if (a && a->has_color) return {a->r / 255.0f, a->g / 255.0f, a->b / 255.0f};
        return {renderer::ARMY_COLORS[army][0], renderer::ARMY_COLORS[army][1],
                renderer::ARMY_COLORS[army][2]};
    };
    const std::string dir = renderer::StrategicIconRenderer::kIconDirectory;

    // Test 1: out past IconFadeInZoom, ARMY_1's units show their blueprints'
    // icons, at the textures' sizes, in its colour; the selected one its
    // selected texture.
    (void)shots.shoot(*ctx.sim.terrain(), sx, sz + 30, 300.0f);
    Frame f = next();
    const auto blue = army_tint(0);
    t.check(shows(f, tank, icon_texture(ctx, "uel0201", "rest"), blue) &&
                shows(f, scout, icon_texture(ctx, "uea0101", "rest"), blue) &&
                shows(f, sacu, icon_texture(ctx, "uel0301", "rest"), blue),
            fmt::format("Test 1: the tank, scout and support commander show their own icons "
                        "({} icons in all)",
                        f.icons.size()));
    t.check(shows(f, picked, icon_texture(ctx, "uel0201", "selected"), blue),
            "Test 2: the selected tank shows its selected icon");

    // Test 3: a unit being built or dying has none.
    t.check(icons_at(f, unbuilt).empty() && icons_at(f, dying).empty(),
            fmt::format("Test 3: the power generator being built ({}) and the dying tank ({}) "
                        "have no icon",
                        icons_at(f, unbuilt).size(), icons_at(f, dying).size()));

    // Test 4: ground, air, high-priority, selected.
    {
        const auto first = [&](u32 id) {
            const auto at = icons_at(f, id);
            return at.empty() ? f.icons.size() : at.front();
        };
        // (The stunned tank, a ground unit made after the scout, still draws
        // before it.)
        const size_t g = first(tank), a = first(scout), h = first(sacu), s = first(picked);
        const size_t g2 = first(dazed);
        t.check(g < a && g2 < a && a < h && h < s && s < f.icons.size(),
                fmt::format("Test 4: ground ({}, {}), air ({}), high-priority ({}), selected ({})",
                            g, g2, a, h, s));
    }

    // Test 5: the stunned tank's badge over its icon.
    {
        const auto at = icons_at(f, dazed);
        t.check(at.size() == 2 && at[1] == at[0] + 1 &&
                    f.icons[at[1]].texture == dir + "stunned_rest.dds" &&
                    same_colour(f.icons[at[1]], 1.0f, 1.0f, 1.0f),
                fmt::format("Test 5: the stunned tank's badge over its icon ({} icons there)",
                            at.size()));
    }
    const std::string pause_badge = dir + "pause_rest.dds";
    const auto badged = [&](const Frame& frame, u32 id) {
        const auto at = icons_at(frame, id);
        return at.size() == 2 && at[1] == at[0] + 1 && frame.icons[at[1]].texture == pause_badge &&
               same_colour(frame.icons[at[1]], 1.0f, 1.0f, 1.0f);
    };
    t.check(badged(f, off) && badged(f, paused) && icons_at(f, tank).size() == 1,
            fmt::format("Test 5b: the tank with its stealth switched off and the paused engineer "
                        "have the pause badge over their icons ({} and {} there), the tank none",
                        icons_at(f, off).size(), icons_at(f, paused).size()));

    // Test 6: FA's StrategicIconPS gives the icon's grey texels the tint
    // itself and keeps the rest: inside the tank's icon, some pixels are its
    // army's colour exactly, and none a darkened copy of it (the grey times
    // the tint, as a plain multiply would draw it).
    {
        const ImageRGBA8 image = shots.grab(&selection);
        const auto at = icons_at(f, tank);
        int tinted = 0;
        int darkened = 0;
        if (!at.empty() && image.width > 0) {
            const Quad& q = f.icons[at.front()];
            for (int y = static_cast<int>(q.y - q.h / 2); y < static_cast<int>(q.y + q.h / 2); ++y)
                for (int x = static_cast<int>(q.x - q.w / 2); x < static_cast<int>(q.x + q.w / 2);
                     ++x) {
                    if (x < 0 || y < 0 || x >= static_cast<int>(image.width) ||
                        y >= static_cast<int>(image.height))
                        continue;
                    const u8* px = &image.pixels[(static_cast<size_t>(y) * image.width + x) * 4];
                    // The pixel as k times the tint, k from its brightest channel.
                    const size_t c = blue[0] >= blue[1] && blue[0] >= blue[2] ? 0
                                     : blue[1] >= blue[2]                     ? 1
                                                                              : 2;
                    const f32 k = static_cast<f32>(px[c]) / (blue[c] * 255.0f);
                    bool scaled = true;
                    for (size_t ch = 0; ch < 3; ++ch)
                        scaled = scaled &&
                                 std::abs(static_cast<f32>(px[ch]) - k * blue[ch] * 255.0f) <= 4.0f;
                    if (scaled && k > 0.97f && k < 1.03f) ++tinted;
                    if (scaled && k > 0.15f && k < 0.9f) ++darkened;
                }
        }
        t.check(tinted > 0 && darkened == 0,
                fmt::format("Test 6: the tank's icon: {} pixels its army's colour, {} a darkened "
                            "copy of it",
                            tinted, darkened));
    }

    // Test 7: ARMY_2's, on radar and never seen: generic icons in
    // UnidentifiedColor.
    const auto [ur, ug, ub] = r.recon().unidentified_rgb();
    const std::array<f32, 3> grey = {ur, ug, ub};
    t.check(shows(f, e_eng, dir + "icon_land_generic_rest.dds", grey) &&
                shows(f, e_gen, dir + "icon_structure_generic_rest.dds", grey) &&
                shows(f, e_air, dir + "icon_fighter_generic_rest.dds", grey),
            "Test 7: never-seen blips show generic land, structure and air icons, unidentified");

    // Test 8: seen once, a blip shows its own icon in its army's colour.
    scry.push_back({sx + 40, sz + 84});
    (void)next();
    scry.clear();
    f = next();
    t.check(shows(f, e_tank, icon_texture(ctx, "uel0201", "rest"), army_tint(1)),
            "Test 8: the tank seen once shows its own icon, in ARMY_2's colour");
    t.check(icons_at(f, e_tank).size() == 1,
            "Test 8b: ARMY_2's tank, its stealth switched off too, has no pause badge");

    // Test 9: zoomed in, inside IconFadeInZoom: ARMY_1's units, drawn as
    // themselves, have none; the blips keep theirs.
    r.camera().set_target(sx, sz + 42);
    r.camera().set_eye_distance(110.0f);
    f = next();
    t.check(icons_at(f, tank).empty() && icons_at(f, scout).empty() && !icons_at(f, e_eng).empty(),
            fmt::format("Test 9: zoomed in, the tank has no icon ({}), the blip has ({})",
                        icons_at(f, tank).size(), icons_at(f, e_eng).size()));
    const auto badge_alone = [&](const Frame& frame, u32 id) {
        const auto at = icons_at(frame, id);
        return at.size() == 1 && frame.icons[at[0]].texture == pause_badge;
    };
    t.check(badge_alone(f, off) && badge_alone(f, paused),
            fmt::format("Test 9b: zoomed in, the switched-off tank and the paused engineer show "
                        "the pause badge alone ({} and {} icons there)",
                        icons_at(f, off).size(), icons_at(f, paused).size()));
    run_lua(ctx, "__osc_ic_off:SetScriptBit('RULEUTC_StealthToggle', false)\n"
                 "__osc_ic_paused:SetPaused(false)\n");
    f = next();
    t.check(icons_at(f, off).empty() && icons_at(f, paused).empty(),
            "Test 9c: stealth switched back on and unpaused, zoomed in, they have none");

    // Test 10: an objective's underlay (Unit:SetStrategicUnderlay) beneath
    // the tank's icon, at its own colour; "" takes it away.
    const std::string ring = dir + "icon_objective_primary_rest.dds";
    (void)r.texture_cache().get_blocking(ring);
    run_lua(ctx, "__osc_ic_tank:SetStrategicUnderlay('ICON_Objective_Primary')\n");
    (void)shots.shoot(*ctx.sim.terrain(), sx, sz + 30, 300.0f);
    f = next();
    {
        const auto at = icons_at(f, tank);
        t.check(at.size() == 2 && at[1] == at[0] + 1 && f.icons[at[0]].texture == ring &&
                    same_colour(f.icons[at[0]], 1.0f, 1.0f, 1.0f) &&
                    f.icons[at[1]].texture == icon_texture(ctx, "uel0201", "rest") &&
                    same_colour(f.icons[at[1]], blue[0], blue[1], blue[2]),
                fmt::format("Test 10: the objective ring beneath the tank's icon ({} there)",
                            at.size()));
    }
    run_lua(ctx, "__osc_ic_tank:SetStrategicUnderlay('')\n");
    f = next();
    t.check(icons_at(f, tank).size() == 1, "Test 11: SetStrategicUnderlay('') takes it away");

    r.camera().set_target(sx, sz + 42);
    r.camera().set_eye_distance(110.0f);
    f = next();
    const f32 map_w = static_cast<f32>(ctx.sim.terrain()->map_width());
    const f32 map_h = static_cast<f32>(ctx.sim.terrain()->map_height());
    const f32 mm_size = static_cast<f32>(renderer::MinimapRenderer::MINIMAP_SIZE);
    const f32 mm_margin = static_cast<f32>(renderer::MinimapRenderer::MINIMAP_MARGIN);
    const renderer::MapArea area =
        renderer::fit_map_area(mm_margin, static_cast<f32>(r.height()) - mm_size - mm_margin,
                               mm_size, mm_size, map_w, map_h);
    const auto minimap_icons_at = [&](u32 id) {
        std::vector<const renderer::UIQuad*> found;
        const sim::EntityRecord* e = seen.cur().find(id);
        if (!e) {
            return found;
        }
        const f32 x = std::floor(area.x + e->position.x / map_w * area.w);
        const f32 y = std::floor(area.y + e->position.z / map_h * area.h);
        for (const renderer::UIQuad& q : r.minimap().quads()) {
            if (q.inst.rect[2] < 64.0f &&
                std::abs(q.inst.rect[0] + q.inst.rect[2] * 0.5f - x) < 1.0f &&
                std::abs(q.inst.rect[1] + q.inst.rect[3] * 0.5f - y) < 1.0f) {
                found.push_back(&q);
            }
        }
        return found;
    };
    const auto on_minimap = [&](u32 id, const std::string& texture,
                                const std::array<f32, 3>& tint) {
        const auto at = minimap_icons_at(id);
        const renderer::GPUTexture* tex = r.texture_cache().get(texture);
        if (at.empty() || !tex) {
            return false;
        }
        const renderer::UIQuad& q = *at.front();
        return q.texture_ds == tex->descriptor_set &&
               q.inst.rect[2] == static_cast<f32>(tex->width & ~1u) &&
               q.inst.rect[3] == static_cast<f32>(tex->height & ~1u) &&
               std::abs(q.inst.color[0] - tint[0]) < 0.01f &&
               std::abs(q.inst.color[1] - tint[1]) < 0.01f &&
               std::abs(q.inst.color[2] - tint[2]) < 0.01f;
    };
    t.check(on_minimap(tank, icon_texture(ctx, "uel0201", "rest"), blue) &&
                on_minimap(scout, icon_texture(ctx, "uea0101", "rest"), blue) &&
                on_minimap(sacu, icon_texture(ctx, "uel0301", "rest"), blue),
            fmt::format("Test 12: zoomed in, the minimap shows the tank's, scout's and support "
                        "commander's icons at their textures' sizes ({} minimap quads)",
                        r.minimap().quads().size()));
    t.check(on_minimap(picked, icon_texture(ctx, "uel0201", "selected"), blue),
            "Test 13: the selected tank's minimap icon is its selected one");
    t.check(on_minimap(e_eng, dir + "icon_land_generic_rest.dds", grey) &&
                on_minimap(e_gen, dir + "icon_structure_generic_rest.dds", grey),
            "Test 14: never-seen blips' minimap icons are generic, unidentified");
    t.check(minimap_icons_at(unbuilt).empty(),
            "Test 15: the power generator being built has no minimap icon");

    run_lua(ctx, "__osc_ic_bolt = __osc_ic_tank:CreateProjectile("
                 "'/projectiles/TDFHiroLaser01/TDFHiroLaser01_proj.bp', 0, 3, 0, 0, 0, 1)\n"
                 "__osc_ic_bolt:SetVelocity(0, 0, 0)\n");
    const auto bolt_squares = [&](const Frame& frame) {
        std::vector<Quad> found;
        for (const sim::EntityRecord& e : seen.cur().entities) {
            if (!e.is_projectile) {
                continue;
            }
            const auto at = screen_of(r, e.position);
            if (!at) {
                continue;
            }
            for (const Quad& q : frame.icons) {
                if (q.texture.empty() && std::abs(q.x - std::floor((*at)[0])) < 1.0f &&
                    std::abs(q.y - std::floor((*at)[1])) < 1.0f) {
                    found.push_back(q);
                }
            }
        }
        return found;
    };
    (void)shots.shoot(*ctx.sim.terrain(), sx, sz + 30, 300.0f);
    f = next(false);
    {
        const auto at = bolt_squares(f);
        t.check(at.size() == 1 && at[0].w == 3.0f && at[0].h == 3.0f &&
                    same_colour(at[0], 1.0f, 1.0f, 0.0f),
                fmt::format("Test 16: the bolt is a yellow square 3 across ({} there, zoom {:.0f})",
                            at.size(), r.camera().zoom()));
    }

    {
        int squares = 0;
        for (const sim::EntityRecord& e : seen.cur().entities) {
            if (!e.is_projectile) {
                continue;
            }
            const f32 x = std::floor(area.x + e.position.x / map_w * area.w);
            const f32 y = std::floor(area.y + e.position.z / map_h * area.h);
            for (const renderer::UIQuad& q : r.minimap().quads()) {
                if (q.inst.rect[2] == 3.0f && q.inst.rect[3] == 3.0f &&
                    std::abs(q.inst.rect[0] + 1.5f - x) < 1.0f &&
                    std::abs(q.inst.rect[1] + 1.5f - y) < 1.0f && q.inst.color[0] == 1.0f &&
                    q.inst.color[1] == 1.0f && q.inst.color[2] == 0.0f) {
                    ++squares;
                }
            }
        }
        t.check(squares == 1,
                fmt::format("Test 17: the bolt's square on the minimap ({} there)", squares));
    }

    r.set_weapons_yellow(false);
    f = next(false);
    {
        const auto at = bolt_squares(f);
        t.check(at.size() == 1 && same_colour(at[0], blue[0], blue[1], blue[2]),
                fmt::format("Test 18: unforced, the bolt's square is ARMY_1's colour ({} there)",
                            at.size()));
    }
    r.set_weapons_yellow(true);

    r.camera().set_eye_distance(80.0f);
    f = next(false);
    t.check(bolt_squares(f).empty() && r.camera().zoom() < 128.0f,
            fmt::format("Test 19: at zoom {:.0f}, the bolt has no square ({} there)",
                        r.camera().zoom(), bolt_squares(f).size()));
    const sim::TeamColors team = sim::read_game_colors(ctx.L).team_colors;
    const auto rgb = [](u32 argb) -> std::array<f32, 3> {
        return {static_cast<f32>(argb >> 16 & 0xFFu) / 255.0f,
                static_cast<f32>(argb >> 8 & 0xFFu) / 255.0f,
                static_cast<f32>(argb & 0xFFu) / 255.0f};
    };
    const std::string tank_icon = icon_texture(ctx, "uel0201", "rest");
    (void)shots.shoot(*ctx.sim.terrain(), sx, sz + 30, 300.0f);
    scry.push_back({sx + 40, sz + 84});
    (void)next();
    scry.clear();
    r.set_team_color_mode(true);
    f = next();
    t.check(shows(f, tank, tank_icon, rgb(team.self)) &&
                shows(f, e_tank, tank_icon, rgb(team.enemy)) &&
                shows(f, e_eng, dir + "icon_land_generic_rest.dds", grey) &&
                on_minimap(tank, tank_icon, rgb(team.self)),
            "Test 20: TeamColorMode: the player's tank in Self, the enemy's in Enemy, a blip "
            "unidentified, the minimap's in Self");
    run_lua(ctx, "SetAlliance(1, 2, 'Neutral')\n");
    f = next();
    const bool neutral = shows(f, e_tank, tank_icon, rgb(team.neutral));
    run_lua(ctx, "SetAlliance(1, 2, 'Ally')\n");
    f = next();
    t.check(neutral && shows(f, e_tank, tank_icon, rgb(team.ally)),
            "Test 21: TeamColorMode: a neutral army's tank in Neutral, an ally's in Ally");
    r.set_team_palette({0xFF00FF00u, 0xFFFF00FFu});
    f = next();
    t.check(shows(f, tank, tank_icon, rgb(0xFF00FF00u)) &&
                shows(f, e_tank, tank_icon, rgb(0xFFFF00FFu)),
            "Test 22: FAF's palette colours each army's icons by its index");
    r.set_team_palette({});
    r.set_team_color_mode(false);
    f = next();
    t.check(shows(f, tank, tank_icon, blue) && shows(f, e_tank, tank_icon, army_tint(1)),
            "Test 23: TeamColorMode off: the armies' own colours again");

    // A tracking projectile made with no target is destroyed at once.
    run_lua(ctx, "__blueprints['/projectiles/tifmissilenuke01/tifmissilenuke01_proj.bp']"
                 ".Physics.TrackTarget = false\n");
    run_lua(ctx, "__osc_ic_nuke = __osc_ic_tank:CreateProjectile("
                 "'/projectiles/TIFMissileNuke01/TIFMissileNuke01_proj.bp', 8, 10, 0, 0, 0, 1)\n"
                 "__osc_ic_nuke:SetCollision(false)\n"
                 "__osc_ic_nuke:SetVelocity(0, 0, 0)\n"
                 "__osc_ic_shell = __osc_ic_tank:CreateProjectile("
                 "'/projectiles/TIFAntiMatterShells01/TIFAntiMatterShells01_proj.bp', "
                 "-8, 3, 0, 0, 0, 1)\n"
                 "__osc_ic_shell:SetVelocity(0, 0, 0)\n");
    const auto projectile = [&](const char* name) -> const sim::EntityRecord* {
        for (const sim::EntityRecord& e : seen.cur().entities) {
            std::string id = e.blueprint_id;
            for (char& c : id) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            if (e.is_projectile && id.find(name) != std::string::npos) {
                return &e;
            }
        }
        return nullptr;
    };
    const auto projectile_at = [&](const char* name) -> std::optional<std::array<f32, 2>> {
        const sim::EntityRecord* e = projectile(name);
        const auto at = e ? screen_of(r, e->position) : std::nullopt;
        if (!at) {
            return std::nullopt;
        }
        return std::array<f32, 2>{std::floor((*at)[0]), std::floor((*at)[1])};
    };
    const std::string nuke_icon = "/textures/ui/common/game/unit-over/icon-nuke_bmp.dds";
    const auto nuke_icons = [&] {
        std::vector<renderer::ProjectileIcon> found;
        const auto at = projectile_at("tifmissilenuke01");
        for (const renderer::ProjectileIcon& p : r.projectile_icons()) {
            if (at && p.path == nuke_icon && std::abs(p.x + p.w * 0.5f - (*at)[0]) < 1.0f &&
                std::abs(p.y + p.h * 0.5f - (*at)[1]) < 1.0f) {
                found.push_back(p);
            }
        }
        return found;
    };
    const auto squares_at = [&](const Frame& frame, const char* name) {
        std::vector<Quad> found;
        const auto at = projectile_at(name);
        for (const Quad& q : frame.icons) {
            if (at && q.texture.empty() && std::abs(q.x - (*at)[0]) < 1.0f &&
                std::abs(q.y - (*at)[1]) < 1.0f) {
                found.push_back(q);
            }
        }
        return found;
    };
    (void)shots.shoot(*ctx.sim.terrain(), sx, sz + 30, 300.0f);
    (void)next(false);
    f = next(false);
    {
        const auto icons = nuke_icons();
        t.check(icons.size() == 1 && icons[0].w == 20.0f && icons[0].h == 20.0f &&
                    squares_at(f, "tifmissilenuke01").empty(),
                fmt::format("Test 20: the nuke is its 20x20 icon, not a square ({} icons, {} "
                            "squares there; {} projectile icons in all)",
                            icons.size(), squares_at(f, "tifmissilenuke01").size(),
                            r.projectile_icons().size()));
        const f32 glow = icons.empty() ? -1.0f : icons[0].glow;
        t.check(glow >= renderer::kProjectileGlowMin && glow <= renderer::kProjectileGlowMax &&
                    glow == renderer::projectile_icon_glow(r.projectile_glow_time()),
                fmt::format("Test 21: its glow {:.4f} is the pulse at {:.3f} s", glow,
                            r.projectile_glow_time()));
    }
    {
        renderer::Renderer::SceneImage scene;
        r.request_scene_capture(
            [&](renderer::Renderer::SceneImage image) { scene = std::move(image); });
        f = next(false);
        const auto icons = nuke_icons();
        int lit = 0;
        int dark = 0;
        if (icons.size() == 1 && scene.width > 0) {
            const renderer::ProjectileIcon& icon = icons[0];
            for (u32 y = static_cast<u32>(icon.y); y < static_cast<u32>(icon.y + icon.h); ++y) {
                for (u32 x = static_cast<u32>(icon.x); x < static_cast<u32>(icon.x + icon.w); ++x) {
                    const f32 a = scene.rgba[(static_cast<size_t>(y) * scene.width + x) * 4 + 3];
                    lit += std::abs(a - icon.glow) < 0.002f ? 1 : 0;
                    dark += a == 0.0f ? 1 : 0;
                }
            }
        }
        t.check(lit > 100 && dark > 0 && lit + dark == 400,
                fmt::format("Test 22: in the scene, before the bloom, the icon's texels glow at "
                            "{:.4f} and the rest of its quad at 0 ({} and {} of 400)",
                            icons.empty() ? -1.0f : icons[0].glow, lit, dark));
    }
    t.check(std::abs(renderer::projectile_icon_glow(0.0f) - 0.01f) < 1e-6f &&
                std::abs(renderer::projectile_icon_glow(1.0f) - 0.15f) < 1e-6f &&
                std::abs(renderer::projectile_icon_glow(1.5f) - 0.08f) < 1e-6f &&
                std::abs(renderer::projectile_icon_glow(2.0f) - 0.01f) < 1e-6f,
            "Test 23: the glow rises from 0.01 to 0.15 over a second and falls back over the next");
    {
        const auto at = squares_at(f, "tifantimattershells01");
        t.check(at.size() == 1 && at[0].w == 3.0f && same_colour(at[0], 1.0f, 1.0f, 0.0f),
                fmt::format("Test 24: the antimatter shell, its icon name under Display, is a "
                            "yellow square 3 across ({} there)",
                            at.size()));
    }
    {
        int icons = 0;
        const renderer::GPUTexture* tex = r.texture_cache().get(nuke_icon);
        if (const sim::EntityRecord* e = projectile("tifmissilenuke01")) {
            const f32 x = std::floor(area.x + e->position.x / map_w * area.w);
            const f32 y = std::floor(area.y + e->position.z / map_h * area.h);
            for (const renderer::UIQuad& q : r.minimap().quads()) {
                if (tex && q.texture_ds == tex->descriptor_set && q.inst.rect[2] == 20.0f &&
                    std::abs(q.inst.rect[0] + 10.0f - x) < 1.0f &&
                    std::abs(q.inst.rect[1] + 10.0f - y) < 1.0f && q.inst.uv[1] == 1.0f &&
                    q.inst.uv[3] == 0.0f) {
                    ++icons;
                }
            }
        }
        t.check(
            icons == 1,
            fmt::format("Test 25: the nuke's icon on the minimap, upside down ({} there)", icons));
    }
    r.set_projectile_glow(false);
    f = next(false);
    {
        const auto icons = nuke_icons();
        t.check(icons.size() == 1 && icons[0].glow < 0.0f,
                "Test 26: with UI_RenProjectileGlow off, the nuke's icon has no glow");
    }
    r.set_projectile_glow(true);
    r.set_projectile_icons(false);
    f = next(false);
    t.check(nuke_icons().empty() && squares_at(f, "tifmissilenuke01").empty() &&
                squares_at(f, "tifantimattershells01").size() == 1,
            fmt::format("Test 27: with UI_RenProjectileIcons off, the nuke has neither icon nor "
                        "square ({}, {}); the shell keeps its square",
                        nuke_icons().size(), squares_at(f, "tifmissilenuke01").size()));
    r.set_projectile_icons(true);

    // FAF's patched game's ui_StrategicIconScale and ui_ProjectileIconScale
    // (1 as retail): the half sizes times them, about the same centres.
    r.set_strategic_icon_scale(2.0f);
    r.set_projectile_icon_scale(2.0f);
    f = next(false);
    {
        const auto at = icons_at(f, tank);
        const renderer::GPUTexture* tex =
            r.texture_cache().get(icon_texture(ctx, "uel0201", "rest"));
        const f32 w = tex ? static_cast<f32>(tex->width & ~1u) : 0.0f;
        const f32 h = tex ? static_cast<f32>(tex->height & ~1u) : 0.0f;
        t.check(!at.empty() && w > 0.0f && f.icons[at.front()].w == 2.0f * w &&
                    f.icons[at.front()].h == 2.0f * h,
                fmt::format("Test 28: at ui_StrategicIconScale 2 the tank's icon is {}x{} ({}x{} "
                            "at 1)",
                            at.empty() ? 0.0f : f.icons[at.front()].w,
                            at.empty() ? 0.0f : f.icons[at.front()].h, w, h));
        const auto icons = nuke_icons();
        const auto squares = squares_at(f, "tifantimattershells01");
        t.check(icons.size() == 1 && icons[0].w == 40.0f && icons[0].h == 40.0f &&
                    squares.size() == 1 && squares[0].w == 6.0f,
                fmt::format("Test 29: at ui_ProjectileIconScale 2 the nuke's icon is 40 across "
                            "and the shell's square 6 ({} icons, {} squares there)",
                            icons.size(), squares.size()));
    }
    r.set_strategic_icon_scale(1.0f);
    r.set_projectile_icon_scale(1.0f);

    run_lua(ctx, "__osc_ic_nuke_at = __osc_ic_nuke:GetPosition()\n"
                 "__osc_ic_nuke:SetPosition(__osc_ic_tank:GetPosition(), true)\n");
    f = next(false);
    {
        const auto texels = [&](const std::string& path, u32& w, u32& h) {
            const auto file = ctx.vfs.read_file(path);
            const auto dds = file ? renderer::parse_dds(*file) : std::nullopt;
            if (!dds || dds->format != VK_FORMAT_BC3_UNORM_BLOCK || dds->mips.empty()) {
                return std::vector<u8>{};
            }
            w = dds->width;
            h = dds->height;
            return renderer::decode_bc3_to_rgba(reinterpret_cast<const u8*>(dds->mips[0].data), w,
                                                h);
        };
        u32 tank_w = 0;
        u32 tank_h = 0;
        u32 nuke_w = 0;
        u32 nuke_h = 0;
        const std::vector<u8> tank_texels =
            texels(icon_texture(ctx, "uel0201", "rest"), tank_w, tank_h);
        const std::vector<u8> nuke_texels = texels(nuke_icon, nuke_w, nuke_h);
        const auto at = icons_at(f, tank);
        const auto nukes = nuke_icons();
        const ImageRGBA8 with = shots.grab(&selection);
        r.set_projectile_icons(false);
        const ImageRGBA8 without = shots.grab(&selection);
        r.set_projectile_icons(true);
        int overlap = 0;
        int shown = 0;
        if (!at.empty() && nukes.size() == 1 && !tank_texels.empty() && !nuke_texels.empty() &&
            with.width == without.width && with.height == without.height) {
            const Quad& q = f.icons[at.front()];
            const auto left = static_cast<i32>(q.x - q.w * 0.5f);
            const auto top = static_cast<i32>(q.y - q.h * 0.5f);
            const auto nx = static_cast<i32>(nukes[0].x);
            const auto ny = static_cast<i32>(nukes[0].y);
            for (i32 v = 0; v < static_cast<i32>(nuke_h); ++v) {
                for (i32 u = 0; u < static_cast<i32>(nuke_w); ++u) {
                    const i32 x = nx + u;
                    const i32 y = ny + static_cast<i32>(nuke_h) - 1 - v;
                    const i32 tu = x - left;
                    const i32 tv = y - top;
                    if (tu < 0 || tv < 0 || tu >= static_cast<i32>(tank_w) ||
                        tv >= static_cast<i32>(tank_h) || x < 0 || y < 0 ||
                        x >= static_cast<i32>(with.width) || y >= static_cast<i32>(with.height)) {
                        continue;
                    }
                    const u8* nt =
                        &nuke_texels[(static_cast<size_t>(v) * nuke_w + static_cast<size_t>(u)) *
                                     4];
                    const u8* tt =
                        &tank_texels[(static_cast<size_t>(tv) * tank_w + static_cast<size_t>(tu)) *
                                     4];
                    const bool alike = std::equal(nt, nt + 3, tt, [](u8 a, u8 b) {
                        return std::abs(static_cast<int>(a) - static_cast<int>(b)) <= 8;
                    });
                    if (nt[3] != 255 || tt[3] != 255 || alike) {
                        continue;
                    }
                    const size_t i =
                        (static_cast<size_t>(y) * with.width + static_cast<size_t>(x)) * 4;
                    ++overlap;
                    shown += std::equal(&with.pixels[i], &with.pixels[i] + 3, &without.pixels[i])
                                 ? 0
                                 : 1;
                }
            }
        }
        t.check(overlap > 0 && shown == overlap,
                fmt::format("Test 30: a nuke over the tank is drawn over its icon, as Moho draws "
                            "the projectiles' icons after the units': {} of the {} pixels both "
                            "cover in colours of their own show it",
                            shown, overlap));
    }
    run_lua(ctx, "__osc_ic_nuke:SetPosition(__osc_ic_nuke_at, true)\n");
    (void)next(false);

    OffscreenShots retina(ctx, 2.0f);
    if (!retina.ok()) {
        t.check(false, "renderer init at a display scale of 2");
        return;
    }
    renderer::Renderer& big = retina.renderer();
    big.set_player_army(0);
    const auto tank_blocks = [&](i32 k) {
        std::array<int, 2> n{};
        const std::string rest = icon_texture(ctx, "uel0201", "rest");
        const auto file = ctx.vfs.read_file(rest);
        const auto dds = file ? renderer::parse_dds(*file) : std::nullopt;
        if (!dds || dds->format != VK_FORMAT_BC3_UNORM_BLOCK || dds->mips.empty()) {
            return n;
        }
        const std::vector<u8> texels = renderer::decode_bc3_to_rgba(
            reinterpret_cast<const u8*>(dds->mips[0].data), dds->width, dds->height);
        (void)retina.shoot(*ctx.sim.terrain(), sx, sz, 300.0f);
        const ImageRGBA8 image = retina.grab();
        const auto at = [&](i32 x, i32 y) {
            return &image.pixels[(static_cast<size_t>(y) * image.width + static_cast<size_t>(x)) *
                                 4];
        };
        const auto& quads = big.strategic_icons().quads();
        const auto& textures = big.strategic_icons().quad_textures();
        for (size_t i = 0; i < quads.size() && i < textures.size(); ++i) {
            if (textures[i] != rest) {
                continue;
            }
            const auto x0 = static_cast<i32>(2.0f * quads[i].rect[0]);
            const auto y0 = static_cast<i32>(2.0f * quads[i].rect[1]);
            for (u32 v = 0; v < dds->height; ++v) {
                for (u32 u = 0; u < dds->width; ++u) {
                    if (texels[(static_cast<size_t>(v) * dds->width + u) * 4 + 3] != 255) {
                        continue;
                    }
                    const i32 px = x0 + k * static_cast<i32>(u);
                    const i32 py = y0 + k * static_cast<i32>(v);
                    // An icon drawn later (a badge, a unit nearby) may lie over it.
                    const bool covered = std::any_of(
                        quads.begin() + static_cast<std::ptrdiff_t>(i) + 1, quads.end(),
                        [&](const renderer::UIInstance& q) {
                            return 2.0f * q.rect[0] < static_cast<f32>(px + k) &&
                                   2.0f * (q.rect[0] + q.rect[2]) > static_cast<f32>(px) &&
                                   2.0f * q.rect[1] < static_cast<f32>(py + k) &&
                                   2.0f * (q.rect[1] + q.rect[3]) > static_cast<f32>(py);
                        });
                    if (covered || px < 0 || py < 0 || px + k > static_cast<i32>(image.width) ||
                        py + k > static_cast<i32>(image.height)) {
                        continue;
                    }
                    bool same = true;
                    for (i32 dy = 0; dy < k; ++dy) {
                        for (i32 dx = 0; dx < k; ++dx) {
                            same = same &&
                                   std::equal(at(px, py), at(px, py) + 3, at(px + dx, py + dy));
                        }
                    }
                    ++n[0];
                    n[1] += same ? 0 : 1;
                }
            }
        }
        return n;
    };
    {
        const auto tank = tank_blocks(2);
        t.check(tank[0] > 0 && tank[1] == 0,
                fmt::format("Test 31: at a display scale of 2 each of the tank icon's opaque "
                            "texels is a 2x2 block of one colour (point sampled): {} of {} "
                            "blended",
                            tank[1], tank[0]));
    }

    {
        (void)retina.shoot(*ctx.sim.terrain(), sx, sz + 30, 300.0f);
        const ImageRGBA8 image = retina.grab();
        const Frame frame = drawn(big);
        renderer::Renderer::SceneImage scene;
        big.request_scene_capture(
            [&](renderer::Renderer::SceneImage captured) { scene = std::move(captured); });
        retina.redraw();
        const auto points_of = [&](const char* name) -> std::optional<std::array<f32, 2>> {
            const sim::EntityRecord* e = projectile(name);
            const auto at = e ? screen_of(big, e->position) : std::nullopt;
            if (!at) {
                return std::nullopt;
            }
            return std::array<f32, 2>{std::floor((*at)[0] / 2.0f), std::floor((*at)[1] / 2.0f)};
        };

        const renderer::ProjectileIcon* nuke = nullptr;
        for (const renderer::ProjectileIcon& p : big.projectile_icons()) {
            if (p.path == nuke_icon) {
                nuke = &p;
            }
        }
        const auto nuke_at = points_of("tifmissilenuke01");
        t.check(nuke && nuke_at && nuke->w == 20.0f && nuke->h == 20.0f &&
                    std::abs(nuke->x + 10.0f - (*nuke_at)[0]) < 1.0f &&
                    std::abs(nuke->y + 10.0f - (*nuke_at)[1]) < 1.0f,
                fmt::format("Test 32: at a display scale of 2 the nuke's 20x20 icon is centred on "
                            "its point, in points: ({:.1f}, {:.1f}) at ({:.0f}, {:.0f})",
                            nuke ? nuke->x + nuke->w * 0.5f : -1.0f,
                            nuke ? nuke->y + nuke->h * 0.5f : -1.0f,
                            nuke_at ? (*nuke_at)[0] : -1.0f, nuke_at ? (*nuke_at)[1] : -1.0f));

        const auto glowing = [&](f32 cx, f32 cy, i32 half, int& lit, int& dark) {
            const auto x0 = static_cast<i32>(cx) - half;
            const auto y0 = static_cast<i32>(cy) - half;
            for (i32 y = std::max(y0, 0);
                 y < std::min(y0 + 2 * half, static_cast<i32>(scene.height)); ++y) {
                for (i32 x = std::max(x0, 0);
                     x < std::min(x0 + 2 * half, static_cast<i32>(scene.width)); ++x) {
                    const f32 a =
                        scene.rgba[(static_cast<size_t>(y) * scene.width + static_cast<size_t>(x)) *
                                       4 +
                                   3];
                    lit += std::abs(a - nuke->glow) < 0.002f ? 1 : 0;
                    dark += a == 0.0f ? 1 : 0;
                }
            }
        };
        int lit = 0;
        int dark = 0;
        int lit_1x = 0;
        int dark_1x = 0;
        if (nuke && nuke->glow > 0.0f) {
            glowing(2.0f * (nuke->x + 10.0f), 2.0f * (nuke->y + 10.0f), 20, lit, dark);
            glowing(nuke->x + 10.0f, nuke->y + 10.0f, 20, lit_1x, dark_1x);
        }
        t.check(lit > 400 && lit + dark == 1600 && lit_1x == 0,
                fmt::format("Test 33: in the {}x{} scene the nuke's icon glows in the 40x40 "
                            "pixels around twice its point ({} lit, {} dark), none around the "
                            "point itself ({} lit)",
                            scene.width, scene.height, lit, dark, lit_1x));

        const auto shell_at = points_of("tifantimattershells01");
        const Quad* square =
            shell_at ? quad_at(frame.icons, (*shell_at)[0], (*shell_at)[1], 3.0f, 3.0f) : nullptr;
        int yellow = 0;
        int spilled = 0;
        if (square && image.width > 0) {
            const auto x0 = static_cast<i32>(2.0f * (square->x - 1.5f));
            const auto y0 = static_cast<i32>(2.0f * (square->y - 1.5f));
            for (i32 y = std::max(y0 - 1, 0); y < std::min(y0 + 7, static_cast<i32>(image.height));
                 ++y) {
                for (i32 x = std::max(x0 - 1, 0);
                     x < std::min(x0 + 7, static_cast<i32>(image.width)); ++x) {
                    const u8* px = &image.pixels[(static_cast<size_t>(y) * image.width +
                                                  static_cast<size_t>(x)) *
                                                 4];
                    if (px[0] != 255 || px[1] != 255 || px[2] != 0) {
                        continue;
                    }
                    if (x >= x0 && x < x0 + 6 && y >= y0 && y < y0 + 6) {
                        ++yellow;
                    } else {
                        ++spilled;
                    }
                }
            }
        }
        t.check(square && yellow == 36 && spilled == 0,
                fmt::format("Test 34: at a display scale of 2 the shell's yellow square, 3 points "
                            "across, is the 6x6 pixels at twice its point ({} of 36 yellow, {} "
                            "around them)",
                            yellow, spilled));

        const auto file = ctx.vfs.read_file(nuke_icon);
        const auto dds = file ? renderer::parse_dds(*file) : std::nullopt;
        std::vector<u8> texels;
        if (dds && dds->format == VK_FORMAT_BC3_UNORM_BLOCK && !dds->mips.empty()) {
            texels = renderer::decode_bc3_to_rgba(reinterpret_cast<const u8*>(dds->mips[0].data),
                                                  dds->width, dds->height);
        }
        const auto blocks_at = [&](f32 left, f32 top, u32 width, u32 height, const auto& at,
                                   const std::vector<std::array<f32, 4>>& over) {
            std::array<int, 2> n{};
            if (texels.empty()) {
                return n;
            }
            const auto x0 = static_cast<i32>(2.0f * left);
            const auto y0 = static_cast<i32>(2.0f * top);
            for (u32 v = 0; v < dds->height; ++v) {
                for (u32 u = 0; u < dds->width; ++u) {
                    if (texels[(static_cast<size_t>(v) * dds->width + u) * 4 + 3] != 255) {
                        continue;
                    }
                    const i32 px = x0 + 2 * static_cast<i32>(u);
                    const i32 py = y0 + 2 * static_cast<i32>(dds->height - 1 - v);
                    const bool covered =
                        std::any_of(over.begin(), over.end(), [&](const std::array<f32, 4>& r) {
                            return 2.0f * r[0] < static_cast<f32>(px + 2) &&
                                   2.0f * (r[0] + r[2]) > static_cast<f32>(px) &&
                                   2.0f * r[1] < static_cast<f32>(py + 2) &&
                                   2.0f * (r[1] + r[3]) > static_cast<f32>(py);
                        });
                    if (covered || px < 0 || py < 0 || px + 1 >= static_cast<i32>(width) ||
                        py + 1 >= static_cast<i32>(height)) {
                        continue;
                    }
                    bool same = true;
                    for (const auto& [dx, dy] :
                         {std::pair{1, 0}, std::pair{0, 1}, std::pair{1, 1}}) {
                        same = same && std::equal(at(px, py), at(px, py) + 3, at(px + dx, py + dy));
                    }
                    ++n[0];
                    n[1] += same ? 0 : 1;
                }
            }
            return n;
        };
        std::array<int, 2> in_scene{};
        if (nuke && scene.width > 0) {
            in_scene = blocks_at(nuke->x, nuke->y, scene.width, scene.height,
                                 [&](i32 x, i32 y) {
                                     return &scene.rgba[(static_cast<size_t>(y) * scene.width +
                                                         static_cast<size_t>(x)) *
                                                        4];
                                 },
                                 {});
        }
        t.check(in_scene[0] > 0 && in_scene[1] == 0,
                fmt::format("Test 35: at a display scale of 2 each of the nuke icon's opaque "
                            "texels is a 2x2 block of one colour in the scene, as the tank's "
                            "icon: {} of {} blended",
                            in_scene[1], in_scene[0]));

        const renderer::GPUTexture* tex = big.texture_cache().get(nuke_icon);
        std::array<int, 2> on_map{};
        const auto& map_quads = big.minimap().quads();
        for (size_t i = 0; i < map_quads.size(); ++i) {
            const renderer::UIQuad& q = map_quads[i];
            if (!tex || q.texture_ds != tex->descriptor_set || image.width == 0) {
                continue;
            }
            std::vector<std::array<f32, 4>> later;
            for (size_t j = i + 1; j < map_quads.size(); ++j) {
                const auto& r = map_quads[j].inst.rect;
                later.push_back({r[0], r[1], r[2], r[3]});
            }
            on_map = blocks_at(
                q.inst.rect[0], q.inst.rect[1], image.width, image.height,
                [&](i32 x, i32 y) {
                    return &image.pixels[(static_cast<size_t>(y) * image.width +
                                          static_cast<size_t>(x)) *
                                         4];
                },
                later);
        }
        t.check(on_map[0] > 0 && on_map[1] == 0,
                fmt::format("Test 36: so is its icon on the minimap: {} of {} blended", on_map[1],
                            on_map[0]));

        big.set_strategic_icon_scale(2.0f);
        const auto tank = tank_blocks(4);
        t.check(tank[0] > 0 && tank[1] == 0,
                fmt::format("Test 37: at a display scale of 2 and ui_StrategicIconScale 2 each of "
                            "the tank icon's opaque texels is a 4x4 block of one colour: {} of {} "
                            "blended",
                            tank[1], tank[0]));
        big.set_projectile_icon_scale(2.0f);
        (void)retina.shoot(*ctx.sim.terrain(), sx, sz + 30, 300.0f);
        big.request_scene_capture(
            [&](renderer::Renderer::SceneImage captured) { scene = std::move(captured); });
        retina.redraw();
        nuke = nullptr;
        for (const renderer::ProjectileIcon& p : big.projectile_icons()) {
            if (p.path == nuke_icon) {
                nuke = &p;
            }
        }
        int lit_4x = 0;
        int dark_4x = 0;
        if (nuke && nuke->w == 40.0f && nuke->glow > 0.0f) {
            glowing(2.0f * (nuke->x + 20.0f), 2.0f * (nuke->y + 20.0f), 40, lit_4x, dark_4x);
        }
        t.check(lit_4x > 1600 && lit_4x + dark_4x == 6400,
                fmt::format("Test 38: at a display scale of 2 and ui_ProjectileIconScale 2 the "
                            "nuke's icon glows in the 80x80 pixels around twice its point ({} "
                            "lit, {} dark)",
                            lit_4x, dark_4x));
        big.set_strategic_icon_scale(1.0f);
        big.set_projectile_icon_scale(1.0f);
    }

    {
        renderer::Renderer& big = retina.renderer();
        big.camera().set_ortho(true);
        renderer::Renderer::SceneImage scene;
        big.request_scene_capture(
            [&](renderer::Renderer::SceneImage captured) { scene = std::move(captured); });
        retina.redraw();
        big.camera().set_ortho(false);
        const renderer::ProjectileIcon* nuke = nullptr;
        for (const renderer::ProjectileIcon& p : big.projectile_icons()) {
            if (p.path == nuke_icon) {
                nuke = &p;
            }
        }
        const auto lit_around = [&](f32 cx, f32 cy) {
            int lit = 0;
            const auto x0 = static_cast<i32>(cx) - 20;
            const auto y0 = static_cast<i32>(cy) - 20;
            for (i32 y = std::max(y0, 0); y < std::min(y0 + 40, static_cast<i32>(scene.height));
                 ++y) {
                for (i32 x = std::max(x0, 0); x < std::min(x0 + 40, static_cast<i32>(scene.width));
                     ++x) {
                    const f32 a =
                        scene.rgba[(static_cast<size_t>(y) * scene.width + static_cast<size_t>(x)) *
                                       4 +
                                   3];
                    lit += std::abs(a - nuke->glow) < 0.002f ? 1 : 0;
                }
            }
            return lit;
        };
        int lit = 0;
        int lit_1x = 0;
        if (nuke && nuke->glow > 0.0f) {
            lit = lit_around(2.0f * (nuke->x + 10.0f), 2.0f * (nuke->y + 10.0f));
            lit_1x = lit_around(nuke->x + 10.0f, nuke->y + 10.0f);
        }
        t.check(big.cartographic_main() && lit > 400 && lit_1x == 0,
                fmt::format("Test 33b: so in a cartographic main view ({}): {} lit around twice "
                            "its point, {} around the point itself",
                            big.cartographic_main(), lit, lit_1x));
    }

    spdlog::info("Strategic icon test: {}/{} passed", t.pass, t.pass + t.fail);
}

} // namespace osc::test
