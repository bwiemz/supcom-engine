// --beam-render-test (M214a): FA's beams.
//
// A beam effect with a BeamBlueprint draws as a strip facing the camera,
// 2·Thickness across, StartColor at its start and EndColor at its end, its
// texture repeating RepeatRate times a unit of length and scrolling by
// UShift and VShift a tick, blended by its Blendmode (particle.fx's TBeam
// techniques), not past its LODCutoff. On dry ground away from the starts
// ARMY_1's engineers hang 3 above the ground (so their beams clear it),
// joined by beams of the test's own blueprints (a white texture); a
// collision beam carries one from its start to its far end; an engineer
// builds, drawing retail's build beams; and ARMY_2's beam in the fog isn't
// drawn.

#include "integration_tests.hpp"
#include "support/temp_path.hpp"
#include "intel_probe.hpp"
#include "plate_fixtures.hpp"
#include "render_probe.hpp"

#include "renderer/beam_renderer.hpp"
#include "renderer/renderer.hpp"
#include "sim/entity.hpp"
#include "sim/sim_state.hpp"
#include "sim/world_snapshot.hpp"
#include "vfs/directory_mount.hpp"
#include "vfs/virtual_file_system.hpp"

#include <lua.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace osc::test {

namespace {

constexpr const char* kRoot = "/osc_beam_test";

/// The drawn beam of effect `id`, or null.
const renderer::BeamRenderer::Drawn* beam_of(const renderer::Renderer& r, u32 id) {
    for (const auto& b : r.beam_renderer().drawn())
        if (b.effect_id == id) return &b;
    return nullptr;
}

bool near3(const sim::Vector3& a, const sim::Vector3& b, f32 tol = 0.05f) {
    return std::abs(a.x - b.x) < tol && std::abs(a.y - b.y) < tol && std::abs(a.z - b.z) < tol;
}

f32 distance(const sim::Vector3& a, const sim::Vector3& b) {
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) +
                     (a.z - b.z) * (a.z - b.z));
}

} // namespace

void test_beam_render(TestContext& ctx) {
    spdlog::info("=== BEAM TEST: FA's beams (M214a) ===");
    Tally t;

    const auto spot = quiet_spot(ctx.sim);
    if (!spot) {
        t.check(false, "dry ground 60 from every unit");
        return;
    }
    const f32 sx = spot->x;
    const f32 sz = spot->z;
    spdlog::info("Beam test: the spot ({:.0f}, {:.0f})", sx, sz);

    // The test's blueprints: a white texture; a fat additive beam going red
    // to blue; a fat subtractive (MODULATEINVERSE) one in white; one that
    // stops drawing 5 from the camera.
    const TempDir scratch("osc_beam_test");
    const auto& dir = scratch.path();
    write_dds(dir / "white.dds", 1,
              [](int, u32, u32) { return std::array<u8, 4>{255, 255, 255, 255}; });
    const auto beam_bp = [&](const char* file, const char* colors, int blend, f32 lod) {
        std::ofstream(dir / file) << fmt::format(
            "BeamBlueprint {{\n"
            "    TextureName = '{}/white.dds',\n"
            "    Thickness = 1, Length = 12, RepeatRate = 0.5, UShift = 0.1, VShift = -0.2,\n"
            "    {}\n"
            "    Blendmode = {}, LODCutoff = {},\n"
            "}}\n",
            kRoot, colors, blend, lod);
    };
    beam_bp("add.bp", "StartColor = {x=1,y=0,z=0,w=1}, EndColor = {x=0,y=0,z=1,w=1},", 3, 500);
    beam_bp("mod.bp", "StartColor = {x=1,y=1,z=1,w=1}, EndColor = {x=1,y=1,z=1,w=1},", 1, 500);
    beam_bp("lod.bp", "StartColor = {x=1,y=1,z=1,w=1}, EndColor = {x=1,y=1,z=1,w=1},", 3, 5);
    ctx.vfs.mount(kRoot, std::make_unique<vfs::DirectoryMount>(dir));
    const std::string add_bp = std::string(kRoot) + "/add.bp";
    const std::string mod_bp = std::string(kRoot) + "/mod.bp";
    const std::string lod_bp = std::string(kRoot) + "/lod.bp";

    // ARMY_1's engineers, 3 up: A to B (additive), C (a beam along its
    // facing), D to E (subtractive), A to C (the LOD's).
    constexpr f32 kLift = 3.0f;
    const u32 a = spawn_unit(ctx, "__osc_bt_a", "uel0105", "ARMY_1", {sx - 12, sz}, kLift);
    const u32 b = spawn_unit(ctx, "__osc_bt_b", "uel0105", "ARMY_1", {sx + 12, sz}, kLift);
    const u32 c = spawn_unit(ctx, "__osc_bt_c", "uel0105", "ARMY_1", {sx - 12, sz + 20}, kLift);
    const u32 d = spawn_unit(ctx, "__osc_bt_d", "uel0105", "ARMY_1", {sx - 12, sz - 20}, kLift);
    const u32 e = spawn_unit(ctx, "__osc_bt_e", "uel0105", "ARMY_1", {sx + 12, sz - 20}, kLift);
    const auto effect_id = [&](const char* global) {
        run_lua(ctx, fmt::format("__osc_bt_id = {0} and {0}._c_effect_id or 0\n", global));
        lua_pushstring(ctx.L, "__osc_bt_id");
        lua_rawget(ctx.L, LUA_GLOBALSINDEX);
        const u32 id = lua_isnumber(ctx.L, -1) ? static_cast<u32>(lua_tonumber(ctx.L, -1)) : 0;
        lua_pop(ctx.L, 1);
        return id;
    };
    run_lua(
        ctx,
        fmt::format(
            "__osc_bt_ab = AttachBeamEntityToEntity(__osc_bt_a, -1, __osc_bt_b, -1, 1, '{0}')\n"
            "__osc_bt_c1 = CreateBeamEmitter('{0}', 1)\n"
            "AttachBeamToEntity(__osc_bt_c1, __osc_bt_c, -1, 1)\n"
            "__osc_bt_de = AttachBeamEntityToEntity(__osc_bt_d, -1, __osc_bt_e, -1, 1, '{1}')\n"
            "__osc_bt_lod = AttachBeamEntityToEntity(__osc_bt_a, -1, __osc_bt_c, -1, 1, '{2}')\n",
            add_bp, mod_bp, lod_bp));
    const u32 ab = effect_id("__osc_bt_ab");
    const u32 c1 = effect_id("__osc_bt_c1");
    const u32 de = effect_id("__osc_bt_de");
    const u32 lod = effect_id("__osc_bt_lod");

    // A collision beam (a weapon's) 30 west, reaching 20 north; its beam
    // emitter on its start.
    const Spot beam_at{sx - 30, sz - 10};
    run_lua(ctx, fmt::format("local beam = {{}}\n"
                             "setmetatable(beam, {{ __index = moho.CollisionBeamEntity }})\n"
                             "moho.CollisionBeamEntity.__init(beam, {{ BeamBone = 0 }})\n"
                             "Warp(beam, Vector({}, {}, {}))\n"
                             "moho.CollisionBeamEntity.Enable(beam)\n"
                             "__osc_bt_cb = beam\n"
                             "__osc_bt_cbfx = CreateBeamEmitter('{}', 1)\n"
                             "AttachBeamToEntity(__osc_bt_cbfx, beam, 0, 1)\n"
                             "__osc_bt_cbid = tonumber(beam:GetEntityId())\n",
                             beam_at.x,
                             ctx.sim.terrain()->get_terrain_height(beam_at.x, beam_at.z) + kLift,
                             beam_at.z, add_bp));
    const u32 cbfx = effect_id("__osc_bt_cbfx");
    lua_pushstring(ctx.L, "__osc_bt_cbid");
    lua_rawget(ctx.L, LUA_GLOBALSINDEX);
    const u32 cb = lua_isnumber(ctx.L, -1) ? static_cast<u32>(lua_tonumber(ctx.L, -1)) : 0;
    lua_pop(ctx.L, 1);
    sim::Vector3 cb_end{beam_at.x, 0, beam_at.z + 20};
    cb_end.y = ctx.sim.terrain()->get_terrain_height(cb_end.x, cb_end.z) + kLift;
    const auto hold_beam_end = [&] {
        if (auto* ent = ctx.sim.entity_registry().find(cb)) ent->set_beam_endpoint(cb_end);
    };
    hold_beam_end();

    OffscreenShots shots(ctx);
    if (!shots.ok()) {
        t.check(false, "renderer init (no Vulkan?)");
        return;
    }
    renderer::Renderer& r = shots.renderer();
    sim::WorldHistory seen;
    const auto next = [&](int ticks = 1) {
        for (int i = 0; i < ticks; ++i) {
            ctx.sim.tick();
            hold_beam_end();
        }
        shots.recapture();
        shots.redraw();
        seen.capture(ctx.sim);
    };
    const auto pos = [&](u32 id) {
        const sim::EntityRecord* rec = seen.cur().find(id);
        return rec ? rec->position : sim::Vector3{};
    };

    // Test 1: A to B, as the blueprint says.
    (void)shots.shoot(*ctx.sim.terrain(), sx, sz, 60.0f);
    next();
    const auto* beam = beam_of(r, ab);
    t.check(beam && near3(beam->start, pos(a)) && near3(beam->end, pos(b)) &&
                beam->thickness == 1.0f && beam->blendmode == 3 && beam->start_color[0] == 1.0f &&
                beam->end_color[2] == 1.0f,
            fmt::format("Test 1: the beam runs from A to B, 1 thick, additive, red to blue ({})",
                        beam ? "drawn" : "not drawn"));

    // Test 2: its texture repeats RepeatRate times a unit (24 long: 12), and
    // scrolls UShift and VShift a tick.
    const f32 u_then = beam ? beam->u_offset : 0.0f;
    const f32 v_then = beam ? beam->v_start : 0.0f;
    const f32 span = beam ? beam->v_end - beam->v_start : 0.0f;
    next(10);
    beam = beam_of(r, ab);
    const f32 du = beam ? beam->u_offset - u_then : 0.0f;
    const f32 dv = beam ? beam->v_start - v_then : 0.0f;
    t.check(std::abs(span - 12.0f) < 0.01f && std::abs(du - 1.0f) < 0.05f &&
                std::abs(dv + 2.0f) < 0.05f,
            fmt::format("Test 2: V spans {:.3f} (12); 10 ticks scroll U {:.3f} (1), V {:.3f} (-2)",
                        span, du, dv));

    // Test 3: a beam emitter on C reaches the blueprint's Length (12) along
    // C's facing.
    const auto* along = beam_of(r, c1);
    bool facing = false;
    if (const sim::EntityRecord* rec = seen.cur().find(c); rec && along) {
        const sim::Vector3 fwd = sim::quat_rotate(rec->orientation, {0, 0, 1});
        facing = near3(along->end, {along->start.x + fwd.x * 12.0f, along->start.y + fwd.y * 12.0f,
                                    along->start.z + fwd.z * 12.0f});
    }
    t.check(along && near3(along->start, pos(c)) &&
                std::abs(distance(along->start, along->end) - 12.0f) < 0.01f && facing,
            fmt::format("Test 3: the beam on C reaches {:.3f} (12), along its facing: {}",
                        along ? distance(along->start, along->end) : 0.0f, facing));

    // Test 4: the collision beam's, from its start to its far end, with no
    // placeholder in the overlay.
    const auto* weapon = beam_of(r, cbfx);
    const Frame f = drawn(r);
    const bool placeholder = std::any_of(f.overlay.begin(), f.overlay.end(), [](const Quad& q) {
        return same_colour(q, 1.0f, 0.4f, 0.1f);
    });
    t.check(
        weapon && near3(weapon->start, pos(cb)) && near3(weapon->end, cb_end) && !placeholder,
        fmt::format("Test 4: the collision beam's beam runs to its far end ({}); placeholder: {}",
                    weapon ? "drawn" : "not drawn", placeholder));

    // Test 5: the LOD beam, 5 its cutoff, isn't drawn from 60 away.
    const bool lod_line = std::any_of(f.overlay.begin(), f.overlay.end(), [](const Quad& q) {
        return same_colour(q, 0.8f, 0.9f, 1.0f);
    });
    t.check(
        !beam_of(r, lod) && beam_of(r, de) && !lod_line,
        fmt::format("Test 5: past its LODCutoff a beam isn't drawn; overlay line: {}", lod_line));

    // Test 6: in the frame, the additive beam adds its colour, red at its
    // start and blue at its end; the subtractive one takes colour away.
    {
        const ImageRGBA8 with = shots.grab();
        run_lua(ctx, "__osc_bt_ab:Destroy() __osc_bt_de:Destroy()\n");
        next();
        const ImageRGBA8 without = shots.grab();
        const auto at = [&](const ImageRGBA8& img, const sim::Vector3& p) -> std::array<f32, 3> {
            const auto s = screen_of(r, p);
            if (!s || img.width == 0) return {0, 0, 0};
            const u32 x =
                static_cast<u32>(std::clamp((*s)[0], 0.0f, static_cast<f32>(img.width - 1)));
            const u32 y =
                static_cast<u32>(std::clamp((*s)[1], 0.0f, static_cast<f32>(img.height - 1)));
            const u8* px = &img.pixels[(static_cast<size_t>(y) * img.width + x) * 4];
            return {px[0] / 255.0f, px[1] / 255.0f, px[2] / 255.0f};
        };
        const auto along_ab = [&](f32 k) {
            const sim::Vector3 pa = pos(a);
            const sim::Vector3 pb = pos(b);
            return sim::Vector3{pa.x + (pb.x - pa.x) * k, pa.y + (pb.y - pa.y) * k,
                                pa.z + (pb.z - pa.z) * k};
        };
        // Across the strip: 0.7 from its axis (inside its 1) the red is
        // there; 1.5 out (past it), not.
        f32 inside_red = 0;
        f32 outside_red = 0;
        {
            f32 ex = 0, ey = 0, ez = 0;
            r.camera().eye_position(ex, ey, ez);
            const sim::Vector3 mid = along_ab(0.3f);
            const sim::Vector3 fwd{r.camera().focus_x() - ex, r.camera().focus_y() - ey,
                                   r.camera().focus_z() - ez};
            const sim::Vector3 pa = pos(a);
            const sim::Vector3 pb = pos(b);
            const sim::Vector3 axis{pa.x - pb.x, pa.y - pb.y, pa.z - pb.z};
            sim::Vector3 side{fwd.y * axis.z - fwd.z * axis.y, fwd.z * axis.x - fwd.x * axis.z,
                              fwd.x * axis.y - fwd.y * axis.x};
            const f32 n = std::sqrt(side.x * side.x + side.y * side.y + side.z * side.z);
            if (n > 0) side = {side.x / n, side.y / n, side.z / n};
            const auto off = [&](f32 k) {
                return sim::Vector3{mid.x + side.x * k, mid.y + side.y * k, mid.z + side.z * k};
            };
            inside_red = at(with, off(0.7f))[0] - at(without, off(0.7f))[0];
            outside_red = at(with, off(1.5f))[0] - at(without, off(1.5f))[0];
        }
        const auto near_start_with = at(with, along_ab(0.15f));
        const auto near_start_without = at(without, along_ab(0.15f));
        const auto near_end_with = at(with, along_ab(0.85f));
        const auto near_end_without = at(without, along_ab(0.85f));
        const f32 red_start = near_start_with[0] - near_start_without[0];
        const f32 blue_start = near_start_with[2] - near_start_without[2];
        const f32 red_end = near_end_with[0] - near_end_without[0];
        const f32 blue_end = near_end_with[2] - near_end_without[2];
        const sim::Vector3 pd = pos(d);
        const sim::Vector3 pe = pos(e);
        const sim::Vector3 mid_de{(pd.x + pe.x) / 2, (pd.y + pe.y) / 2, (pd.z + pe.z) / 2};
        const auto dark_with = at(with, mid_de);
        const auto dark_without = at(without, mid_de);
        const f32 bright_with = dark_with[0] + dark_with[1] + dark_with[2];
        const f32 bright_without = dark_without[0] + dark_without[1] + dark_without[2];
        t.check(red_start > 0.3f && red_start > blue_start + 0.2f && blue_end > 0.3f &&
                    blue_end > red_end + 0.2f && bright_with < 0.1f && bright_without > 0.1f &&
                    inside_red > 0.2f && std::abs(outside_red) < 0.05f,
                fmt::format("Test 6: added near the start r{:+.2f} b{:+.2f}, near the end r{:+.2f} "
                            "b{:+.2f}; under the subtractive beam {:.2f} (without {:.2f}); red 0.7 "
                            "across {:+.2f}, 1.5 across {:+.2f}",
                            red_start, blue_start, red_end, blue_end, bright_with, bright_without,
                            inside_red, outside_red));
    }

    // Test 7: an engineer building draws retail's build beams.
    {
        run_lua(ctx,
                fmt::format("local brain = GetArmyBrain('ARMY_1')\n"
                            "brain:GiveResource('MASS', 5000)\n"
                            "brain:GiveResource('ENERGY', 50000)\n"
                            "local eng = CreateUnitHPR('uel0105', 'ARMY_1', {0}, 0, {1}, 0, 0, 0)\n"
                            "IssueBuildMobile({{eng}}, Vector({0}, 0, {2}), 'ueb1101', {{}})\n",
                            sx + 20, sz + 20, sz + 30));
        bool built = false;
        std::string seen_bp;
        for (int i = 0; i < 200 && !built; ++i) {
            next();
            for (const auto& drawn_beam : r.beam_renderer().drawn())
                if (drawn_beam.blueprint.find("build_beam") != std::string::npos) {
                    built = true;
                    seen_bp = drawn_beam.blueprint;
                }
        }
        t.check(built, fmt::format("Test 7: a building engineer draws retail's build beams ({})",
                                   built ? seen_bp : "none"));
        // Test 7b: and no line of the overlay's from it to what it builds
        bool lined = false;
        bool building = false;
        const Frame frame = drawn(r);
        for (const sim::EntityRecord& e : seen.cur().entities) {
            const sim::EntityRecord* target =
                e.is_unit && e.build_target_id != 0 ? seen.cur().find(e.build_target_id) : nullptr;
            if (!target) {
                continue;
            }
            building = true;
            const sim::Vector3 mid{(e.position.x + target->position.x) / 2,
                                   (e.position.y + target->position.y) / 2,
                                   (e.position.z + target->position.z) / 2};
            const auto at = screen_of(r, mid);
            for (const Quad& q : frame.overlay) {
                if (at && same_colour(q, 0.2f, 0.9f, 0.6f) &&
                    std::abs(q.x - (*at)[0]) <= q.w / 2 + 1 &&
                    std::abs(q.y - (*at)[1]) <= q.h / 2 + 1) {
                    lined = true;
                }
            }
        }
        t.check(building && !lined,
                fmt::format("Test 7b: no overlay line to the structure being built (building {}, "
                            "lined {})",
                            building, lined));
    }

    // Test 7c: two power generators side by side, one selected: adjacent,
    // and no line of the overlay's between them
    {
        const u32 p1 =
            spawn_unit(ctx, "__osc_bt_p1", "ueb1101", "ARMY_1", {sx - 20.5f, sz + 30.5f});
        const u32 p2 =
            spawn_unit(ctx, "__osc_bt_p2", "ueb1101", "ARMY_1", {sx - 18.5f, sz + 30.5f});
        next(2);
        const sim::EntityRecord* first = seen.cur().find(p1);
        bool adjacent = false;
        if (first) {
            for (u32 id : seen.cur().adjacent_of(*first)) {
                adjacent = adjacent || id == p2;
            }
        }
        const std::unordered_set<u32> selected{p1};
        shots.redraw(&selected);
        bool lined = false;
        for (const Quad& q : drawn(r).overlay) {
            lined = lined || same_colour(q, 1.0f, 0.6f, 0.1f);
        }
        t.check(
            adjacent && !lined,
            fmt::format("Test 7c: adjacent {}, an overlay line between them {}", adjacent, lined));
    }

    // Test 8: ARMY_2's beam in the fog (45 from ARMY_1's nearest) isn't
    // drawn.
    {
        (void)spawn_unit(ctx, "__osc_bt_f", "uel0105", "ARMY_2", {sx + 50, sz - 45}, kLift);
        (void)spawn_unit(ctx, "__osc_bt_g", "uel0105", "ARMY_2", {sx + 62, sz - 45}, kLift);
        run_lua(ctx,
                fmt::format("__osc_bt_fg = AttachBeamEntityToEntity(__osc_bt_f, -1, __osc_bt_g, "
                            "-1, 2, '{}')\n",
                            add_bp));
        const u32 fg = effect_id("__osc_bt_fg");
        r.set_fog_enabled(true);
        r.set_player_army(0);
        next();
        const bool hidden = !beam_of(r, fg);
        r.set_fog_enabled(false);
        next();
        const bool shown = beam_of(r, fg) != nullptr;
        t.check(hidden && shown,
                fmt::format("Test 8: ARMY_2's beam in the fog: hidden {}, with the "
                            "fog off shown {}",
                            hidden, shown));
    }

    spdlog::info("Beam test: {}/{} passed", t.pass, t.pass + t.fail);
}

} // namespace osc::test
