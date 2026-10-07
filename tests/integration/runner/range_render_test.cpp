// --range-render-test: FA's range overlays as Moho's RangeRenderer draws
// them. ARMY_1's tanks on dry ground, retail's DirectFire profile: the
// selected tank's ring in its selected colour on the ground at its reach,
// the hovered one's in the rollover colour, the active filter's over every
// own built tank in view (none for an enemy's), two near tanks' rings
// merged, a placement's at the cursor; the stencil left clear; nothing with
// the convars off.

#include "integration_tests.hpp"
#include "intel_probe.hpp"
#include "render_probe.hpp"

#include "renderer/range_overlays.hpp"
#include "renderer/renderer.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"

#include <spdlog/spdlog.h>

#include <cmath>
#include <optional>
#include <string>
#include <unordered_set>

namespace osc::test {

namespace {

using renderer::RangeColor;

/// Retail's DirectFire overlay (rangeoverlayparams.lua)
renderer::RangeProfile direct_fire() {
    renderer::RangeProfile p;
    p.name = "DirectFire";
    p.categories = sim::CategoryExpr::all();
    p.normal = renderer::range_color(0x00ff2c2cu);
    p.selected = renderer::range_color(0x08ff5253u);
    p.rollover = renderer::range_color(0x10ff6363u);
    p.inner = {0.02f, 2.0f};
    p.outer = {0.04f, 4.0f};
    return p;
}

bool near_colour(const ImageRGBA8& image, i32 x, i32 y, const RangeColor& c) {
    if (x < 0 || y < 0 || x >= static_cast<i32>(image.width) || y >= static_cast<i32>(image.height))
        return false;
    const u8* p =
        &image.pixels[(static_cast<size_t>(y) * image.width + static_cast<size_t>(x)) * 4];
    for (int i = 0; i < 3; ++i)
        if (std::abs(static_cast<f32>(p[i]) / 255.0f - c[static_cast<size_t>(i)]) > 0.06f)
            return false;
    return true;
}

/// Whether `image` has colour `c` within 2 pixels of world (x, z) on the
/// ground
bool coloured_at(renderer::Renderer& r, const sim::SimState& sim, const ImageRGBA8& image, f32 x,
                 f32 z, const RangeColor& c) {
    const auto s = screen_of(r, {x, sim.terrain()->get_terrain_height(x, z), z});
    if (!s) return false;
    const i32 sx = static_cast<i32>(std::lround((*s)[0]));
    const i32 sy = static_cast<i32>(std::lround((*s)[1]));
    for (i32 dy = -2; dy <= 2; ++dy)
        for (i32 dx = -2; dx <= 2; ++dx)
            if (near_colour(image, sx + dx, sy + dy, c)) return true;
    return false;
}

/// Of `n` points round the circle of `radius` about (x, z) (from angle a0
/// to a1), how many show colour `c`
int on_circle(renderer::Renderer& r, const sim::SimState& sim, const ImageRGBA8& image, f32 x,
              f32 z, f32 radius, const RangeColor& c, int n = 36, f32 a0 = 0.0f,
              f32 a1 = 6.2831853f) {
    int hits = 0;
    for (int i = 0; i < n; ++i) {
        const f32 a = a0 + (a1 - a0) * (static_cast<f32>(i) + 0.5f) / static_cast<f32>(n);
        hits += coloured_at(r, sim, image, x + radius * std::cos(a), z + radius * std::sin(a), c)
                    ? 1
                    : 0;
    }
    return hits;
}

} // namespace

void test_range_render(TestContext& ctx) {
    spdlog::info("=== RANGE TEST: FA's range overlays, as RangeRenderer draws them ===");
    Tally t;

    const auto spot = quiet_spot(ctx.sim);
    if (!spot) {
        t.check(false, "dry ground 60 from every unit");
        return;
    }
    const f32 sx = spot->x;
    const f32 sz = spot->z;
    const u32 tank = spawn_unit(ctx, "__osc_rr_a", "uel0201", "ARMY_1", {sx, sz});
    ctx.sim.tick();

    OffscreenShots shots(ctx);
    if (!shots.ok()) {
        t.check(false, "renderer init (no Vulkan?)");
        return;
    }
    renderer::Renderer& r = shots.renderer();
    r.set_player_army(0);
    renderer::RangeOverlays& overlays = r.range_overlays();
    overlays.set_profile(direct_fire());
    (void)shots.shoot(*ctx.sim.terrain(), sx, sz, 110.0f);

    const std::unordered_set<u32> selected{tank};
    const auto frame = [&](const std::unordered_set<u32>* sel,
                           const renderer::BuildGhost* ghost = nullptr) {
        for (int i = 0; i < 4; ++i) shots.redraw(sel, ghost);
        return shots.grab(sel, ghost);
    };
    const auto& drawn = [&]() -> const std::vector<renderer::RangeRenderer::Drawn>& {
        return r.range_renderer().drawn();
    };
    const ImageRGBA8 bare = frame(nullptr);
    t.check(drawn().empty(), "Test 1: nothing selected, no filter: no ring");

    // Test 2: selected, with range_RenderSelected (retail's UI turns it on)
    overlays.settings().render_selected = true;
    const ImageRGBA8 sel = frame(&selected);
    const auto& rings = r.range_renderer();
    const bool one = drawn().size() == 1 && drawn()[0].fill_count == 1;
    const renderer::RangeRing fill =
        one ? rings.bands()[drawn()[0].fill_first] : renderer::RangeRing{};
    const f32 thick = one ? drawn()[0].outer_thickness : 0.0f;
    const f32 reach = fill.outer + thick;
    const RangeColor want = direct_fire().selected;
    const int hits = on_circle(r, ctx.sim, sel, fill.x, fill.z, reach - thick * 0.5f, want);
    const int inside = on_circle(r, ctx.sim, sel, fill.x, fill.z, reach * 0.5f, want);
    t.check(one && reach > 10.0f && hits >= 32 && inside == 0 && drawn()[0].color == want,
            fmt::format("Test 2: the selected tank's ring at its reach {:.1f} (line {:.2f}): {} of "
                        "36 points on it, {} inside",
                        reach, thick, hits, inside));

    // Test 3: range_RenderSelected off: none
    overlays.settings().render_selected = false;
    (void)frame(&selected);
    t.check(drawn().empty(), "Test 3: range_RenderSelected off draws no ring");

    // Test 4: under the cursor, the rollover colour
    overlays.settings().render_highlighted = true;
    r.set_selection_marks(tank, std::nullopt);
    const ImageRGBA8 hover = frame(nullptr);
    const int hover_hits =
        on_circle(r, ctx.sim, hover, fill.x, fill.z, reach - thick * 0.5f, direct_fire().rollover);
    t.check(drawn().size() == 1 && drawn()[0].color == direct_fire().rollover && hover_hits >= 32,
            fmt::format("Test 4: the hovered tank's ring in the rollover colour ({} of 36)",
                        hover_hits));
    r.set_selection_marks(0, std::nullopt);
    overlays.settings().render_highlighted = false;

    // Test 5: the active filter: every own built tank in view, not an
    // enemy's, nor one being built
    const u32 foe = spawn_unit(ctx, "__osc_rr_b", "uel0201", "ARMY_2", {sx - 40, sz});
    const u32 frame_only = spawn_unit(ctx, "__osc_rr_c", "uel0201", "ARMY_1", {sx, sz - 40});
    if (auto* u = dynamic_cast<sim::Unit*>(ctx.sim.entity_registry().find(frame_only))) {
        u->set_is_being_built(true);
        u->set_fraction_complete(0.5f);
    }
    ctx.sim.tick();
    shots.recapture();
    overlays.set_filters({"DirectFire"});
    const ImageRGBA8 filtered = frame(nullptr);
    const bool lone = drawn().size() == 1 && drawn()[0].fill_count == 1;
    const int filter_hits =
        on_circle(r, ctx.sim, filtered, fill.x, fill.z, reach - thick * 0.5f, direct_fire().normal);
    t.check(lone && drawn()[0].color == direct_fire().normal && filter_hits >= 32,
            fmt::format("Test 5: the filter rings the own built tank alone ({} batches, {} of 36; "
                        "foe {}, being built {})",
                        drawn().size(), filter_hits, foe, frame_only));

    // Test 6: two near tanks' rings merge: neither line shows inside the
    // other's ring, both show outside
    const u32 second = spawn_unit(ctx, "__osc_rr_d", "uel0201", "ARMY_1", {sx + 12, sz});
    ctx.sim.tick();
    shots.recapture();
    const ImageRGBA8 merged = frame(nullptr);
    const bool two = drawn().size() == 1 && drawn()[0].fill_count == 2;
    // The first tank's line toward the second (inside its ring), and away
    const f32 mid = reach - thick * 0.5f;
    const int toward =
        on_circle(r, ctx.sim, merged, fill.x, fill.z, mid, direct_fire().normal, 9, -0.35f, 0.35f);
    const int away = on_circle(r, ctx.sim, merged, fill.x, fill.z, mid, direct_fire().normal, 9,
                               3.14159f - 0.35f, 3.14159f + 0.35f);
    t.check(two && toward == 0 && away >= 8,
            fmt::format("Test 6: two tanks' rings merge: {} toward the other, {} away (of 9; "
                        "second {})",
                        toward, away, second));
    overlays.set_filters({});

    // Test 7: placing one, with range_RenderBuild: its blueprint's ring at
    // the cursor, in the normal colour
    overlays.settings().render_build = true;
    renderer::BuildGhost ghost;
    ghost.blueprint_id = "uel0201";
    ghost.x = ghost.cursor_x = sx;
    ghost.z = ghost.cursor_z = sz + 30;
    ghost.y = ctx.sim.terrain()->get_terrain_height(sx, sz + 30);
    (void)frame(nullptr, &ghost);
    const bool placed = drawn().size() == 1 && drawn()[0].color == direct_fire().normal &&
                        rings.bands()[drawn()[0].fill_first].x == sx &&
                        rings.bands()[drawn()[0].fill_first].z == sz + 30;
    t.check(placed,
            fmt::format("Test 7: a placement's ring at the cursor ({} batches)", drawn().size()));
    overlays.settings().render_build = false;

    // Test 8: the stencil is left clear: a frame without rings is the
    // frame before them
    (void)frame(nullptr);
    const ImageRGBA8 after = frame(nullptr);
    size_t differing = 0;
    for (size_t i = 0; i < bare.pixels.size() && i < after.pixels.size(); ++i)
        differing +=
            std::abs(static_cast<int>(bare.pixels[i]) - static_cast<int>(after.pixels[i])) > 8 ? 1
                                                                                               : 0;
    t.check(drawn().empty() && bare.pixels.size() == after.pixels.size(),
            fmt::format("Test 8: no rings once they are off ({} channel values differ from the "
                        "first frame, the units since)",
                        differing));

    // Test 9: ren_Ranges off: none, whatever is on
    overlays.settings().render_selected = true;
    overlays.settings().enabled = false;
    (void)frame(&selected);
    t.check(drawn().empty(), "Test 9: ren_Ranges off draws no ring");

    spdlog::info("Range test: {}/{} passed", t.pass, t.pass + t.fail);
}

} // namespace osc::test
