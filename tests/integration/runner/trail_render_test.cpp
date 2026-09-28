// --trail-render-test (M214b): FA's trails.
//
// A trail (CreateTrail) emits a segment every tick from where its point was
// to where it is, while the player could see it, catching up to
// min(missed, TrailLength, 24) segments when it can again. The segments
// outlive the trail for TrailLength ticks, each end aging from 0 to 1, and
// are drawn as a ribbon facing the camera, Size either side: the ramp
// texture by age, the repeat texture by distance, blended by a TPolyTrail
// technique, a negative SortOrder's under the water. On dry ground away
// from the starts, ARMY_1's engineers hang 3 above the ground carrying
// trails of the test's blueprints and move as the test warps them;
// ARMY_2's moves in the fog; a retail shell carries retail's.

#include "integration_tests.hpp"
#include "intel_probe.hpp"
#include "plate_fixtures.hpp"
#include "render_probe.hpp"

#include "renderer/renderer.hpp"
#include "renderer/trail_renderer.hpp"
#include "sim/entity.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
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
#include <optional>
#include <string>
#include <vector>

namespace osc::test {

namespace {

constexpr const char* kRoot = "/osc_trail_test";
constexpr f32 kLift = 3.0f;

using Segment = renderer::TrailRenderer::Drawn;

/// The segments of effect `id` drawn last frame, oldest first.
std::vector<Segment> segments_of(const renderer::Renderer& r, u32 id) {
    std::vector<Segment> out;
    for (const auto& s : r.trail_renderer().drawn())
        if (s.effect_id == id) out.push_back(s);
    return out;
}

bool near3(const sim::Vector3& a, const sim::Vector3& b, f32 tol = 0.05f) {
    return std::abs(a.x - b.x) < tol && std::abs(a.y - b.y) < tol && std::abs(a.z - b.z) < tol;
}

f32 distance(const sim::Vector3& a, const sim::Vector3& b) {
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) +
                     (a.z - b.z) * (a.z - b.z));
}

/// Whether consecutive segments join, each starting where the last ended.
bool joined(const std::vector<Segment>& segs) {
    for (size_t i = 1; i < segs.size(); ++i)
        if (!near3(segs[i - 1].end, segs[i].start)) return false;
    return true;
}

} // namespace

void test_trail_render(TestContext& ctx) {
    spdlog::info("=== TRAIL TEST: FA's trails (M214b) ===");
    Tally t;

    const auto spot = quiet_spot(ctx.sim);
    if (!spot) {
        t.check(false, "dry ground 60 from every unit");
        return;
    }
    const f32 sx = spot->x;
    const f32 sz = spot->z;
    spdlog::info("Trail test: the spot ({:.0f}, {:.0f})", sx, sz);

    // The test's blueprints: white and red textures (ramp and repeat), and
    // trails of them.
    const auto dir = std::filesystem::temp_directory_path() / "osc_trail_test";
    std::filesystem::create_directories(dir);
    write_dds(dir / "white.dds", 1,
              [](int, u32, u32) { return std::array<u8, 4>{255, 255, 255, 255}; });
    write_dds(dir / "red.dds", 1, [](int, u32, u32) { return std::array<u8, 4>{255, 0, 0, 255}; });
    // A ramp red while young (its first two columns), blue while old; a
    // repeat texture striped along the distance (its rows), white then black.
    write_dds(dir / "ramp.dds", 1, [](int, u32 column, u32) {
        return column < 2 ? std::array<u8, 4>{255, 0, 0, 255} : std::array<u8, 4>{0, 0, 255, 255};
    });
    write_dds(dir / "stripes.dds", 1, [](int, u32, u32 row) {
        return row < 2 ? std::array<u8, 4>{255, 255, 255, 255} : std::array<u8, 4>{0, 0, 0, 255};
    });
    const auto trail_bp = [&](const char* file, const std::string& fields, const char* ramp,
                              const char* repeat = "white") {
        std::ofstream(dir / file) << fmt::format("TrailEmitterBlueprint {{\n"
                                                 "    BlueprintId = '{}',\n"
                                                 "    {}\n"
                                                 "    RampTexture = '{}/{}.dds',\n"
                                                 "    RepeatTexture = '{}/{}.dds',\n"
                                                 "}}\n",
                                                 file, fields, kRoot, ramp, kRoot, repeat);
    };
    trail_bp("white.bp",
             "Lifetime = -1, TrailLength = 4, Size = 0.5, TextureRepeatRate = 0.25, "
             "BlendMode = 3, LODCutoff = 500,",
             "white");
    trail_bp("long.bp",
             "Lifetime = -1, TrailLength = 30, Size = 0.5, TextureRepeatRate = 1, BlendMode = 3, "
             "LODCutoff = 500,",
             "white");
    trail_bp("lod.bp",
             "Lifetime = -1, TrailLength = 4, Size = 0.5, TextureRepeatRate = 1, BlendMode = 3, "
             "LODCutoff = 70,",
             "white");
    trail_bp("life.bp",
             "Lifetime = 3, TrailLength = 30, Size = 0.5, TextureRepeatRate = 1, BlendMode = 3, "
             "LODCutoff = 500,",
             "white");
    trail_bp("ramp.bp",
             "Lifetime = -1, TrailLength = 8, Size = 1.5, TextureRepeatRate = 1, BlendMode = 3, "
             "LODCutoff = 500,",
             "ramp");
    trail_bp("stripes.bp",
             "Lifetime = -1, TrailLength = 8, Size = 1.5, TextureRepeatRate = 0.25, BlendMode = 3, "
             "LODCutoff = 500,",
             "white", "stripes");
    trail_bp("over.bp",
             "Lifetime = -1, TrailLength = 8, Size = 1.5, TextureRepeatRate = 1, BlendMode = 3, "
             "LODCutoff = 500,",
             "red");
    trail_bp("mod.bp",
             "Lifetime = -1, TrailLength = 8, Size = 1.5, TextureRepeatRate = 1, BlendMode = 1, "
             "LODCutoff = 500,",
             "white");
    trail_bp("under.bp",
             "Lifetime = -1, TrailLength = 8, Size = 1.5, TextureRepeatRate = 1, BlendMode = 3, "
             "SortOrder = -102, LODCutoff = 500,",
             "red");
    ctx.vfs.mount(kRoot, std::make_unique<vfs::DirectoryMount>(dir));
    const auto bp = [](const char* file) { return fmt::format("{}/{}", kRoot, file); };

    const auto effect_id = [&](const char* global) {
        run_lua(ctx, fmt::format("__osc_tt_id = {0} and {0}._c_effect_id or 0\n", global));
        lua_pushstring(ctx.L, "__osc_tt_id");
        lua_rawget(ctx.L, LUA_GLOBALSINDEX);
        const u32 id = lua_isnumber(ctx.L, -1) ? static_cast<u32>(lua_tonumber(ctx.L, -1)) : 0;
        lua_pop(ctx.L, 1);
        return id;
    };
    const auto height = [&](f32 x, f32 z) { return ctx.sim.terrain()->get_terrain_height(x, z); };
    const auto warp = [&](const char* global, f32 x, f32 z, f32 y) {
        run_lua(ctx, fmt::format("Warp({}, Vector({}, {}, {}))\n", global, x, y, z));
    };
    // Movers keep the height they started at, so a segment's length is how
    // far they went across.
    const auto lifted = [&](const Spot& s0) { return height(s0.x, s0.z) + kLift; };

    // ARMY_1's engineers, west of the spot; ARMY_2's far east of them.
    const Spot a0{sx - 25, sz + 50};
    const u32 a = spawn_unit(ctx, "__osc_tt_a", "uel0105", "ARMY_1", a0, kLift);
    const u32 c = spawn_unit(ctx, "__osc_tt_c", "uel0105", "ARMY_1", {sx - 25, sz + 30}, kLift);
    const u32 d = spawn_unit(ctx, "__osc_tt_d", "uel0105", "ARMY_1", {sx - 25, sz + 15}, kLift);
    const Spot l0{sx - 10, sz + 35};
    (void)spawn_unit(ctx, "__osc_tt_l", "uel0105", "ARMY_1", l0, kLift);
    const Spot u0{sx - 10, sz + 20};
    const u32 u = spawn_unit(ctx, "__osc_tt_u", "uel0105", "ARMY_1", u0, kLift);
    const Spot e0{sx - 10, sz + 5};
    const u32 e = spawn_unit(ctx, "__osc_tt_e", "uel0105", "ARMY_1", e0, kLift);
    const Spot g0{sx - 25, sz - 10};
    const Spot h0{sx - 25, sz - 22};
    const Spot k0{sx - 25, sz - 34};
    (void)spawn_unit(ctx, "__osc_tt_g", "uel0105", "ARMY_1", g0, kLift);
    (void)spawn_unit(ctx, "__osc_tt_h", "uel0105", "ARMY_1", h0, kLift);
    (void)spawn_unit(ctx, "__osc_tt_k", "uel0105", "ARMY_1", k0, kLift);
    const Spot f0{sx + 40, sz + 45};
    const u32 f = spawn_unit(ctx, "__osc_tt_f", "uel0105", "ARMY_2", f0, kLift);

    OffscreenShots shots(ctx);
    if (!shots.ok()) {
        t.check(false, "renderer init (no Vulkan?)");
        return;
    }
    renderer::Renderer& r = shots.renderer();
    (void)shots.shoot(*ctx.sim.terrain(), a0.x + 10, a0.z - 10, 80.0f);
    for (const char* tex : {"white", "red", "ramp", "stripes"})
        (void)r.texture_cache().get_blocking(fmt::format("{}/{}.dds", kRoot, tex));
    sim::WorldHistory seen;
    const auto step = [&] {
        ctx.sim.tick();
        shots.recapture();
        shots.redraw();
        seen.capture(ctx.sim);
    };
    const auto pos = [&](u32 id) {
        const sim::EntityRecord* rec = seen.cur().find(id);
        return rec ? rec->position : sim::Vector3{};
    };
    const auto anchor_of = [&](u32 fx) -> std::optional<sim::Vector3> {
        for (const sim::EffectRecord& rec : seen.cur().effects)
            if (rec.id == fx && rec.anchored) return rec.anchor;
        return std::nullopt;
    };
    const auto look_at = [&](f32 x, f32 z, f32 dist) {
        r.camera().set_target(x, z);
        r.camera().set_eye_distance(dist);
    };

    // Test 1: A's trail as A moves 2 east a tick: a segment a tick, joined,
    // the newest ending at A; the last 4 (TrailLength) ticks' drawn, each
    // end's age as the clock gives it; Size and distance coordinates the
    // blueprint's.
    run_lua(ctx,
            fmt::format("__osc_tt_afx = CreateTrail(__osc_tt_a, -1, 1, '{}')\n", bp("white.bp")));
    const u32 afx = effect_id("__osc_tt_afx");
    step(); // its first point
    f32 ax = a0.x;
    for (int i = 0; i < 6; ++i) {
        ax += 2.0f;
        warp("__osc_tt_a", ax, a0.z, lifted(a0));
        step();
    }
    {
        const auto segs = segments_of(r, afx);
        const Segment* last = segs.empty() ? nullptr : &segs.back();
        const bool newest = last && near3(last->end, pos(a)) &&
                            near3(last->start, {pos(a).x - 2, pos(a).y, pos(a).z}) &&
                            std::abs(last->t_start - 0.25f) < 1e-3f &&
                            std::abs(last->t_end) < 1e-3f;
        const bool coords = last && std::abs(last->u_end - 3.0f) < 1e-3f &&
                            std::abs(last->u_end - last->u_start - 0.5f) < 1e-3f;
        t.check(segs.size() == 4 && joined(segs) && newest && coords && last->size == 0.5f &&
                    last->blendmode == 3 && !last->under_water,
                fmt::format("Test 1: {} segments (4), joined {}; the newest ends at A, ages "
                            "{:.3f} -> {:.3f} (0.25 -> 0); distance {:.3f} (3), {:.3f} a segment "
                            "(0.5)",
                            segs.size(), joined(segs), last ? last->t_start : -1.0f,
                            last ? last->t_end : -1.0f, last ? last->u_end : -1.0f,
                            last ? last->u_end - last->u_start : -1.0f));
    }

    // Test 1b: A turns north: its new segment's start carries the last
    // one's direction (east), so the two meet.
    f32 az = a0.z + 2.0f;
    warp("__osc_tt_a", ax, az, lifted(a0));
    step();
    {
        const auto segs = segments_of(r, afx);
        const Segment* last = segs.empty() ? nullptr : &segs.back();
        const bool north = last && last->end.z - last->start.z > 1.9f;
        const bool carried = last && near3(last->start_tangent, {1, 0, 0}, 1e-3f);
        t.check(north && carried,
                fmt::format("Test 1b: A's turn north {}, its start along east {}", north, carried));
    }

    // Test 2: A stops; 5 ticks on, the moving segments are gone (a still
    // trail's are empty).
    for (int i = 0; i < 5; ++i) step();
    {
        const auto segs = segments_of(r, afx);
        const bool still = std::all_of(segs.begin(), segs.end(), [](const Segment& s) {
            return distance(s.start, s.end) < 1e-3f;
        });
        t.check(!segs.empty() && segs.size() <= 4 && still,
                fmt::format("Test 2: after 5 still ticks, {} segments, all empty: {}", segs.size(),
                            still));
    }

    // Test 3: OffsetEmitter(0, 0, 2) puts C's point 2 ahead of it along its
    // facing (+X); a second, (0, 0, 1), adds to it; POSITION_Z sets it.
    run_lua(ctx, "__osc_tt_c:SetOrientation({0, 0.70710678, 0, 0.70710678}, true)\n");
    run_lua(ctx, fmt::format("__osc_tt_cfx = CreateTrail(__osc_tt_c, -1, 1, '{}')"
                             ":OffsetEmitter(0, 0, 2)\n",
                             bp("white.bp")));
    const u32 cfx = effect_id("__osc_tt_cfx");
    step();
    const auto ahead = [&](f32 k) {
        const sim::EntityRecord* rec = seen.cur().find(c);
        if (!rec) return sim::Vector3{};
        const sim::Vector3 fwd = sim::quat_rotate(rec->orientation, {0, 0, 1});
        return sim::Vector3{rec->position.x + fwd.x * k, rec->position.y + fwd.y * k,
                            rec->position.z + fwd.z * k};
    };
    const auto c_fwd = [&] {
        const sim::EntityRecord* rec = seen.cur().find(c);
        return rec ? sim::quat_rotate(rec->orientation, {0, 0, 1}) : sim::Vector3{};
    };
    const std::optional<sim::Vector3> by2 = anchor_of(cfx);
    const bool faces_x = std::abs(c_fwd().x - 1.0f) < 1e-3f;
    run_lua(ctx, "__osc_tt_cfx:OffsetEmitter(0, 0, 1)\n");
    step();
    const std::optional<sim::Vector3> by3 = anchor_of(cfx);
    const auto c_segs = segments_of(r, cfx);
    const bool drawn_there = !c_segs.empty() && by3 && near3(c_segs.back().end, *by3);
    run_lua(ctx, "__osc_tt_cfx:SetEmitterParam('POSITION_Z', 1)\n");
    step();
    const std::optional<sim::Vector3> by1 = anchor_of(cfx);
    t.check(faces_x && by2 && near3(*by2, ahead(2)) && by3 && near3(*by3, ahead(3)) && by1 &&
                near3(*by1, ahead(1)) && drawn_there,
            fmt::format("Test 3: C faces +X {}; its point 2, 3, 1 ahead: {} {} {}; drawn there {}",
                        faces_x, by2 && near3(*by2, ahead(2)), by3 && near3(*by3, ahead(3)),
                        by1 && near3(*by1, ahead(1)), drawn_there));

    // Test 4: a trail on one of D's bones, turned a quarter about Y by a
    // rotator and offset 1 along it, follows the bone: the bone's point, 1
    // along its (turned) +Z.
    {
        auto* unit = static_cast<sim::Unit*>(ctx.sim.entity_registry().find(d));
        i32 bone = -1;
        for (i32 i = 1; i < 32 && unit && bone < 0; ++i)
            if (distance(unit->bone_world_position(i), unit->position()) > 0.2f) bone = i;
        run_lua(ctx, fmt::format("__osc_tt_drot = CreateRotator(__osc_tt_d, {0}, 'y', 90, 360)\n"
                                 "__osc_tt_drot:SetPrecedence(1000)\n"
                                 "__osc_tt_dfx = CreateTrail(__osc_tt_d, {0}, 1, '{1}')"
                                 ":OffsetEmitter(0, 0, 1)\n",
                                 bone, bp("white.bp")));
        const u32 dfx = effect_id("__osc_tt_dfx");
        // 360°/s: a quarter turn in 2.5 ticks. Its precedence puts it over the
        // engineer's own animator, which poses the bone too.
        for (int i = 0; i < 4; ++i) step();
        unit = static_cast<sim::Unit*>(ctx.sim.entity_registry().find(d));
        // The bone faces away from the unit now, or the test proves nothing.
        bool turned = false;
        if (unit && bone > 0) {
            const sim::Vector3 bf = unit->bone_world_forward(bone);
            const sim::Vector3 uf = sim::quat_rotate(unit->orientation(), {0, 0, 1});
            turned = bf.x * uf.x + bf.y * uf.y + bf.z * uf.z < 0.5f;
        }
        std::optional<sim::Vector3> want;
        if (unit && bone > 0) {
            const sim::Vector3 at = unit->bone_world_position(bone);
            const sim::Vector3 fwd = unit->bone_world_forward(bone);
            want = sim::Vector3{at.x + fwd.x, at.y + fwd.y, at.z + fwd.z};
        }
        const std::optional<sim::Vector3> got = anchor_of(dfx);
        t.check(turned && want && got && near3(*want, *got, 0.01f),
                fmt::format("Test 4: bone {} turned {}; its trail at ({:.3f}, {:.3f}, {:.3f}), the "
                            "bone 1 along ({:.3f}, {:.3f}, {:.3f})",
                            bone, turned, got ? got->x : 0.0f, got ? got->y : 0.0f,
                            got ? got->z : 0.0f, want ? want->x : 0.0f, want ? want->y : 0.0f,
                            want ? want->z : 0.0f));
    }

    // Test 5: A moves on and is destroyed: its trail outlives it, then fades
    // (4 ticks).
    for (int i = 0; i < 3; ++i) {
        ax += 2.0f;
        warp("__osc_tt_a", ax, az, lifted(a0));
        step();
    }
    run_lua(ctx, "__osc_tt_a:Destroy()\n");
    step();
    {
        const auto after = segments_of(r, afx);
        const bool moving = std::any_of(after.begin(), after.end(), [](const Segment& s) {
            return distance(s.start, s.end) > 1.0f;
        });
        const bool unit_gone = seen.cur().find(a) == nullptr;
        const bool forgotten = !r.trail_renderer().draws_effect(afx);
        for (int i = 0; i < 4; ++i) step();
        const size_t later = segments_of(r, afx).size();
        t.check(unit_gone && forgotten && moving && later == 0,
                fmt::format("Test 5: A gone {}, its trail forgotten {}, its segments drawn on {} "
                            "({}); 4 ticks later {} (0)",
                            unit_gone, forgotten, moving, after.size(), later));
    }

    // Test 6: ARMY_2's F, its trail 30 ticks long, the camera on it. Seen, it
    // emits (north); in the fog it emits on until its intel's next look (every
    // fifth tick from its first), then nothing; seen again, nothing until the
    // next look, which catches up 24 segments (of 30 missed) along F's real
    // path (south), starting afresh.
    {
        look_at(f0.x, f0.z, 60.0f);
        r.set_player_army(0);
        r.set_fog_enabled(false);
        run_lua(ctx, fmt::format("__osc_tt_ffx = CreateTrail(__osc_tt_f, -1, 2, '{}')\n",
                                 bp("long.bp")));
        const u32 ffx = effect_id("__osc_tt_ffx");
        Spot fp = f0;
        const auto move_f = [&](f32 dz) {
            fp.z += dz;
            warp("__osc_tt_f", fp.x, fp.z, lifted(f0));
            look_at(fp.x, fp.z, 60.0f);
            step();
        };
        step();                         // R0: its first point
        std::array<size_t, 42> count{}; // segments drawn after Rk
        for (int k = 1; k <= 5; ++k) {  // R1-R5 north (its first look at R1)
            move_f(0.4f);
            count[static_cast<size_t>(k)] = segments_of(r, ffx).size();
            if (k == 3) r.set_fog_enabled(true); // from R4 on, fogged
        }
        for (int k = 6; k <= 36; ++k) {            // then south
            if (k == 34) r.set_fog_enabled(false); // seen from R34 on
            move_f(-0.4f);
            count[static_cast<size_t>(k)] = segments_of(r, ffx).size();
        }
        const auto segs = segments_of(r, ffx);
        const bool south_steps = std::all_of(segs.begin(), segs.end(), [](const Segment& s) {
            return std::abs(distance(s.start, s.end) - 0.4f) < 1e-3f && s.end.z < s.start.z;
        });
        const bool afresh = !segs.empty() && segs.front().start_tangent.z < -0.99f;
        const bool at_f = !segs.empty() && near3(segs.back().end, pos(f));
        t.check(count[3] == 3 && count[5] == 5 && count[10] == 5,
                fmt::format("Test 6: fogged after R3, F's trail emits on until its look at R6: "
                            "{} {} {} segments (3 5 5)",
                            count[3], count[5], count[10]));
        t.check(count[35] == 0 && count[36] == 25 && joined(segs) && south_steps && afresh && at_f,
                fmt::format("Test 6: seen again at R34, nothing until the look at R36 ({}, 0), "
                            "then {} (25) joined {} along F's path {} to F {}, starting afresh "
                            "{}",
                            count[35], count[36], joined(segs), south_steps, at_f, afresh));

        // Test 6b: fogged again, it emits on to its look at R41 and stops;
        // out of view at R42, its looks restart, so back in view (and seen)
        // at R43 it looks at once and emits.
        r.set_fog_enabled(true);
        sim::Vector3 at_r40;
        for (int k = 37; k <= 41; ++k) {
            if (k == 41) at_r40 = pos(f);
            move_f(-0.4f);
        }
        const auto after_look = segments_of(r, ffx);
        const bool stopped = !after_look.empty() && near3(after_look.back().end, at_r40);
        fp.z -= 0.4f; // R42, the camera elsewhere
        warp("__osc_tt_f", fp.x, fp.z, lifted(f0));
        look_at(fp.x - 150, fp.z, 60.0f);
        step();
        r.set_fog_enabled(false);
        move_f(-0.4f); // R43, back on F
        const auto back = segments_of(r, ffx);
        const bool resumed = !back.empty() && near3(back.back().end, pos(f));
        t.check(stopped && resumed,
                fmt::format("Test 6b: fogged, it stops at its look (R41) {}; back in view it "
                            "looks at once (R43) {}",
                            stopped, resumed));
    }

    // Test 7: E's trail (LODCutoff 70) from 100 away emits nothing; from 40,
    // it catches up the 3 ticks it missed.
    {
        look_at(e0.x, e0.z, 100.0f);
        run_lua(ctx,
                fmt::format("__osc_tt_efx = CreateTrail(__osc_tt_e, -1, 1, '{}')\n", bp("lod.bp")));
        const u32 efx = effect_id("__osc_tt_efx");
        f32 ex = e0.x;
        const auto move_e = [&] {
            ex += 1.0f;
            warp("__osc_tt_e", ex, e0.z, lifted(e0));
            step();
        };
        step();
        size_t far = 0;
        for (int i = 0; i < 3; ++i) {
            move_e();
            far += segments_of(r, efx).size();
        }
        f32 cx = 0, cy = 0, cz = 0;
        r.camera().eye_position(cx, cy, cz);
        const f32 far_eye = distance({cx, cy, cz}, pos(e));
        look_at(e0.x, e0.z, 40.0f);
        move_e();
        const size_t near = segments_of(r, efx).size();
        t.check(far == 0 && near == 4,
                fmt::format("Test 7: from ~{:.0f} away, {} segments (0); from 40, {} (4: 3 "
                            "caught up)",
                            far_eye, far, near));

        // Test 7b: looking away from E (it's out of view) it emits nothing;
        // looking back, it catches up.
        look_at(e0.x, e0.z - 150, 40.0f);
        for (int i = 0; i < 3; ++i) move_e();
        const size_t away = segments_of(r, efx).size();
        look_at(ex, e0.z, 40.0f);
        move_e();
        const size_t back = segments_of(r, efx).size();
        t.check(away <= 1 && back == 4,
                fmt::format("Test 7b: out of view 3 ticks, {} segments left (<= 1); back in view "
                            "{} (4)",
                            away, back));

        // Test 10a: a trail it draws needs no overlay dot; one with no
        // blueprint keeps its dot.
        run_lua(ctx, fmt::format("__osc_tt_ufx = CreateTrail(__osc_tt_u, -1, 1, '{}')\n",
                                 bp("no_such.bp")));
        look_at((u0.x + ex) / 2, (u0.z + e0.z) / 2, 60.0f);
        step();
        const Frame frame = drawn(r);
        const auto dot_at = [&](const sim::Vector3& p) {
            const auto s = screen_of(r, p);
            return s && quad_at(frame.overlay, (*s)[0], (*s)[1], 4.0f, 4.0f) != nullptr;
        };
        const sim::Vector3 e_at = anchor_of(efx).value_or(sim::Vector3{});
        t.check(dot_at(pos(u)) && !dot_at(e_at) && r.trail_renderer().draws_effect(efx),
                fmt::format("Test 10a: an unreadable trail's dot {}; a drawn trail's {}",
                            dot_at(pos(u)), dot_at(e_at)));
    }

    // Test 8: in the frame. G's additive trail (8 long, 1.5 each side, its
    // ramp red while young and blue while old) adds red near its head and
    // blue further back, within its last 8 ticks and nowhere else, keeping
    // the ground's green (added, not laid over it). H's MODULATEINVERSE
    // trail darkens the ground; K's repeat texture stripes it along the
    // distance (every 4 units, 0.25 its rate). Drawn a tick later halfway
    // (the frame's interpolant 0.5), G's head reaches only where G is drawn.
    {
        look_at(g0.x + 14, h0.z, 75.0f);
        step();
        const ImageRGBA8 without = shots.grab();
        run_lua(ctx, fmt::format("__osc_tt_gfx = CreateTrail(__osc_tt_g, -1, 1, '{}')\n"
                                 "__osc_tt_hfx = CreateTrail(__osc_tt_h, -1, 1, '{}')\n"
                                 "__osc_tt_kfx = CreateTrail(__osc_tt_k, -1, 1, '{}')\n",
                                 bp("ramp.bp"), bp("mod.bp"), bp("stripes.bp")));
        for (int k = 1; k <= 12; ++k) {
            warp("__osc_tt_g", g0.x + 2.0f * static_cast<f32>(k), g0.z, lifted(g0));
            warp("__osc_tt_h", h0.x + static_cast<f32>(k), h0.z, lifted(h0));
            warp("__osc_tt_k", k0.x + static_cast<f32>(k), k0.z, lifted(k0));
            step();
        }
        const ImageRGBA8 with = shots.grab();
        const auto px = [&](const ImageRGBA8& img, const sim::Vector3& p) -> std::array<f32, 3> {
            const auto s = screen_of(r, p);
            if (!s || img.width == 0) return {0, 0, 0};
            const u32 x =
                static_cast<u32>(std::clamp((*s)[0], 0.0f, static_cast<f32>(img.width - 1)));
            const u32 y =
                static_cast<u32>(std::clamp((*s)[1], 0.0f, static_cast<f32>(img.height - 1)));
            const u8* q = &img.pixels[(static_cast<size_t>(y) * img.width + x) * 4];
            return {q[0] / 255.0f, q[1] / 255.0f, q[2] / 255.0f};
        };
        const auto on = [&](const Spot& s0, f32 k) {
            return sim::Vector3{s0.x + k, lifted(s0), s0.z};
        };
        // What a frame added at p over the frame without trails.
        const auto added = [&](const ImageRGBA8& img, const sim::Vector3& p) {
            const auto a = px(img, p);
            const auto b = px(without, p);
            return std::array<f32, 3>{a[0] - b[0], a[1] - b[1], a[2] - b[2]};
        };
        const auto none = [](const std::array<f32, 3>& c) {
            return std::abs(c[0]) < 0.05f && std::abs(c[1]) < 0.05f && std::abs(c[2]) < 0.05f;
        };
        // G's points are 2 apart, the one 2k along k ticks after it began:
        // 23 along is 1/16 of the way back (the ramp's red edge), 20 a
        // quarter, 10 seven eighths (its blue).
        const auto head = added(with, on(g0, 23));
        const auto young = added(with, on(g0, 20));
        const auto old = added(with, on(g0, 10));
        const auto tail = added(with, on(g0, 6));
        const auto ahead_of = added(with, on(g0, 26.5f));
        // Across the ribbon at 16 along: 1 out (inside its 1.5) and 2.5 (past).
        f32 ex = 0, ey = 0, ez = 0;
        r.camera().eye_position(ex, ey, ez);
        const sim::Vector3 mid = on(g0, 16);
        const sim::Vector3 fwd{r.camera().focus_x() - ex, r.camera().focus_y() - ey,
                               r.camera().focus_z() - ez};
        // cross(view axis, east): the way the ribbon spreads.
        sim::Vector3 side{0.0f, fwd.z, -fwd.y};
        const f32 n = std::sqrt(side.x * side.x + side.y * side.y + side.z * side.z);
        if (n > 0) side = {side.x / n, side.y / n, side.z / n};
        const auto across = [&](f32 k) {
            const auto c =
                added(with, {mid.x + side.x * k, mid.y + side.y * k, mid.z + side.z * k});
            return c[0] + c[2];
        };
        const f32 inside = across(1.0f);
        const f32 outside = across(2.5f);
        t.check(head[0] > 0.3f && head[2] < 0.1f && young[0] > 0.3f && young[2] < 0.1f &&
                    std::abs(young[1]) < 0.05f && old[2] > 0.3f && old[0] < 0.1f && none(tail) &&
                    none(ahead_of) && inside > 0.3f && std::abs(outside) < 0.05f,
                fmt::format("Test 8: added at its head r{:+.2f} b{:+.2f}; a quarter back r{:+.2f} "
                            "g{:+.2f} b{:+.2f}; 7/8 back r{:+.2f} b{:+.2f}; past its 8 ticks "
                            "{:+.2f}, ahead {:+.2f}; 1 across {:+.2f}, 2.5 across {:+.2f}",
                            head[0], head[2], young[0], young[1], young[2], old[0], old[2],
                            tail[0] + tail[2], ahead_of[0] + ahead_of[2], inside, outside));
        const auto bright = [](const std::array<f32, 3>& c) { return c[0] + c[1] + c[2]; };
        const f32 dark_with = bright(px(with, on(h0, 8)));
        const f32 dark_without = bright(px(without, on(h0, 8)));
        t.check(dark_with < 0.1f && dark_without > 0.1f,
                fmt::format("Test 8: under the MODULATEINVERSE trail {:.2f} (without {:.2f})",
                            dark_with, dark_without));
        // K's point k along is k - 1 from where it began: its distance
        // coordinate a quarter of that, white for the first half of each 1.
        const f32 white1 = bright(added(with, on(k0, 6)));
        const f32 black = bright(added(with, on(k0, 8)));
        const f32 white2 = bright(added(with, on(k0, 10)));
        t.check(white1 > 0.3f && white2 > 0.3f && black < 0.1f,
                fmt::format("Test 8: K's stripes along it: {:+.2f} {:+.2f} {:+.2f} (white, black, "
                            "white)",
                            white1, black, white2));

        // Test 8b: G jumps 6 (to 30); drawn halfway to it, its head is at 27.
        warp("__osc_tt_g", g0.x + 30.0f, g0.z, lifted(g0));
        step();
        ImageRGBA8 half;
        r.request_capture([&](ImageRGBA8 image) { half = std::move(image); });
        r.render(sim::FrameView(&seen.prev(), &seen.cur(), 0.5f), seen.events(), nullptr, ctx.L);
        const f32 behind = bright(added(half, on(g0, 25.5f)));
        const f32 beyond = bright(added(half, on(g0, 28.5f)));
        t.check(behind > 0.3f && std::abs(beyond) < 0.05f,
                fmt::format("Test 8b: halfway into a tick, 1.5 behind the drawn head {:+.2f}, 1.5 "
                            "beyond it {:+.2f}",
                            behind, beyond));
    }

    // Test 9: a negative SortOrder's trail draws under the water: where
    // there's water, water covers it, and not a SortOrder 0 one beside it.
    {
        const map::Terrain& terrain = *ctx.sim.terrain();
        std::optional<Spot> wet;
        const f32 w = terrain.water_elevation();
        // A 20 x 16 patch 2 under the water, on an 8-unit lattice.
        for (u32 z = 32; z + 32 < terrain.map_height() && !wet; z += 8)
            for (u32 x = 32; x + 32 < terrain.map_width() && !wet; x += 8) {
                bool deep = terrain.has_water();
                for (int dz = -8; dz <= 8 && deep; dz += 4)
                    for (int dx = -4; dx <= 16 && deep; dx += 4)
                        deep = terrain.get_terrain_height(
                                   static_cast<f32>(static_cast<int>(x) + dx),
                                   static_cast<f32>(static_cast<int>(z) + dz)) < w - 2.0f;
                if (deep) wet = Spot{static_cast<f32>(x), static_cast<f32>(z)};
            }
        (void)spawn_unit(ctx, "__osc_tt_i", "uel0105", "ARMY_1",
                         {wet ? wet->x : sx + 20, wet ? wet->z - 4 : sz - 20}, 0);
        (void)spawn_unit(ctx, "__osc_tt_j", "uel0105", "ARMY_1",
                         {wet ? wet->x : sx + 20, wet ? wet->z + 4 : sz - 10}, 0);
        const Spot i0{wet ? wet->x : sx + 20, wet ? wet->z - 4 : sz - 20};
        const Spot j0{wet ? wet->x : sx + 20, wet ? wet->z + 4 : sz - 10};
        const f32 y = wet ? w + 1.0f : height(i0.x, i0.z) + kLift;
        warp("__osc_tt_i", i0.x, i0.z, y);
        warp("__osc_tt_j", j0.x, j0.z, y);
        look_at(i0.x + 6, wet ? wet->z : (i0.z + j0.z) / 2, 60.0f);
        step();
        const ImageRGBA8 without = shots.grab();
        run_lua(ctx, fmt::format("__osc_tt_ifx = CreateTrail(__osc_tt_i, -1, 1, '{}')\n"
                                 "__osc_tt_jfx = CreateTrail(__osc_tt_j, -1, 1, '{}')\n",
                                 bp("under.bp"), bp("over.bp")));
        const u32 ifx = effect_id("__osc_tt_ifx");
        const u32 jfx = effect_id("__osc_tt_jfx");
        for (int k = 1; k <= 10; ++k) {
            warp("__osc_tt_i", i0.x + static_cast<f32>(k), i0.z, y);
            warp("__osc_tt_j", j0.x + static_cast<f32>(k), j0.z, y);
            step();
        }
        const ImageRGBA8 with = shots.grab();
        const auto isegs = segments_of(r, ifx);
        const auto jsegs = segments_of(r, jfx);
        const bool passes = !isegs.empty() && !jsegs.empty() && isegs.back().under_water &&
                            !jsegs.back().under_water;
        f32 red_i = 0;
        f32 red_j = 0;
        if (wet) {
            const auto red_at = [&](const Spot& s0) {
                const sim::Vector3 p{s0.x + 6, y, s0.z};
                const auto s = screen_of(r, p);
                if (!s || with.width == 0) return 0.0f;
                const u32 x =
                    static_cast<u32>(std::clamp((*s)[0], 0.0f, static_cast<f32>(with.width - 1)));
                const u32 yy =
                    static_cast<u32>(std::clamp((*s)[1], 0.0f, static_cast<f32>(with.height - 1)));
                const size_t at = (static_cast<size_t>(yy) * with.width + x) * 4;
                return (with.pixels[at] - without.pixels[at]) / 255.0f;
            };
            red_i = red_at(i0);
            red_j = red_at(j0);
        }
        spdlog::info("Trail test: water {}",
                     wet ? fmt::format("at ({:.0f}, {:.0f})", wet->x, wet->z)
                         : std::string("none on this map"));
        t.check(passes && (!wet || (red_i > 0.05f && red_j > red_i + 0.1f)),
                fmt::format("Test 9: SortOrder -102's under the water {}, 0's over it {}; red "
                            "under the water {:+.2f}, over it {:+.2f}",
                            !isegs.empty() && isegs.back().under_water,
                            !jsegs.empty() && !jsegs.back().under_water, red_i, red_j));
    }

    // Test 10b: a retail shell (the UEF Gauss cannon's) draws its polytrail
    // from retail's blueprint.
    {
        const Spot v0{sx + 10, sz - 20};
        (void)spawn_unit(ctx, "__osc_tt_v", "uel0105", "ARMY_1", v0, kLift);
        look_at(v0.x + 10, v0.z, 60.0f);
        std::string retail;
        for (int i = 0; i < 60 && retail.empty(); ++i) {
            if (i % 10 == 0)
                run_lua(ctx, "local p = __osc_tt_v:CreateProjectile("
                             "'/projectiles/TDFGauss01/TDFGauss01_proj.bp', 0, 1, 0, 1, 0, 0)\n"
                             "if p then p:SetVelocity(1, 0.2, 0) end\n");
            step();
            for (const auto& s : r.trail_renderer().drawn())
                if (s.blueprint.rfind("/effects/emitters/", 0) == 0 &&
                    r.trail_renderer().draws_effect(s.effect_id))
                    retail = s.blueprint;
        }
        t.check(!retail.empty(), fmt::format("Test 10b: a Gauss shell's polytrail drawn ({})",
                                             retail.empty() ? "none" : retail));
    }

    // Test 11: a trail with Lifetime 3 stops emitting 3 ticks after it
    // appeared (its first tick only finds where it is).
    {
        look_at(l0.x, l0.z, 60.0f);
        run_lua(ctx, fmt::format("__osc_tt_lfx = CreateTrail(__osc_tt_l, -1, 1, '{}')\n",
                                 bp("life.bp")));
        const u32 lfx = effect_id("__osc_tt_lfx");
        for (int k = 0; k < 8; ++k) {
            warp("__osc_tt_l", l0.x + static_cast<f32>(k), l0.z, lifted(l0));
            step();
        }
        const size_t n = segments_of(r, lfx).size();
        t.check(n == 2, fmt::format("Test 11: Lifetime 3's trail emitted {} segments (2)", n));
    }

    spdlog::info("Trail test: {}/{} passed", t.pass, t.pass + t.fail);
}

} // namespace osc::test
