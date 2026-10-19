// --selection-render-test: Moho's selection indicators (ren_SelectBoxes).
//
// On dry ground away from the starts, two of ARMY_1's engineers and one of
// ARMY_2's. The selected one has retail's player brackets, sized from its
// blueprint's SelectionSize; the one under the cursor has the highlighted
// ones, an enemy's the enemy's; the drag box is drawn; the overlay draws no
// ring; ren_SelectBoxes off draws none. An aircraft is picked, and boxed,
// where it is drawn at its height.

#include "integration_tests.hpp"
#include "intel_probe.hpp"
#include "render_probe.hpp"

#include "renderer/input_handler.hpp"
#include "renderer/renderer.hpp"
#include "renderer/selection_renderer.hpp"
#include "renderer/unit_renderer.hpp"
#include "renderer/vision_renderer.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/world_snapshot.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>

#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace osc::test {

namespace {

const renderer::SelectionRenderer::Bracket* bracket_of(const renderer::Renderer& r, u32 id) {
    for (const auto& b : r.selection_renderer().brackets()) {
        if (b.unit == id) {
            return &b;
        }
    }
    return nullptr;
}

std::optional<std::array<f32, 3>> mesh_colour_at(const renderer::Renderer& r,
                                                 const sim::Vector3& at) {
    const renderer::MeshInstance* all = r.unit_renderer().mesh_instances();
    for (const auto& g : r.unit_renderer().mesh_groups()) {
        for (u32 i = 0; all && i < g.instance_count; ++i) {
            const renderer::MeshInstance& m = all[g.instance_offset + i];
            if (std::abs(m.model[12] - at.x) < 0.1f && std::abs(m.model[14] - at.z) < 0.1f) {
                return std::array<f32, 3>{m.r, m.g, m.b};
            }
        }
    }
    return std::nullopt;
}

bool ends_with(const std::string& s, const std::string& tail) {
    return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

/// The pixels (x0, y0, x1, y1) that `points` span on `r`'s frame
std::array<int, 4> screen_area(const renderer::Renderer& r,
                               const std::vector<sim::Vector3>& points) {
    const f32 w = static_cast<f32>(r.width());
    const f32 h = static_cast<f32>(r.height());
    const auto view = r.camera().view_proj(w / std::max(h, 1.0f));
    std::array<f32, 4> area = {w, h, 0, 0};
    for (const auto& p : points) {
        if (const auto s = renderer::screen_point(view, p, w, h)) {
            area = {std::min(area[0], (*s)[0]), std::min(area[1], (*s)[1]),
                    std::max(area[2], (*s)[0]), std::max(area[3], (*s)[1])};
        }
    }
    return {std::max(0, static_cast<int>(area[0])), std::max(0, static_cast<int>(area[1])),
            std::min(static_cast<int>(w) - 1, static_cast<int>(area[2])),
            std::min(static_cast<int>(h) - 1, static_cast<int>(area[3]))};
}

std::vector<sim::Vector3> tile_corners(const renderer::SelectionRenderer::Bracket& b) {
    std::vector<sim::Vector3> out;
    for (const auto& tile : renderer::bracket_quads(b.box, b.corner)) {
        out.insert(out.end(), tile.begin(), tile.end());
    }
    return out;
}

int colour_gap(const ImageRGBA8& a, const ImageRGBA8& b, size_t i) {
    int gap = 0;
    for (size_t c = 0; c < 3; ++c) {
        gap = std::max(
            gap, std::abs(static_cast<int>(a.pixels[i + c]) - static_cast<int>(b.pixels[i + c])));
    }
    return gap;
}

} // namespace

void test_selection_render(TestContext& ctx) {
    spdlog::info("=== SELECTION TEST: brackets, hover and the drag box ===");
    Tally t;

    const auto spot = quiet_spot(ctx.sim);
    if (!spot) {
        t.check(false, "dry ground 60 from every unit");
        return;
    }
    const f32 sx = spot->x;
    const f32 sz = spot->z;
    const u32 own = spawn_unit(ctx, "__osc_sel_a", "uel0105", "ARMY_1", {sx, sz});
    const u32 other = spawn_unit(ctx, "__osc_sel_b", "uel0105", "ARMY_1", {sx + 8, sz});
    const u32 enemy = spawn_unit(ctx, "__osc_sel_c", "uel0105", "ARMY_2", {sx - 8, sz});
    ctx.sim.tick();

    OffscreenShots shots(ctx);
    if (!shots.ok()) {
        t.check(false, "renderer init (no Vulkan?)");
        return;
    }
    renderer::Renderer& r = shots.renderer();
    r.set_player_army(0);
    (void)shots.shoot(*ctx.sim.terrain(), sx, sz, 40.0f);
    const std::unordered_set<u32> selected{own};
    // A few frames: the textures load in the background
    const auto draw = [&] {
        for (int i = 0; i < 10; ++i) {
            shots.redraw(&selected);
        }
    };

    // Test 1: the selected engineer's brackets, its blueprint's box
    draw();
    const auto* mine = bracket_of(r, own);
    t.check(mine && ends_with(mine->texture, "selection_brackets_player.dds") &&
                std::abs(mine->box.half_x - 0.3f) < 1e-3f &&
                std::abs(mine->box.half_z - 0.3f) < 1e-3f,
            fmt::format("Test 1: the selected unit's brackets: {}, half {:.3f} x {:.3f}",
                        mine ? mine->texture : "none", mine ? mine->box.half_x : 0.0f,
                        mine ? mine->box.half_z : 0.0f));

    // Test 2: no ring of the overlay's round it
    bool ring = false;
    for (const Quad& q : drawn(r).overlay) {
        ring = ring || same_colour(q, 0.2f, 1.0f, 0.2f);
    }
    t.check(!ring, "Test 2: the overlay draws no selection ring");

    const auto* own_entity = ctx.sim.entity_registry().find(own);
    const sim::Vector3 own_at = own_entity ? own_entity->position() : sim::Vector3{sx, 0, sz};
    const auto selected_colour = mesh_colour_at(r, own_at);
    shots.redraw();
    const auto plain_colour = mesh_colour_at(r, own_at);
    const auto rgb = [](const std::optional<std::array<f32, 3>>& c) {
        return c ? fmt::format("{:.3f} {:.3f} {:.3f}", (*c)[0], (*c)[1], (*c)[2])
                 : std::string("none");
    };
    t.check(selected_colour && plain_colour && *selected_colour == *plain_colour,
            fmt::format("Test 2b: the selected unit's mesh colour ({}) is its unselected ({})",
                        rgb(selected_colour), rgb(plain_colour)));

    // Test 3: under the cursor, an own unit's highlighted brackets, an
    // enemy's the enemy's
    const auto texture_of = [&](u32 id) {
        const auto* b = bracket_of(r, id);
        return b ? b->texture : std::string("none");
    };
    r.set_selection_marks(other, std::nullopt);
    draw();
    const std::string hovered_own = texture_of(other);
    r.set_selection_marks(enemy, std::nullopt);
    draw();
    const std::string hovered_enemy = texture_of(enemy);
    t.check(ends_with(hovered_own, "_player_highlighted.dds") &&
                ends_with(hovered_enemy, "_enemy.dds"),
            fmt::format("Test 3: hovered: own {}, enemy {}", hovered_own, hovered_enemy));

    // Test 4: the cursor's unit is the one whose box holds the point
    renderer::InputHandler input;
    input.set_player_army(0);
    sim::WorldHistory seen;
    seen.capture(ctx.sim);
    seen.capture(ctx.sim);
    input.set_frame_view(sim::FrameView(&seen.prev(), &seen.cur(), 1.0f));
    const sim::Vector3 at = seen.cur().find(other)->position;
    const u32 on = input.unit_under(ctx.sim, at.x + 0.1f, at.z);
    const u32 beside = input.unit_under(ctx.sim, at.x + 3.0f, at.z);
    t.check(on == other && beside == 0,
            fmt::format("Test 4: under the cursor: on the unit {}, beside it {}", on, beside));

    // Test 5: the drag box
    const f32 y = ctx.sim.terrain()->get_terrain_height(sx, sz);
    r.set_selection_marks(
        0,
        std::array<sim::Vector3, 4>{
            {{sx - 4, y, sz - 4}, {sx + 4, y, sz - 4}, {sx + 4, y, sz + 4}, {sx - 4, y, sz + 4}}});
    draw();
    t.check(r.selection_renderer().drew_drag_box(), "Test 5: the drag box is drawn");

    // Test 6: ren_SelectBoxes off draws none
    r.set_selection_marks(0, std::nullopt);
    r.set_select_boxes(false);
    draw();
    const bool none = r.selection_renderer().brackets().empty();
    r.set_select_boxes(true);
    t.check(none, "Test 6: ren_SelectBoxes off draws no brackets");

    const u32 tank = spawn_unit(ctx, "__osc_sel_d", "uel0201", "ARMY_1", {sx, sz + 8});
    const auto priority = [&](u32 id) {
        const auto* e = ctx.sim.entity_registry().find(id);
        return e && e->is_unit() ? static_cast<const sim::Unit*>(e)->selection_priority() : 0;
    };
    t.check(priority(own) == 3 && priority(tank) == 1,
            fmt::format("Test 7: selection priority: engineer {}, tank {}", priority(own),
                        priority(tank)));

    const u32 factory = spawn_unit(ctx, "__osc_sel_f", "ueb0101", "ARMY_1", {sx, sz - 20});
    const auto* fe = ctx.sim.entity_registry().find(factory);
    const sim::Vector3 fp = fe ? fe->position() : sim::Vector3{sx, 0, sz - 20};
    const u32 builder = spawn_unit(ctx, "__osc_sel_g", "uel0105", "ARMY_1", {fp.x + 2.6f, fp.z});
    seen.capture(ctx.sim);
    seen.capture(ctx.sim);
    input.set_frame_view(sim::FrameView(&seen.prev(), &seen.cur(), 1.0f));
    input.set_selected({builder});
    const f32 click_x = fp.x + 2.3f;
    const u32 under = input.unit_under(ctx.sim, click_x, fp.z);
    input.left_click_at(ctx.sim, click_x, fp.z, false);
    const auto& now = input.selected();
    t.check(under == factory && now.size() == 1 && now.count(factory) == 1,
            fmt::format("Test 8: a click on the factory by its builder selects the unit under "
                        "the cursor ({}): {} selected, factory {}, builder {}",
                        under == factory ? "the factory" : "not the factory", now.size(),
                        now.count(factory), now.count(builder)));

    // Test 9: a double-click on a tank adds every tank of the player's in
    // view (Moho's HandleDoubleClickSelection): not the enemy's, nor one off
    // the screen, nor the engineer.
    const u32 tank2 = spawn_unit(ctx, "__osc_sel_t2", "uel0201", "ARMY_1", {sx + 6, sz + 8});
    const u32 far_tank = spawn_unit(ctx, "__osc_sel_t3", "uel0201", "ARMY_1", {sx + 300, sz + 300});
    const u32 foe_tank = spawn_unit(ctx, "__osc_sel_t4", "uel0201", "ARMY_2", {sx - 6, sz + 8});
    seen.capture(ctx.sim);
    seen.capture(ctx.sim);
    input.set_frame_view(sim::FrameView(&seen.prev(), &seen.cur(), 1.0f));
    const auto* te = ctx.sim.entity_registry().find(tank);
    const sim::Vector3 tp = te ? te->position() : sim::Vector3{sx, 0, sz + 8};
    input.left_click_at(ctx.sim, tp.x, tp.z, false);
    const f32 aspect = static_cast<f32>(r.width()) / static_cast<f32>(std::max(r.height(), 1u));
    input.select_similar_in_view(ctx.sim, tp.x, tp.z, r.camera().view_proj(aspect));
    const auto& similar = input.selected();
    t.check(similar.size() == 2 && similar.count(tank) == 1 && similar.count(tank2) == 1 &&
                similar.count(far_tank) == 0 && similar.count(foe_tank) == 0 &&
                similar.count(own) == 0,
            fmt::format("Test 9: a double-click selects the tanks in view: {} selected (tank {}, "
                        "second {}, far {}, enemy {}, engineer {})",
                        similar.size(), similar.count(tank), similar.count(tank2),
                        similar.count(far_tank), similar.count(foe_tank), similar.count(own)));

    // Test 10: with Shift held, the second click of a double-click doesn't
    // toggle the tank back off: it is a double-click, not a click.
    input.set_selected({});
    input.left_click_at(ctx.sim, tp.x, tp.z, true);
    input.world_click(ctx.sim, tp.x, tp.z, true, false, true, r.camera().view_proj(aspect));
    const auto& shifted = input.selected();
    t.check(shifted.size() == 2 && shifted.count(tank) == 1 && shifted.count(tank2) == 1,
            fmt::format("Test 10: a Shift double-click keeps the tank and adds its like: {} "
                        "selected (tank {}, second {})",
                        shifted.size(), shifted.count(tank), shifted.count(tank2)));

    // Tests 11-12: an aircraft is picked where it is drawn, at its height:
    // the cursor's ray meets its box short of the ground, and a drag box
    // holds it by where it is on the screen. By the ground under the cursor
    // (as picking had gone) it is missed.
    const u32 air = spawn_unit(ctx, "__osc_sel_air", "uea0101", "ARMY_1", {sx + 4, sz - 10});
    // An idle aircraft lands after its AutoLandTime: this one patrols.
    if (auto* plane = static_cast<sim::Unit*>(ctx.sim.entity_registry().find(air))) {
        sim::UnitCommand patrol;
        patrol.type = sim::CommandType::Patrol;
        patrol.target_pos = {sx + 8, 0, sz - 10};
        plane->push_command(patrol, true);
        patrol.target_pos = {sx, 0, sz - 10};
        plane->push_command(patrol, false);
    }
    for (int i = 0; i < 60; ++i) ctx.sim.tick(); // up to its height
    seen.capture(ctx.sim);
    seen.capture(ctx.sim);
    input.set_frame_view(sim::FrameView(&seen.prev(), &seen.cur(), 1.0f));
    const sim::EntityRecord* air_rec = seen.cur().find(air);
    const sim::Vector3 ap = air_rec ? air_rec->position : sim::Vector3{};
    const f32 ground = ctx.sim.terrain()->get_surface_height(ap.x, ap.z);
    sim::Vector3 eye{};
    r.camera().eye_position(eye.x, eye.y, eye.z);
    sim::Vector3 dir{ap.x - eye.x, ap.y + 0.1f - eye.y, ap.z - eye.z};
    const f32 len = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    dir = {dir.x / len, dir.y / len, dir.z / len};
    // Where that ray meets the ground beyond the aircraft
    f32 gx = ap.x;
    f32 gz = ap.z;
    for (int step = 0; step < 1600; ++step) {
        const f32 s = len + 0.25f * static_cast<f32>(step);
        gx = eye.x + dir.x * s;
        gz = eye.z + dir.z * s;
        if (eye.y + dir.y * s <= ctx.sim.terrain()->get_surface_height(gx, gz)) break;
    }
    renderer::InputHandler by_ground; // no cursor ray: straight down at (gx, gz)
    by_ground.set_player_army(0);
    by_ground.set_frame_view(sim::FrameView(&seen.prev(), &seen.cur(), 1.0f));
    const u32 old_pick = by_ground.unit_under(ctx.sim, gx, gz);
    input.set_cursor_ray({eye, dir}, gx, gz);
    const u32 ray_pick = input.unit_under(ctx.sim, gx, gz);
    t.check(ap.y - ground > 5.0f && ray_pick == air && old_pick != air,
            fmt::format("Test 11: an aircraft {:.1f} up is picked where it is drawn: by the ray "
                        "{}, by the ground under the cursor {}",
                        ap.y - ground, ray_pick == air ? "it" : std::to_string(ray_pick),
                        old_pick == air ? "it" : std::to_string(old_pick)));

    const auto vp = r.camera().view_proj(aspect);
    const f32 sw = static_cast<f32>(r.width());
    const f32 sh = static_cast<f32>(r.height());
    const auto drawn_at = renderer::screen_point(vp, ap, sw, sh);
    const auto under_at = renderer::screen_point(vp, {ap.x, ground, ap.z}, sw, sh);
    bool boxed_air = false;
    bool boxed_ground = true;
    if (drawn_at && under_at) {
        input.set_selected({});
        input.select_in_box(ctx.sim, vp, sw, sh, (*drawn_at)[0] - 6, (*drawn_at)[1] - 6,
                            (*drawn_at)[0] + 6, (*drawn_at)[1] + 6, true);
        boxed_air = input.selected().count(air) == 1;
        input.set_selected({});
        input.select_in_box(ctx.sim, vp, sw, sh, (*under_at)[0] - 6, (*under_at)[1] - 6,
                            (*under_at)[0] + 6, (*under_at)[1] + 6, true);
        boxed_ground = input.selected().count(air) == 1;
    }
    t.check(boxed_air && !boxed_ground,
            fmt::format("Test 12: a drag box holds the aircraft where it is drawn ({}), not "
                        "where the ground under it is ({})",
                        boxed_air, boxed_ground));

    // Test 13: brackets are tested against the scene's depth
    const u32 walls = spawn_unit(ctx, "__osc_sel_pg", "ueb1301", "ARMY_1", {sx + 30, sz + 40});
    const auto* we = ctx.sim.entity_registry().find(walls);
    const sim::Vector3 wp = we ? we->position() : sim::Vector3{sx + 30, 0, sz + 40};
    const u32 inside = spawn_unit(ctx, "__osc_sel_in", "uel0105", "ARMY_1", {wp.x, wp.z});
    const u32 outside = spawn_unit(ctx, "__osc_sel_out", "uel0105", "ARMY_1", {sx + 30, sz + 50});
    ctx.sim.tick();
    shots.recapture();
    const auto bracket_pixels = [&](u32 id) {
        const auto* e = ctx.sim.entity_registry().find(id);
        const sim::Vector3 p = e ? e->position() : sim::Vector3{};
        (void)shots.shoot_frame(*ctx.sim.terrain(), p.x, p.z, 25.0f);
        const std::unordered_set<u32> one{id};
        const ImageRGBA8 plain = shots.grab();
        const ImageRGBA8 marked = shots.grab(&one);
        const auto* b = bracket_of(r, id);
        if (!b || plain.width == 0 || plain.pixels.size() != marked.pixels.size()) {
            return -1;
        }
        const auto [x0, y0, x1, y1] = screen_area(r, tile_corners(*b));
        const int width = static_cast<int>(plain.width);
        int n = 0;
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                const size_t i = static_cast<size_t>(y * width + x) * 4;
                if (marked.pixels[i + 2] > plain.pixels[i + 2] + 25) {
                    ++n;
                }
            }
        }
        return n;
    };
    const int open = bracket_pixels(outside);
    const int walled = bracket_pixels(inside);
    t.check(walls != 0 && open > 20 && walled == 0,
            fmt::format("Test 13: brackets behind a mesh are hidden: {} pixels of an engineer's "
                        "in the open, {} of one's inside a power generator",
                        open, walled));

    // Test 14: the fog of war is drawn over the brackets
    r.set_unit_bars(false);
    run_lua(ctx, "__osc_sel_air:Destroy()");
    ctx.sim.tick();
    const u32 wall = spawn_unit(ctx, "__osc_sel_wall", "ueb5101", "ARMY_1", {sx - 25, sz + 65});
    shots.recapture();
    seen.capture(ctx.sim);
    seen.capture(ctx.sim);
    const auto* wall_e = ctx.sim.entity_registry().find(wall);
    const sim::Vector3 wl = wall_e ? wall_e->position() : sim::Vector3{sx - 25, 0, sz + 65};
    bool unseen = true;
    for (const auto& c :
         renderer::vision_circles(sim::FrameView(&seen.prev(), &seen.cur(), 1.0f), 0)) {
        const f32 dx = c.x - wl.x;
        const f32 dz = c.z - wl.z;
        if (dx * dx + dz * dz < c.outer * c.outer) {
            unseen = false;
        }
    }
    (void)shots.shoot_frame(*ctx.sim.terrain(), wl.x, wl.z, 20.0f);
    const std::unordered_set<u32> wall_set{wall};
    r.set_fog_enabled(true);
    const ImageRGBA8 fog_plain = shots.grab();
    const ImageRGBA8 fog_marked = shots.grab(&wall_set);
    r.set_fog_enabled(false);
    const ImageRGBA8 lit_plain = shots.grab();
    const ImageRGBA8 lit_marked = shots.grab(&wall_set);
    std::vector<f32> dimmed;
    const auto* wb = bracket_of(r, wall);
    if (wb && lit_plain.pixels.size() == fog_marked.pixels.size()) {
        const auto [x0, y0, x1, y1] = screen_area(r, tile_corners(*wb));
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                const size_t i = static_cast<size_t>(y * static_cast<int>(lit_plain.width) + x) * 4;
                if (lit_marked.pixels[i + 2] > lit_plain.pixels[i + 2] + 25) {
                    dimmed.push_back(static_cast<f32>(fog_marked.pixels[i + 2]) /
                                     static_cast<f32>(lit_marked.pixels[i + 2]));
                }
            }
        }
    }
    const f32 dim = dimmed.empty() ? 1.0f : median(dimmed);
    t.check(unseen && dimmed.size() >= 20 && dim < 0.8f,
            fmt::format("Test 14: a wall's brackets out of sight ({}) are darkened by the fog of "
                        "war: {} pixels, their blue {:.2f} of the lit ones'",
                        unseen ? "unseen" : "seen", dimmed.size(), dim));

    // Test 15: the brackets write their depth where their texture shows
    const u32 high = spawn_unit(ctx, "__osc_sel_hi", "uel0105", "ARMY_1", {sx + 45, sz + 20}, 1.0f);
    shots.recapture();
    const auto* high_e = ctx.sim.entity_registry().find(high);
    const sim::Vector3 hp = high_e ? high_e->position() : sim::Vector3{sx + 45, 0, sz + 20};
    (void)shots.shoot_frame(*ctx.sim.terrain(), hp.x, hp.z, 20.0f);
    r.camera().eye_position(eye.x, eye.y, eye.z);
    const sim::Vector3 ray = {hp.x - eye.x, hp.y - eye.y, hp.z - eye.z};
    const f32 beyond = (hp.y - ctx.sim.terrain()->get_terrain_height(hp.x, hp.z)) / -ray.y;
    const u32 low = spawn_unit(ctx, "__osc_sel_lo", "uel0105", "ARMY_2",
                               {hp.x + ray.x * beyond, hp.z + ray.z * beyond});
    shots.recapture();
    const std::unordered_set<u32> upper_set{high};
    const ImageRGBA8 unmarked = shots.grab();
    const ImageRGBA8 upper = shots.grab(&upper_set);
    r.set_selection_marks(low, std::nullopt);
    const ImageRGBA8 lower = shots.grab();
    const ImageRGBA8 stacked = shots.grab(&upper_set);
    const auto* hb = bracket_of(r, high);
    r.set_selection_marks(0, std::nullopt);
    r.set_unit_bars(true);
    int overlap = 0;
    int upper_shown = 0;
    int clear = 0;
    int lower_shown = 0;
    if (hb && unmarked.pixels.size() == stacked.pixels.size()) {
        const auto [x0, y0, x1, y1] = screen_area(r, tile_corners(*hb));
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                const size_t i = static_cast<size_t>(y * static_cast<int>(unmarked.width) + x) * 4;
                if (colour_gap(lower, unmarked, i) <= 40) {
                    continue;
                }
                if (colour_gap(upper, unmarked, i) > 40) {
                    ++overlap;
                    if (colour_gap(stacked, upper, i) < colour_gap(stacked, lower, i)) {
                        ++upper_shown;
                    }
                } else if (colour_gap(upper, unmarked, i) == 0) {
                    ++clear;
                    if (colour_gap(stacked, lower, i) < colour_gap(stacked, unmarked, i)) {
                        ++lower_shown;
                    }
                }
            }
        }
    }
    t.check(overlap >= 10 && upper_shown * 10 >= overlap * 9 && clear >= 10 &&
                lower_shown * 10 >= clear * 9,
            fmt::format("Test 15: a hovered unit's brackets beneath a selected one's: hidden "
                        "under its strokes at {} of {} pixels, shown through its clear texels "
                        "at {} of {}",
                        upper_shown, overlap, lower_shown, clear));

    spdlog::info("Selection test: {}/{} passed", t.pass, t.pass + t.fail);
}

} // namespace osc::test
