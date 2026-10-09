// --strategic-icon-test (M215c): FA's strategic icons.
//
// Moho draws each unit's blueprint icon (StrategicIconName's rest or
// selected texture, at the texture's own size, tinted by its army's colour)
// once the camera is out past its mesh's IconFadeInZoom, and a blip's at any
// zoom: generic (structure, land, naval, air) in UnidentifiedColor until the
// unit has been seen. The runs draw ground, air, high-priority, selected; a
// stunned unit's badge goes over its icon; a unit being built has none. On
// dry ground away from the starts stand ARMY_1's units, all senses off but
// a radar, and beyond them ARMY_2's, on that radar.

#include "integration_tests.hpp"
#include "intel_probe.hpp"
#include "render_probe.hpp"

#include "renderer/army_colors.hpp"
#include "renderer/renderer.hpp"
#include "renderer/strategic_icon_renderer.hpp"
#include "renderer/texture_cache.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/world_snapshot.hpp"

#include <lua.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <string>
#include <unordered_set>
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
    (void)spawn_unit(ctx, "__osc_ic_radar", "ueb1101", "ARMY_1", {sx, sz + 60});
    // ARMY_2's, 24 beyond the radar (it reaches 60): an engineer, a power
    // generator, an air scout (none of them ever seen), and a tank to be
    // seen once.
    const u32 e_eng = spawn_unit(ctx, "__osc_ic_eeng", "uel0105", "ARMY_2", {sx - 20, sz + 84});
    const u32 e_gen = spawn_unit(ctx, "__osc_ic_egen", "ueb1101", "ARMY_2", {sx, sz + 84});
    const u32 e_air = spawn_unit(ctx, "__osc_ic_eair", "uea0101", "ARMY_2", {sx + 20, sz + 84});
    const u32 e_tank = spawn_unit(ctx, "__osc_ic_etank", "uel0201", "ARMY_2", {sx + 40, sz + 84});
    run_lua(ctx, "for _, u in {__osc_ic_tank, __osc_ic_scout, __osc_ic_sacu, __osc_ic_unbuilt,\n"
                 "              __osc_ic_picked, __osc_ic_dazed, __osc_ic_radar} do\n"
                 "  u:DisableIntel('Vision') u:DisableIntel('Radar') u:DisableIntel('Omni')\n"
                 "end\n"
                 "__osc_ic_radar:InitIntel(1, 'Radar', 60)\n"
                 "__osc_ic_radar:EnableIntel('Radar')\n"
                 "__osc_ic_dazed:SetStunned(100)\n");
    if (auto* e = ctx.sim.entity_registry().find(unbuilt); e && e->is_unit())
        static_cast<sim::Unit*>(e)->set_is_being_built(true);
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
    const auto next = [&] {
        for (const Spot& at : scry)
            run_lua(ctx,
                    fmt::format("CreateVisibleAreaAtPoint(1, {}, 0, {}, 12, 0.1)\n", at.x, at.z));
        ctx.sim.tick();
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

    // Test 3: a unit being built has none.
    t.check(icons_at(f, unbuilt).empty(), "Test 3: the power generator being built has no icon");

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

    // Test 9: zoomed in, inside IconFadeInZoom: ARMY_1's units, drawn as
    // themselves, have none; the blips keep theirs.
    r.camera().set_target(sx, sz + 42);
    r.camera().set_eye_distance(110.0f);
    f = next();
    t.check(icons_at(f, tank).empty() && icons_at(f, scout).empty() && !icons_at(f, e_eng).empty(),
            fmt::format("Test 9: zoomed in, the tank has no icon ({}), the blip has ({})",
                        icons_at(f, tank).size(), icons_at(f, e_eng).size()));

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

    spdlog::info("Strategic icon test: {}/{} passed", t.pass, t.pass + t.fail);
}

} // namespace osc::test
