// --particle-render-test (M214c): FA's particles.
//
// Once a tick an emitter emits its EmitRate's whole part (the fraction
// carried), reading its curves at its clock; each particle then moves by
// itself from its spawn state (P + V·t + ½A·t², or with drag), sized from
// StartSize to EndSize over its life, drawn as a quad facing the camera,
// flat on the world or along its motion, its texture (animated: frames and
// strips) times its ramp by age, blended by its TRamp technique. Emitters
// follow their bone (attached) or stay where they were made (At-); a
// SortOrder below -101's draw under the water; EmitIfVisible ones emit only
// where the player could see them, catching up after. On dry ground away
// from the starts, ARMY_1's engineers hang 3 above the ground carrying the
// test's emitters and move as the test warps them.

#include "integration_tests.hpp"
#include "support/temp_path.hpp"
#include "intel_probe.hpp"
#include "plate_fixtures.hpp"
#include "render_probe.hpp"

#include "renderer/effect_blueprint_file.hpp"
#include "renderer/particle_system.hpp"
#include "renderer/renderer.hpp"
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

constexpr const char* kRoot = "/osc_particle_test";
constexpr f32 kLift = 3.0f;

using Particle = renderer::ParticleSystem::Drawn;

/// Effect `id`'s particles drawn last frame, youngest first.
std::vector<Particle> particles_of(const renderer::Renderer& r, u32 id) {
    std::vector<Particle> out;
    for (const auto& p : r.particle_system().drawn())
        if (p.effect_id == id) out.push_back(p);
    std::stable_sort(out.begin(), out.end(),
                     [](const Particle& a, const Particle& b) { return a.age < b.age; });
    return out;
}

bool near(f32 a, f32 b, f32 tol = 1e-3f) {
    return std::abs(a - b) < tol;
}

bool near3(const sim::Vector3& a, const sim::Vector3& b, f32 tol = 1e-3f) {
    return near(a.x, b.x, tol) && near(a.y, b.y, tol) && near(a.z, b.z, tol);
}

f32 length(const sim::Vector3& a) {
    return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
}

/// A constant curve.
std::string curve(const char* name, f32 y) {
    return fmt::format("    {} = {{ Keys = {{ {{ x = 0, y = {}, z = 0 }} }} }},\n", name, y);
}

} // namespace

void test_particle_render(TestContext& ctx) {
    spdlog::info("=== PARTICLE TEST: FA's particles (M214c) ===");
    Tally t;

    const auto spot = quiet_spot(ctx.sim);
    if (!spot) {
        t.check(false, "dry ground 60 from every unit");
        return;
    }
    const f32 sx = spot->x;
    const f32 sz = spot->z;
    spdlog::info("Particle test: the spot ({:.0f}, {:.0f})", sx, sz);

    // Textures: white, red; a ramp red while young (its first two columns)
    // and blue while old.
    const TempDir scratch("osc_particle_test");
    const auto& dir = scratch.path();
    write_dds(dir / "white.dds", 1,
              [](int, u32, u32) { return std::array<u8, 4>{255, 255, 255, 255}; });
    write_dds(dir / "red.dds", 1, [](int, u32, u32) { return std::array<u8, 4>{255, 0, 0, 255}; });
    write_dds(dir / "ramp.dds", 1, [](int, u32 column, u32) {
        return column < 2 ? std::array<u8, 4>{255, 0, 0, 255} : std::array<u8, 4>{0, 0, 255, 255};
    });
    // The test's emitters: seen or not (EmitIfVisible false) unless said,
    // emitting at their tick's start, no water, textures white unless said.
    const auto emitter_bp = [&](const char* file, const std::string& body,
                                const char* ramp = "white") {
        std::ofstream(dir / file) << fmt::format(
            "EmitterBlueprint {{\n"
            "    Lifetime = -1, Repeattime = 10, LODCutoff = 500,\n"
            "    EmitIfVisible = false, InterpolateEmission = false, SnapToWaterline = false,\n"
            "    LocalVelocity = false,\n"
            "    Texture = '{0}/white.dds', RampTexture = '{0}/{1}.dds',\n"
            "{2}}}\n",
            kRoot, ramp, body);
    };
    emitter_bp("count.bp", curve("EmitRateCurve", 3) + curve("LifetimeCurve", 4));
    emitter_bp("life.bp", curve("EmitRateCurve", 3) + curve("LifetimeCurve", 10));
    emitter_bp("move.bp", "    Gravity = true,\n" + curve("EmitRateCurve", 1) +
                              curve("LifetimeCurve", 20) + curve("XDirectionCurve", 1) +
                              curve("VelocityCurve", 2) + curve("YAccelCurve", -0.5f));
    emitter_bp("drag.bp", "    ParticleResistance = true,\n" + curve("EmitRateCurve", 1) +
                              curve("LifetimeCurve", 20) + curve("XDirectionCurve", 1) +
                              curve("VelocityCurve", 2) + curve("ResistanceCurve", 0.5f));
    emitter_bp("local.bp", "    LocalVelocity = true, LocalAcceleration = true,\n" +
                               curve("EmitRateCurve", 1) + curve("LifetimeCurve", 20) +
                               curve("ZDirectionCurve", 1) + curve("VelocityCurve", 1) +
                               curve("ZAccelCurve", 1));
    emitter_bp("still.bp", curve("EmitRateCurve", 1) + curve("LifetimeCurve", 20));
    emitter_bp("spread.bp", "    InterpolateEmission = true,\n" + curve("EmitRateCurve", 4) +
                                curve("LifetimeCurve", 20));
    emitter_bp("bill.bp", curve("EmitRateCurve", 1) + curve("LifetimeCurve", 4) +
                              curve("StartSizeCurve", 1) + curve("EndSizeCurve", 3) +
                              curve("InitialRotationCurve", 90));
    emitter_bp("flat.bp", "    Flat = true,\n" + curve("EmitRateCurve", 1) +
                              curve("LifetimeCurve", 4) + curve("StartSizeCurve", 1) +
                              curve("EndSizeCurve", 1));
    // (Its own velocity, along the world's +Z, gives way to the bone's axis.)
    emitter_bp("tobone.bp", "    AlignToBone = true,\n" + curve("EmitRateCurve", 1) +
                                curve("LifetimeCurve", 4) + curve("StartSizeCurve", 1) +
                                curve("EndSizeCurve", 1) + curve("ZDirectionCurve", 1) +
                                curve("VelocityCurve", 3));
    emitter_bp("scatter.bp",
               curve("EmitRateCurve", 20) + curve("LifetimeCurve", 5) + curve("SizeCurve", 10));
    emitter_bp("align.bp", "    AlignRotation = true,\n" + curve("EmitRateCurve", 1) +
                               curve("LifetimeCurve", 4) + curve("StartSizeCurve", 1) +
                               curve("EndSizeCurve", 1) + curve("XDirectionCurve", 1) +
                               curve("VelocityCurve", 1));
    emitter_bp("anim.bp", "    TextureFramecount = 4, TextureStripcount = 2,\n" +
                              curve("EmitRateCurve", 1) + curve("LifetimeCurve", 8) +
                              curve("FrameRateCurve", 2) + curve("TextureSelectionCurve", 1) +
                              curve("RampSelectionCurve", 0.75f));
    emitter_bp("snap.bp", "    SnapToWaterline = true,\n" + curve("EmitRateCurve", 1) +
                              curve("LifetimeCurve", 20));
    emitter_bp("snapunder.bp", "    SnapToWaterline = true, SortOrder = -102,\n" +
                                   curve("EmitRateCurve", 1) + curve("LifetimeCurve", 20));
    emitter_bp("onwater.bp", "    OnlyEmitOnWater = true,\n" + curve("EmitRateCurve", 1) +
                                 curve("LifetimeCurve", 20));
    emitter_bp("seen.bp", "    EmitIfVisible = true,\n" + curve("EmitRateCurve", 1) +
                              curve("LifetimeCurve", 30));
    emitter_bp("unmade.bp", "    EmitIfVisible = true, CreateIfVisible = true,\n" +
                                curve("EmitRateCurve", 1) + curve("LifetimeCurve", 30));
    emitter_bp("lod.bp", "    EmitIfVisible = true, LODCutoff = 70,\n" + curve("EmitRateCurve", 1) +
                             curve("LifetimeCurve", 10));
    const std::string big = curve("EmitRateCurve", 1) + curve("LifetimeCurve", 30) +
                            curve("StartSizeCurve", 3) + curve("EndSizeCurve", 3);
    emitter_bp("add.bp", "    Blendmode = 3,\n" + big, "ramp");
    emitter_bp("mod.bp", "    Blendmode = 1,\n" + big);
    emitter_bp("under.bp", "    Blendmode = 3, SortOrder = -102,\n" + big, "red");
    emitter_bp("over.bp", "    Blendmode = 3,\n" + big, "red");
    emitter_bp("lowoff.bp", "    LowFidelity = false,\n" + curve("EmitRateCurve", 1) +
                                curve("LifetimeCurve", 20));
    ctx.vfs.mount(kRoot, std::make_unique<vfs::DirectoryMount>(dir));
    const auto bp = [](const char* file) { return fmt::format("{}/{}", kRoot, file); };

    const auto effect_id = [&](const char* global) {
        run_lua(ctx, fmt::format("__osc_pt_id = {0} and {0}._c_effect_id or 0\n", global));
        lua_pushstring(ctx.L, "__osc_pt_id");
        lua_rawget(ctx.L, LUA_GLOBALSINDEX);
        const u32 id = lua_isnumber(ctx.L, -1) ? static_cast<u32>(lua_tonumber(ctx.L, -1)) : 0;
        lua_pop(ctx.L, 1);
        return id;
    };
    const auto height = [&](f32 x, f32 z) { return ctx.sim.terrain()->get_terrain_height(x, z); };
    const auto lifted = [&](const Spot& s0) { return height(s0.x, s0.z) + kLift; };
    const auto warp = [&](const char* global, f32 x, f32 z, f32 y) {
        run_lua(ctx, fmt::format("Warp({}, Vector({}, {}, {}))\n", global, x, y, z));
    };
    // An emitter `kind` ("OnEntity", "AtEntity") of `bp` on `unit`.
    const auto make = [&](const char* global, const char* kind, const char* unit,
                          const std::string& path) {
        run_lua(ctx, fmt::format("{} = CreateEmitter{}({}, 1, '{}')\n", global, kind, unit, path));
        return effect_id(global);
    };

    const Spot a0{sx - 25, sz + 60};
    const Spot b0{sx - 25, sz + 45};
    const Spot c0{sx - 25, sz + 30};
    const Spot d0{sx - 10, sz + 60};
    const Spot e0{sx - 10, sz + 45};
    const Spot q0{sx + 5, sz + 60};
    const Spot c2{sx - 10, sz + 30};
    const Spot g0{sx - 25, sz + 5};
    const Spot h0{sx - 25, sz - 10};
    const Spot l0{sx + 20, sz - 20};
    const Spot f0{sx + 45, sz - 30};
    (void)spawn_unit(ctx, "__osc_pt_a", "uel0105", "ARMY_1", a0, kLift);
    const u32 b = spawn_unit(ctx, "__osc_pt_b", "uel0105", "ARMY_1", b0, kLift);
    const u32 c = spawn_unit(ctx, "__osc_pt_c", "uel0105", "ARMY_1", c0, kLift);
    const u32 d = spawn_unit(ctx, "__osc_pt_d", "uel0105", "ARMY_1", d0, kLift);
    (void)spawn_unit(ctx, "__osc_pt_e", "uel0105", "ARMY_1", e0, kLift);
    (void)spawn_unit(ctx, "__osc_pt_q", "uel0105", "ARMY_1", q0, kLift);
    const u32 c2_id = spawn_unit(ctx, "__osc_pt_c2", "uel0105", "ARMY_1", c2, kLift);
    const u32 g = spawn_unit(ctx, "__osc_pt_g", "uel0105", "ARMY_1", g0, kLift);
    (void)spawn_unit(ctx, "__osc_pt_h", "uel0105", "ARMY_1", h0, kLift);
    const u32 l = spawn_unit(ctx, "__osc_pt_l", "uel0105", "ARMY_1", l0, kLift);
    (void)spawn_unit(ctx, "__osc_pt_f", "uel0105", "ARMY_2", f0, kLift);

    OffscreenShots shots(ctx);
    if (!shots.ok()) {
        t.check(false, "renderer init (no Vulkan?)");
        return;
    }
    renderer::Renderer& r = shots.renderer();
    (void)shots.shoot(*ctx.sim.terrain(), sx, sz + 30, 80.0f);
    for (const char* tex : {"white", "red", "ramp"})
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
    const auto look_at = [&](f32 x, f32 z, f32 dist) {
        r.camera().set_target(x, z);
        r.camera().set_eye_distance(dist);
    };

    // Test 1: 3 a tick, each living 4 ticks, born at its tick's start and
    // drawn a tick on: after 5 ticks, the last three ticks' 9, aged 1-3.
    // One whose effect goes after 2 ticks stops, its 6 living on.
    const u32 count_fx = make("__osc_pt_count", "AtEntity", "__osc_pt_a", bp("count.bp"));
    const u32 life_fx = make("__osc_pt_life", "AtEntity", "__osc_pt_a", bp("life.bp"));
    for (int i = 0; i < 2; ++i) step();
    run_lua(ctx, "__osc_pt_life:Destroy()\n");
    for (int i = 0; i < 3; ++i) step();
    {
        const auto ps = particles_of(r, count_fx);
        const bool ages = ps.size() == 9 && near(ps.front().age, 1.0f) &&
                          near(ps.back().age, 3.0f) && near(ps.front().lifetime, 4.0f);
        const size_t lived = particles_of(r, life_fx).size();
        t.check(ages && lived == 6,
                fmt::format("Test 1: {} particles (9), aged {:.2f}-{:.2f} (1-3); the ended "
                            "emitter's {} live on (6)",
                            ps.size(), ps.empty() ? 0.0f : ps.front().age,
                            ps.empty() ? 0.0f : ps.back().age, lived));
    }

    // Test 2: moving 2 a tick along +X, falling 0.5 a tick² and gravity's
    // 0.02: P + V·t + ½A·t². With drag 0.5: P + 4(1 − e^(−t/2)) along X.
    const u32 move_fx = make("__osc_pt_move", "AtEntity", "__osc_pt_b", bp("move.bp"));
    const u32 drag_fx = make("__osc_pt_drag", "AtEntity", "__osc_pt_b", bp("drag.bp"));
    for (int i = 0; i < 4; ++i) step();
    {
        const sim::Vector3 p = pos(b);
        const auto moving = particles_of(r, move_fx);
        const auto dragged = particles_of(r, drag_fx);
        const auto at = [](const std::vector<Particle>& ps, f32 age) -> const Particle* {
            for (const Particle& q : ps)
                if (near(q.age, age)) return &q;
            return nullptr;
        };
        const Particle* m = at(moving, 3.0f);
        const Particle* dr = at(dragged, 3.0f);
        const bool falls = m && near3(m->center, {p.x + 6.0f, p.y - 0.5f * 0.52f * 9.0f, p.z});
        const bool drags =
            dr && near3(dr->center, {p.x + 4.0f * (1.0f - std::exp(-1.5f)), p.y, p.z});
        t.check(falls && drags,
                fmt::format("Test 2: aged 3, moved ({:.3f}, {:.3f}) from its start (6, -2.34); "
                            "with drag {:.3f} (3.107)",
                            m ? m->center.x - p.x : 0.0f, m ? m->center.y - p.y : 0.0f,
                            dr ? dr->center.x - p.x : 0.0f));
    }

    // Test 3: on C turned to face +X, a velocity and acceleration along its
    // +Z go along the world's +X (LocalVelocity, LocalAcceleration).
    run_lua(ctx, "__osc_pt_c:SetOrientation({0, 0.70710678, 0, 0.70710678}, true)\n");
    step();
    const u32 local_fx = make("__osc_pt_local", "OnEntity", "__osc_pt_c", bp("local.bp"));
    for (int i = 0; i < 4; ++i) step();
    {
        const sim::Vector3 p = pos(c);
        bool along = false;
        for (const Particle& q : particles_of(r, local_fx))
            if (near(q.age, 3.0f)) along = near3(q.center, {p.x + 3.0f + 4.5f, p.y, p.z}, 1e-2f);
        t.check(along, fmt::format("Test 3: C's local +Z moves its particles along +X: {}", along));
    }

    // Test 3b: attached to one of C2's bones, turned a quarter by a rotator
    // (over the engineer's own animator), they go along the bone's +Z.
    {
        auto* unit = static_cast<sim::Unit*>(ctx.sim.entity_registry().find(c2_id));
        i32 bone = -1;
        for (i32 i = 1; i < 32 && unit && bone < 0; ++i)
            if (length({unit->bone_world_position(i).x - unit->position().x,
                        unit->bone_world_position(i).y - unit->position().y,
                        unit->bone_world_position(i).z - unit->position().z}) > 0.2f)
                bone = i;
        run_lua(ctx, fmt::format("__osc_pt_rot = CreateRotator(__osc_pt_c2, {}, 'y', 90, 360)\n"
                                 "__osc_pt_rot:SetPrecedence(1000)\n",
                                 bone));
        for (int i = 0; i < 4; ++i) step();
        run_lua(ctx,
                fmt::format("__osc_pt_bone = CreateAttachedEmitter(__osc_pt_c2, {}, 1, '{}')\n",
                            bone, bp("local.bp")));
        const u32 bone_fx = effect_id("__osc_pt_bone");
        for (int i = 0; i < 4; ++i) step();
        unit = static_cast<sim::Unit*>(ctx.sim.entity_registry().find(c2_id));
        bool turned = false;
        bool along_bone = false;
        if (unit && bone > 0) {
            const sim::Vector3 at = unit->bone_world_position(bone);
            const sim::Vector3 fwd = unit->bone_world_forward(bone);
            const sim::Vector3 uf = sim::quat_rotate(unit->orientation(), {0, 0, 1});
            turned = fwd.x * uf.x + fwd.y * uf.y + fwd.z * uf.z < 0.5f;
            for (const Particle& q : particles_of(r, bone_fx))
                if (near(q.age, 3.0f))
                    along_bone = near3(
                        q.center, {at.x + fwd.x * 7.5f, at.y + fwd.y * 7.5f, at.z + fwd.z * 7.5f},
                        1e-2f);
        }
        t.check(turned && along_bone,
                fmt::format("Test 3b: bone {} turned {}; its particles along it {}", bone, turned,
                            along_bone));
    }

    // Test 4: D moves 2 east a tick. An attached emitter's particles are
    // born where D was (its tick's start); an At-emitter's where it was made.
    f32 dx = d0.x;
    const sim::Vector3 made_at = pos(d);
    const u32 on_fx = make("__osc_pt_on", "OnEntity", "__osc_pt_d", bp("still.bp"));
    const u32 at_fx = make("__osc_pt_at", "AtEntity", "__osc_pt_d", bp("still.bp"));
    sim::Vector3 was;
    for (int i = 0; i < 4; ++i) {
        was = pos(d);
        dx += 2.0f;
        warp("__osc_pt_d", dx, d0.z, lifted(d0));
        step();
    }
    {
        const auto on = particles_of(r, on_fx);
        const auto at = particles_of(r, at_fx);
        const bool follows = !on.empty() && near3(on.front().center, was, 1e-2f);
        const bool stays = !at.empty() && std::all_of(at.begin(), at.end(), [&](const Particle& q) {
            return near3(q.center, made_at, 1e-2f);
        });
        t.check(follows && stays,
                fmt::format("Test 4: attached, born where D was {}; At-, where it was made {}",
                            follows, stays));
    }

    // Test 5: InterpolateEmission spreads 4 a tick over the tick: E moving
    // 4 east a tick, its newest four born a quarter tick apart, a unit apart.
    f32 ex = e0.x;
    const u32 spread_fx = make("__osc_pt_spread", "OnEntity", "__osc_pt_e", bp("spread.bp"));
    sim::Vector3 start;
    for (int i = 0; i < 3; ++i) {
        start = {ex, lifted(e0), e0.z};
        ex += 4.0f;
        warp("__osc_pt_e", ex, e0.z, lifted(e0));
        step();
    }
    {
        const auto ps = particles_of(r, spread_fx);
        bool spread = ps.size() >= 4;
        for (size_t i = 0; spread && i < 4; ++i) {
            const Particle& q = ps[i];
            // Born c into its tick: age 1 − c, at start + 4c.
            const f32 cursor = 1.0f - q.age;
            spread = near(cursor, 0.75f - 0.25f * static_cast<f32>(i), 1e-3f) &&
                     near3(q.center, {start.x + 4.0f * cursor, start.y, start.z}, 1e-2f);
        }
        t.check(spread, fmt::format("Test 5: a tick's four spread along its path: {}", spread));
    }

    // Test 6: quads. A billboard turned 90° spans the camera's up across,
    // growing from 1 to 3 over its life; a flat one the world's X and Z; an
    // aligned one lies along its motion (+X).
    const u32 bill_fx = make("__osc_pt_bill", "AtEntity", "__osc_pt_q", bp("bill.bp"));
    const u32 flat_fx = make("__osc_pt_flat", "AtEntity", "__osc_pt_q", bp("flat.bp"));
    const u32 align_fx = make("__osc_pt_align", "AtEntity", "__osc_pt_q", bp("align.bp"));
    // (AlignToBone on C, which faces +X: along its +Z, staying put.)
    const u32 tobone_fx = make("__osc_pt_tobone", "OnEntity", "__osc_pt_c", bp("tobone.bp"));
    for (int i = 0; i < 3; ++i) step();
    {
        const auto v = r.camera().view();
        const sim::Vector3 up{v[1], v[5], v[9]};
        bool billboard = false;
        for (const Particle& q : particles_of(r, bill_fx))
            if (near(q.age, 2.0f)) {
                const f32 size = 1.0f + 2.0f * 2.0f / 4.0f; // 2
                billboard = near3(q.axis_x, {up.x * size, up.y * size, up.z * size}, 1e-3f) &&
                            near(length(q.axis_y), size);
            }
        const auto flat = particles_of(r, flat_fx);
        const bool lies_flat = !flat.empty() && near3(flat.front().axis_x, {1, 0, 0}) &&
                               near3(flat.front().axis_y, {0, 0, 1});
        const auto aligned = particles_of(r, align_fx);
        const bool along = !aligned.empty() && near3(aligned.front().axis_y, {1, 0, 0}) &&
                           near(aligned.front().axis_x.x, 0.0f);
        bool to_bone = false;
        for (const Particle& q : particles_of(r, tobone_fx))
            if (near(q.age, 2.0f))
                to_bone = near3(q.axis_y, {1, 0, 0}) && near3(q.center, pos(c), 1e-2f);
        t.check(
            billboard && lies_flat && along && to_bone,
            fmt::format("Test 6: billboard {}, flat {}, aligned {}, along the bone and still {}",
                        billboard, lies_flat, along, to_bone));
    }

    // Test 7: 4 frames across and 2 strips down, 2 frames a tick, strip 1:
    // at age 1 frame 2 (u 0.5), v from 0.5; the ramp read at 0.75 down.
    const u32 anim_fx = make("__osc_pt_anim", "AtEntity", "__osc_pt_q", bp("anim.bp"));
    step();
    {
        const auto ps = particles_of(r, anim_fx);
        const bool frames = !ps.empty() && near(ps.front().uv[0], 0.5f) &&
                            near(ps.front().uv[1], 0.25f) && near(ps.front().uv[2], 0.5f) &&
                            near(ps.front().uv[3], 0.5f) && near(ps.front().ramp_v, 0.75f);
        t.check(frames, fmt::format("Test 7: frame and strip {} ({:.2f} {:.2f} {:.2f} {:.2f}, "
                                    "ramp {:.2f})",
                                    frames, ps.empty() ? 0.0f : ps.front().uv[0],
                                    ps.empty() ? 0.0f : ps.front().uv[1],
                                    ps.empty() ? 0.0f : ps.front().uv[2],
                                    ps.empty() ? 0.0f : ps.front().uv[3],
                                    ps.empty() ? 0.0f : ps.front().ramp_v));
    }

    // Test 8: on the water, particles snap up to it from below (down to it
    // from above, a SortOrder below -101's); OnlyEmitOnWater emits over water,
    // on it, and nothing over land.
    std::optional<Spot> wet;
    {
        const map::Terrain& terrain = *ctx.sim.terrain();
        const f32 w = terrain.water_elevation();
        for (u32 z = 32; z + 32 < terrain.map_height() && !wet; z += 8)
            for (u32 x = 32; x + 32 < terrain.map_width() && !wet; x += 8) {
                bool deep = terrain.has_water();
                for (int dz = -8; dz <= 8 && deep; dz += 4)
                    for (int dxx = -8; dxx <= 8 && deep; dxx += 4)
                        deep = terrain.get_terrain_height(
                                   static_cast<f32>(static_cast<int>(x) + dxx),
                                   static_cast<f32>(static_cast<int>(z) + dz)) < w - 4.0f;
                if (deep) wet = Spot{static_cast<f32>(x), static_cast<f32>(z)};
            }
    }
    const f32 water = ctx.sim.terrain()->water_elevation();
    if (wet) {
        (void)spawn_unit(ctx, "__osc_pt_w1", "uel0105", "ARMY_1", {wet->x - 4, wet->z}, 0);
        (void)spawn_unit(ctx, "__osc_pt_w2", "uel0105", "ARMY_1", {wet->x + 4, wet->z}, 0);
        warp("__osc_pt_w1", wet->x - 4, wet->z, water - 2.0f);
        warp("__osc_pt_w2", wet->x + 4, wet->z, water + 2.0f);
        const u32 below = make("__osc_pt_snap", "AtEntity", "__osc_pt_w1", bp("snap.bp"));
        const u32 above = make("__osc_pt_snapu", "AtEntity", "__osc_pt_w2", bp("snapunder.bp"));
        const u32 on_water = make("__osc_pt_ow", "AtEntity", "__osc_pt_w2", bp("onwater.bp"));
        const u32 on_land = make("__osc_pt_ol", "AtEntity", "__osc_pt_a", bp("onwater.bp"));
        step();
        const auto first_y = [&](u32 fx) {
            const auto ps = particles_of(r, fx);
            return ps.empty() ? -1e9f : ps.front().center.y;
        };
        const f32 off = renderer::kParticleWaterOffset;
        t.check(near(first_y(below), water + off) && near(first_y(above), water - off) &&
                    near(first_y(on_water), water + off) && particles_of(r, on_land).empty(),
                fmt::format("Test 8: water at {:.2f}: snapped up {:.2f}, down {:.2f}, on it "
                            "{:.2f}; over land {}",
                            water, first_y(below), first_y(above), first_y(on_water),
                            particles_of(r, on_land).size()));
    } else {
        t.check(false, "Test 8: water on the map");
    }

    // Test 9: EmitIfVisible. ARMY_2's F in the fog emits nothing; seen, it
    // catches up at its next look (every fifth tick). L's (LODCutoff 70)
    // emits nothing from 100 away, and catches up from 40.
    {
        look_at(f0.x, f0.z, 60.0f);
        r.set_player_army(0);
        r.set_fog_enabled(true);
        const u32 seen_fx = make("__osc_pt_seen", "AtEntity", "__osc_pt_f", bp("seen.bp"));
        // (And a CreateIfVisible one, unseen when made: never made, nor marked.)
        const u32 unmade_fx = make("__osc_pt_unmade", "AtEntity", "__osc_pt_f", bp("unmade.bp"));
        for (int i = 0; i < 12; ++i) step();
        const size_t fogged = particles_of(r, seen_fx).size();
        r.set_fog_enabled(false);
        size_t caught = 0;
        for (int i = 0; i < 5 && caught == 0; ++i) {
            step();
            caught = particles_of(r, seen_fx).size();
        }
        const Frame seen_frame = drawn(r);
        const sim::EntityRecord* fr = nullptr;
        for (const auto& e : seen.cur().entities)
            if (std::abs(e.position.x - f0.x) < 0.5f && std::abs(e.position.z - f0.z) < 0.5f)
                fr = &e;
        const auto fs = fr ? screen_of(r, fr->position) : std::nullopt;
        const bool unmarked = fs && !quad_at(seen_frame.overlay, (*fs)[0], (*fs)[1], 4.0f, 4.0f) &&
                              particles_of(r, unmade_fx).empty();
        t.check(unmarked, fmt::format("Test 9b: seen at last, the CreateIfVisible emitter made in "
                                      "the fog stays unmade and unmarked: {}",
                                      unmarked));
        look_at(l0.x, l0.z, 100.0f);
        const u32 lod_fx = make("__osc_pt_lod", "AtEntity", "__osc_pt_l", bp("lod.bp"));
        for (int i = 0; i < 3; ++i) step();
        const size_t far = particles_of(r, lod_fx).size();
        look_at(l0.x, l0.z, 40.0f);
        step();
        const size_t near_now = particles_of(r, lod_fx).size();
        t.check(fogged == 0 && caught >= 12 && far == 0 && near_now == 4,
                fmt::format("Test 9: fogged {} (0), seen {} (a catch-up, >= 12); from 100 {} "
                            "(0), from 40 {} (4: 3 caught up)",
                            fogged, caught, far, near_now));
    }

    // Test 10: in the frame, G's additive emitter (a 3-wide quad, its ramp
    // red while young, blue old) adds red then blue; H's MODULATEINVERSE one
    // darkens; over the water a SortOrder −102 one is covered by the water, a
    // SortOrder 0 one isn't.
    {
        look_at(g0.x, (g0.z + h0.z) / 2, 50.0f);
        step();
        const ImageRGBA8 without = shots.grab();
        const u32 add_fx = make("__osc_pt_add", "AtEntity", "__osc_pt_g", bp("add.bp"));
        (void)make("__osc_pt_mod", "AtEntity", "__osc_pt_h", bp("mod.bp"));
        step();
        const ImageRGBA8 young = shots.grab();
        for (int i = 0; i < 24; ++i) step();
        const ImageRGBA8 old = shots.grab();
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
        // Beside the engineer, inside the quad: 2 along the camera's right.
        const auto v = r.camera().view();
        const sim::Vector3 right{v[0], v[4], v[8]};
        const sim::Vector3 gp = pos(g);
        const sim::Vector3 in{gp.x + right.x * 2, gp.y + right.y * 2, gp.z + right.z * 2};
        const sim::Vector3 out{gp.x + right.x * 5, gp.y + right.y * 5, gp.z + right.z * 5};
        const auto added = [&](const ImageRGBA8& img, const sim::Vector3& p) {
            const auto a = px(img, p);
            const auto bb = px(without, p);
            return std::array<f32, 3>{a[0] - bb[0], a[1] - bb[1], a[2] - bb[2]};
        };
        const auto red_young = added(young, in);
        const auto blue_old = added(old, in);
        const auto outside = added(young, out);
        const sim::Vector3 hp{h0.x + right.x * 2, lifted(h0) + right.y * 2, h0.z + right.z * 2};
        const auto dark = px(old, hp);
        const auto light = px(without, hp);
        t.check(red_young[0] > 0.3f && red_young[2] < 0.1f && std::abs(red_young[1]) < 0.05f &&
                    blue_old[2] > 0.3f && std::abs(outside[0]) < 0.05f &&
                    dark[0] + dark[1] + dark[2] < 0.1f && light[0] + light[1] + light[2] > 0.1f &&
                    !particles_of(r, add_fx).empty(),
                fmt::format("Test 10: young added r{:+.2f} b{:+.2f}, old b{:+.2f}; outside "
                            "{:+.2f}; under MODULATEINVERSE {:.2f} (without {:.2f})",
                            red_young[0], red_young[2], blue_old[2], outside[0],
                            dark[0] + dark[1] + dark[2], light[0] + light[1] + light[2]));
        if (wet) {
            (void)spawn_unit(ctx, "__osc_pt_u1", "uel0105", "ARMY_1", {wet->x, wet->z - 6}, 0);
            (void)spawn_unit(ctx, "__osc_pt_u2", "uel0105", "ARMY_1", {wet->x, wet->z + 6}, 0);
            warp("__osc_pt_u1", wet->x, wet->z - 6, water + 1.0f);
            warp("__osc_pt_u2", wet->x, wet->z + 6, water + 1.0f);
            look_at(wet->x, wet->z, 50.0f);
            step();
            const ImageRGBA8 dry = shots.grab();
            (void)make("__osc_pt_under", "AtEntity", "__osc_pt_u1", bp("under.bp"));
            (void)make("__osc_pt_over", "AtEntity", "__osc_pt_u2", bp("over.bp"));
            step();
            const ImageRGBA8 lit = shots.grab();
            const auto red_at = [&](f32 z) {
                const sim::Vector3 p{wet->x + right.x * 2, water + right.y * 2, z + right.z * 2};
                return px(lit, p)[0] - px(dry, p)[0];
            };
            const f32 under = red_at(wet->z - 6);
            const f32 over = red_at(wet->z + 6);
            t.check(under > 0.02f && over > under + 0.1f,
                    fmt::format("Test 10: over the water, SortOrder -102 adds {:+.2f}, 0 {:+.2f}",
                                under, over));
        }
    }

    // Test 11: a drawn emitter needs no overlay dot, from the first frame it
    // is in (an unreadable one keeps its dot); a retail emitter
    // (aeon_build_01) draws.
    {
        look_at((q0.x + a0.x) / 2, q0.z, 60.0f);
        const u32 retail = make("__osc_pt_retail", "AtEntity", "__osc_pt_q",
                                "/effects/emitters/aeon_build_01_emit.bp");
        (void)make("__osc_pt_none", "AtEntity", "__osc_pt_a", bp("no_such.bp"));
        step();
        const Frame first = drawn(r);
        for (int i = 0; i < 2; ++i) step();
        const Frame frame = drawn(r);
        const auto dot_in = [&](const Frame& f, const sim::Vector3& p) {
            const auto s = screen_of(r, p);
            return s && quad_at(f.overlay, (*s)[0], (*s)[1], 4.0f, 4.0f) != nullptr;
        };
        const sim::EntityRecord* qa = nullptr;
        const sim::EntityRecord* aa = nullptr;
        for (const auto& e : seen.cur().entities) {
            if (std::abs(e.position.x - q0.x) < 0.5f && std::abs(e.position.z - q0.z) < 0.5f)
                qa = &e;
            if (std::abs(e.position.x - a0.x) < 0.5f && std::abs(e.position.z - a0.z) < 0.5f)
                aa = &e;
        }
        const bool no_dot = qa && !dot_in(first, qa->position) && !dot_in(frame, qa->position);
        const bool dot = aa && dot_in(frame, aa->position);
        const bool retail_drawn = !particles_of(r, retail).empty();
        t.check(no_dot && dot && retail_drawn,
                fmt::format("Test 11: no dot on a drawn emitter, its first frame on {}, a dot on "
                            "an unreadable one {}; retail's aeon_build_01 draws {}",
                            no_dot, dot, retail_drawn));
    }

    // Test 12: scattered across the ground by up to half their Size (10)
    // either way: 20 in a tick, within 5 of their emitter, at its height,
    // not all in one place.
    {
        const u32 scatter_fx = make("__osc_pt_scatter", "AtEntity", "__osc_pt_q", bp("scatter.bp"));
        const sim::Vector3 at = [&] {
            for (const auto& e : seen.cur().entities)
                if (std::abs(e.position.x - q0.x) < 0.5f && std::abs(e.position.z - q0.z) < 0.5f)
                    return e.position;
            return sim::Vector3{};
        }();
        step();
        const auto ps = particles_of(r, scatter_fx);
        f32 widest = 0;
        bool level = true;
        for (const Particle& q : ps) {
            widest = std::max(widest, std::hypot(q.center.x - at.x, q.center.z - at.z));
            level = level && near(q.center.y, at.y, 1e-2f);
        }
        t.check(ps.size() == 20 && widest <= 5.0f + 1e-3f && widest > 1.0f && level,
                fmt::format("Test 12: {} scattered (20), the widest {:.2f} from it (1-5), level {}",
                            ps.size(), widest, level));
    }

    // Test 13: three ticks run before the next frame (a slow one): the
    // emitter emits all three, aged 1-3, its clock run on with them.
    {
        const u32 gap_fx = make("__osc_pt_gap", "AtEntity", "__osc_pt_q", bp("still.bp"));
        step();
        for (int i = 0; i < 3; ++i) ctx.sim.tick();
        shots.recapture();
        shots.redraw();
        seen.capture(ctx.sim);
        const auto ps = particles_of(r, gap_fx);
        const bool all = ps.size() == 4 && near(ps.front().age, 1.0f) && near(ps[1].age, 2.0f) &&
                         near(ps[2].age, 3.0f) && near(ps.back().age, 4.0f);
        std::string clock = "none";
        for (const auto& em : r.particle_system().emitters())
            if (em.effect_id == gap_fx) clock = fmt::format("{:.0f}", em.clock);
        t.check(
            all && clock == "4",
            fmt::format("Test 13: after a 3-tick frame, {} particles (4, aged 1-4), clock {} (4)",
                        ps.size(), clock));
    }

    // Test 14: graphics_Fidelity. At low, an emitter whose blueprint says
    // LowFidelity = false is never made (Moho destroys it as it makes it),
    // and stays unmade once the fidelity is raised; one made at high draws.
    {
        r.video_options().graphics_fidelity = 0;
        const u32 low_fx = make("__osc_pt_low", "AtEntity", "__osc_pt_q", bp("lowoff.bp"));
        const u32 every_fx = make("__osc_pt_every", "AtEntity", "__osc_pt_q", bp("still.bp"));
        for (int i = 0; i < 2; ++i) step();
        const bool left_out = particles_of(r, low_fx).empty() &&
                              r.particle_system().unmade(low_fx) &&
                              !particles_of(r, every_fx).empty();
        r.video_options().graphics_fidelity = 2;
        const u32 high_fx = make("__osc_pt_high", "AtEntity", "__osc_pt_q", bp("lowoff.bp"));
        for (int i = 0; i < 2; ++i) step();
        const bool stays = particles_of(r, low_fx).empty() && !particles_of(r, high_fx).empty();
        t.check(left_out && stays,
                fmt::format("Test 14: at low fidelity a LowFidelity = false emitter is never "
                            "made {}, nor once raised {}; made at high, it draws",
                            left_out, stays));
    }

    // Test 15: a light (CreateLightParticle; particle.fx's TLight) is one
    // flat glow, 2 * its size across, where its unit was made: drawn
    // through the ground its unit is sunk 2 under, gone after its lifetime.
    {
        (void)r.texture_cache().get_blocking("/textures/particles/glow_03.dds");
        (void)r.texture_cache().get_blocking("/textures/particles/ramp_flare_02.dds");
        look_at(l0.x, l0.z, 50.0f);
        step();
        const ImageRGBA8 without = shots.grab();
        const f32 sunk = height(l0.x, l0.z) - 2.0f;
        warp("__osc_pt_l", l0.x, l0.z, sunk);
        run_lua(ctx, "CreateLightParticle(__osc_pt_l, -1, 1, 4, 5, 'glow_03', 'ramp_flare_02')\n");
        step();
        const ImageRGBA8 lit = shots.grab();
        u32 light_fx = 0;
        for (const sim::EffectRecord& fx : seen.cur().effects) {
            if (fx.type == sim::EffectType::LIGHT_PARTICLE && fx.entity_id == l) {
                light_fx = fx.id;
            }
        }
        const auto ps = particles_of(r, light_fx);
        const sim::Vector3 at{l0.x, sunk, l0.z};
        const bool flat = ps.size() == 1 && near3(ps.front().center, at, 1e-2f) &&
                          near3(ps.front().axis_x, {4, 0, 0}) &&
                          near3(ps.front().axis_y, {0, 0, 4});
        const auto brightness = [&](const ImageRGBA8& img) {
            const auto sp = screen_of(r, at);
            if (!sp || img.width == 0) {
                return 0.0f;
            }
            const u32 x = static_cast<u32>(std::clamp((*sp)[0], 0.0f, img.width - 1.0f));
            const u32 y = static_cast<u32>(std::clamp((*sp)[1], 0.0f, img.height - 1.0f));
            const u8* q = &img.pixels[(static_cast<size_t>(y) * img.width + x) * 4];
            return (q[0] + q[1] + q[2]) / 255.0f;
        };
        const f32 added = brightness(lit) - brightness(without);
        for (int i = 0; i < 6; ++i) {
            step();
        }
        const ImageRGBA8 after = shots.grab();
        const f32 left = brightness(after) - brightness(without);
        const bool gone = particles_of(r, light_fx).empty();
        t.check(flat && added > 0.1f && gone && std::abs(left) < 0.05f,
                fmt::format("Test 15: a light flat where made {} ({} particles), through the "
                            "ground adds {:+.2f}; after its lifetime gone {}, {:+.2f}",
                            flat, ps.size(), added, gone, left));
    }

    spdlog::info("Particle test: {}/{} passed", t.pass, t.pass + t.fail);
}

} // namespace osc::test
