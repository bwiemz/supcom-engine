// --selection-render-test: Moho's selection indicators (ren_SelectBoxes).
//
// On dry ground away from the starts, two of ARMY_1's engineers and one of
// ARMY_2's. The selected one has retail's player brackets, sized from its
// blueprint's SelectionSize; the one under the cursor has the highlighted
// ones, an enemy's the enemy's; the drag box is drawn; the overlay draws no
// ring; ren_SelectBoxes off draws none.

#include "integration_tests.hpp"
#include "intel_probe.hpp"
#include "render_probe.hpp"

#include "renderer/input_handler.hpp"
#include "renderer/renderer.hpp"
#include "renderer/selection_renderer.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/world_snapshot.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>

#include <cmath>
#include <string>
#include <unordered_set>

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

bool ends_with(const std::string& s, const std::string& tail) {
    return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
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
                std::abs(mine->box.half_x - 0.4f) < 1e-3f &&
                std::abs(mine->box.half_z - 0.4f) < 1e-3f,
            fmt::format("Test 1: the selected unit's brackets: {}, half {:.3f} x {:.3f}",
                        mine ? mine->texture : "none", mine ? mine->box.half_x : 0.0f,
                        mine ? mine->box.half_z : 0.0f));

    // Test 2: no ring of the overlay's round it
    bool ring = false;
    for (const Quad& q : drawn(r).overlay) {
        ring = ring || same_colour(q, 0.2f, 1.0f, 0.2f);
    }
    t.check(!ring, "Test 2: the overlay draws no selection ring");

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
    const f32 click_x = fp.x + 1.8f;
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
    input.world_click(ctx.sim, tp.x, tp.z, true, true, r.camera().view_proj(aspect));
    const auto& shifted = input.selected();
    t.check(shifted.size() == 2 && shifted.count(tank) == 1 && shifted.count(tank2) == 1,
            fmt::format("Test 10: a Shift double-click keeps the tank and adds its like: {} "
                        "selected (tank {}, second {})",
                        shifted.size(), shifted.count(tank), shifted.count(tank2)));

    spdlog::info("Selection test: {}/{} passed", t.pass, t.pass + t.fail);
}

} // namespace osc::test
