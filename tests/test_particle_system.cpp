// FA's particles (M214c), as Moho's CEfxEmitter emits them and particle.fx's
// WorldVS moves them: curves read in ticks (SEfxCurve::GetValue), at the
// emitter's clock modulo its Repeattime; the emit rate's fraction carried
// from tick to tick; each particle moving by itself from its spawn state.

#include <catch2/catch_test_macros.hpp>

#include "lua/lua_state.hpp"
#include "renderer/beam_blueprint.hpp"
#include "renderer/camera.hpp"
#include "renderer/effect_blueprint_file.hpp"
#include "renderer/emitter_blueprint.hpp"
#include "renderer/particle_system.hpp"
#include "renderer/recon_view.hpp"
#include "renderer/trail_blueprint.hpp"
#include "sim/emitter_params.hpp"
#include "sim/ieffect.hpp"
#include "sim/world_snapshot.hpp"
#include "vfs/directory_mount.hpp"
#include "vfs/virtual_file_system.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

/// A curve of constant keys (no spread).
osc::renderer::EmitterCurve keys(std::vector<osc::renderer::CurveKey> k) {
    return osc::renderer::EmitterCurve{std::move(k)};
}

} // namespace

TEST_CASE("Emitter curves read as Moho's SEfxCurve::GetValue", "[renderer][emitter]") {
    const auto half = [] { return 0.5f; }; // no spread's worth of randomness
    const auto c = keys({{2, 10, 0}, {6, 30, 0}});
    CHECK(c.value(0, half) == 10.0f); // before the first key: that key
    CHECK(c.value(2, half) == 10.0f); // at a key: interpolated from it
    CHECK(c.value(4, half) == 20.0f); // between two: interpolated
    CHECK(c.value(9, half) == 30.0f); // past the last: that key
    CHECK(keys({}).value(3, half) == 0.0f);
    CHECK(c.value(std::nanf(""), half) == 10.0f); // Repeattime 0's NaN: the first key

    // Its spread z is the whole width of the random range about y.
    const auto spread = keys({{0, 5, 4}});
    CHECK(spread.value(0, [] { return 0.0f; }) == 3.0f);
    CHECK(spread.value(0, [] { return 1.0f; }) == 7.0f);
    CHECK(spread.peak() == 7.0f); // y + z/2, the longest life it gives
}

TEST_CASE("Emitters emit their rate's whole part each tick, curves read at their clock",
          "[renderer][emitter]") {
    const fs::path root = fs::temp_directory_path() / "osc_particle_system_test";
    fs::remove_all(root);
    const fs::path dir = root / "effects" / "Emitters";
    fs::create_directories(dir);
    // 2.5 a tick; and one whose rate is 1 for phases 0-2 and 5 at phase 3,
    // over a Repeattime of 4. Both emit straight along their frame's +X at 2
    // a tick, falling 1 a tick², living 50 ticks, emitting at the tick's
    // start (no interpolation), seen or not.
    const std::string common = "    Lifetime = -1,\n    InterpolateEmission = false,\n"
                               "    EmitIfVisible = false,\n"
                               "    SnapToWaterline = false,\n"
                               "    LifetimeCurve = { Keys = { { x = 0, y = 50, z = 0 } } },\n"
                               "    XDirectionCurve = { Keys = { { x = 0, y = 1, z = 0 } } },\n"
                               "    VelocityCurve = { Keys = { { x = 0, y = 2, z = 0 } } },\n"
                               "    YAccelCurve = { Keys = { { x = 0, y = -1, z = 0 } } },\n";
    std::ofstream(dir / "steady_emit.bp")
        << "EmitterBlueprint {\n"
        << common << "    Repeattime = 10,\n"
        << "    EmitRateCurve = { Keys = { { x = 0, y = 2.5, z = 0 } } },\n}\n";
    std::ofstream(dir / "pulse_emit.bp")
        << "EmitterBlueprint {\n"
        << common << "    Repeattime = 4,\n"
        << "    EmitRateCurve = { Keys = { { x = 0, y = 1, z = 0 }, { x = 2, y = 1, z = 0 }, "
           "{ x = 2.001, y = 5, z = 0 } } },\n}\n";

    osc::vfs::VirtualFileSystem vfs;
    vfs.mount("/", std::make_unique<osc::vfs::DirectoryMount>(root));
    osc::lua::LuaState lua;
    osc::renderer::EmitterBlueprintCache cache;
    cache.set_vfs(&vfs);
    osc::renderer::Camera camera;
    osc::renderer::ParticleSystem ps;

    const auto emitter = [](osc::u32 id, const char* name, osc::sim::Quaternion turn) {
        osc::sim::EffectRecord fx;
        fx.id = id;
        fx.type = osc::sim::EffectType::EMITTER_AT_ENTITY;
        fx.blueprint_path = std::string("/effects/emitters/") + name + "_emit.bp";
        fx.framed = true;
        fx.frame_position = {10, 20, 30};
        fx.frame_rotation = turn;
        return fx;
    };
    // The pulse's frame turned a quarter about Y: its +X is the world's -Z.
    const osc::sim::Quaternion quarter{0, std::sqrt(0.5f), 0, std::sqrt(0.5f)};
    std::vector<osc::sim::WorldSnapshot> ticks(9);
    for (osc::u32 t = 0; t < ticks.size(); ++t) {
        ticks[t].tick = t + 1;
        ticks[t].effects = {emitter(1, "steady", {}), emitter(2, "pulse", quarter)};
    }
    const auto count = [&](osc::u32 id) {
        size_t n = 0;
        for (const auto& d : ps.drawn()) n += d.effect_id == id ? 1 : 0;
        return n;
    };
    std::vector<size_t> steady;
    std::vector<size_t> pulse;
    for (size_t t = 0; t < ticks.size(); ++t) {
        const osc::sim::WorldSnapshot& prev = ticks[t == 0 ? 0 : t - 1];
        ps.update(osc::sim::FrameView(&prev, &ticks[t], 1.0f), camera, nullptr, cache, lua.raw(),
                  nullptr);
        steady.push_back(count(1));
        pulse.push_back(count(2));
    }
    // 2.5 a tick: 2, 3, 2, 3 ... (the half carried).
    CHECK(steady == std::vector<size_t>{2, 5, 7, 10, 12, 15, 17, 20, 22});
    // 1, 1, 1, 5 over each Repeattime of 4.
    CHECK(pulse == std::vector<size_t>{1, 2, 3, 8, 9, 10, 11, 16, 17});

    // Each moves by itself: born at its tick, drawn a tick on (the frame's
    // interpolant 1) and each tick after, P + V·t + ½A·t².
    bool steady_moves = false;
    bool pulse_turned = false;
    for (const auto& d : ps.drawn()) {
        const float t = d.age;
        if (d.effect_id == 1 && std::abs(t - 3.0f) < 1e-4f)
            steady_moves = std::abs(d.center.x - (10 + 2 * t)) < 1e-3f &&
                           std::abs(d.center.y - (20 - 0.5f * t * t)) < 1e-3f &&
                           std::abs(d.center.z - 30) < 1e-3f;
        if (d.effect_id == 2 && std::abs(t - 3.0f) < 1e-4f)
            pulse_turned = std::abs(d.center.x - 10) < 1e-3f &&
                           std::abs(d.center.z - (30 - 2 * t)) < 1e-3f; // LocalVelocity
    }
    CHECK(steady_moves);
    CHECK(pulse_turned);
    fs::remove_all(root);
}

TEST_CASE("A script's emitter params and curves reach its emitter", "[renderer][emitter]") {
    // SetEmitterParam's REPEATTIME and TICKCOUNT, SetEmitterCurveParam and
    // ResizeEmitterCurve (M214d), as the effect's record carries them.
    const fs::path root = fs::temp_directory_path() / "osc_particle_overrides_test";
    fs::remove_all(root);
    const fs::path dir = root / "effects" / "Emitters";
    fs::create_directories(dir);
    // 1 a tick for phases 0-2 and 5 at phase 3, over a Repeattime of 4.
    std::ofstream(dir / "pulse_emit.bp")
        << "EmitterBlueprint {\n    Lifetime = -1,\n    InterpolateEmission = false,\n"
           "    EmitIfVisible = false,\n    SnapToWaterline = false,\n    Repeattime = 4,\n"
           "    LifetimeCurve = { Keys = { { x = 0, y = 50, z = 0 } } },\n"
           "    EmitRateCurve = { XRange = 4, Keys = { { x = 0, y = 1, z = 0 }, "
           "{ x = 2, y = 1, z = 0 }, { x = 2.001, y = 5, z = 0 } } },\n}\n";
    osc::vfs::VirtualFileSystem vfs;
    vfs.mount("/", std::make_unique<osc::vfs::DirectoryMount>(root));
    osc::lua::LuaState lua;
    osc::renderer::EmitterBlueprintCache cache;
    cache.set_vfs(&vfs);
    osc::renderer::Camera camera;
    osc::renderer::ParticleSystem ps;

    using Op = osc::sim::IEffect::EmitterCurveOp;
    const auto pulse = [](osc::u32 id) {
        osc::sim::EffectRecord fx;
        fx.id = id;
        fx.blueprint_path = "/effects/emitters/pulse_emit.bp";
        fx.framed = true;
        return fx;
    };
    const auto param = [](osc::sim::EffectRecord& fx, osc::u8 p, float v, osc::u32 serial) {
        fx.emitter_params_set |= 1u << p;
        fx.emitter_params[p] = v;
        fx.overrides_serial = serial;
    };
    std::vector<osc::sim::WorldSnapshot> ticks(8);
    for (osc::u32 t = 0; t < ticks.size(); ++t) {
        ticks[t].tick = t + 1;
        // 3: REPEATTIME 2 from its fourth tick on.
        auto repeat = pulse(3);
        if (t >= 3) param(repeat, osc::sim::kParamRepeatTime, 2, 1);
        // 4: its emit rate made one key, 3 give or take nothing.
        auto rate = pulse(4);
        rate.curve_ops = {Op{3, false, 3, 0}}; // EMITRATE_CURVE
        rate.overrides_serial = 1;
        // 5: its clock set to 3.
        auto clock = pulse(5);
        param(clock, osc::sim::kParamTickCount, 3, 1);
        // 6: its emit rate stretched from 4 ticks to 8 over a Repeattime of 8.
        auto stretched = pulse(6);
        param(stretched, osc::sim::kParamRepeatTime, 8, 1);
        stretched.curve_ops = {Op{3, true, 8, 0}};
        ticks[t].effects = {repeat, rate, clock, stretched};
    }
    const auto count = [&](osc::u32 id) {
        size_t n = 0;
        for (const auto& d : ps.drawn()) n += d.effect_id == id ? 1 : 0;
        return n;
    };
    std::vector<size_t> repeat, rate, clock, stretched;
    for (size_t t = 0; t < ticks.size(); ++t) {
        const osc::sim::WorldSnapshot& prev = ticks[t == 0 ? 0 : t - 1];
        ps.update(osc::sim::FrameView(&prev, &ticks[t], 1.0f), camera, nullptr, cache, lua.raw(),
                  nullptr);
        repeat.push_back(count(3));
        rate.push_back(count(4));
        clock.push_back(count(5));
        stretched.push_back(count(6));
    }
    // Phases 0, 1, 2, then clock 3 over 2: 1, 0, 1 ... -- never the 5.
    CHECK(repeat == std::vector<size_t>{1, 2, 3, 4, 5, 6, 7, 8});
    CHECK(rate == std::vector<size_t>{3, 6, 9, 12, 15, 18, 21, 24});
    // Phases 3, 0, 1, 2, 3, 0 ...: 5, 1, 1, 1, 5, ...
    CHECK(clock == std::vector<size_t>{5, 6, 7, 8, 13, 14, 15, 16});
    // Keys at 0, 4, 4.002 over 8: phases 0-4 give 1, 5-7 give 5.
    CHECK(stretched == std::vector<size_t>{1, 2, 3, 4, 5, 10, 15, 20});
    fs::remove_all(root);
}

TEST_CASE("Effects are made at the fidelities their blueprints allow, as Moho's are",
          "[renderer][emitter]") {
    // REffectBlueprint's LowFidelity, MedFidelity and HighFidelity (true
    // unless false); CEffectManagerImpl destroys, as it makes it, an effect
    // its blueprint leaves out at graphics_Fidelity.
    using osc::renderer::fidelity_allows;
    CHECK(fidelity_allows(0b111, 0));
    CHECK_FALSE(fidelity_allows(0b110, 0)); // LowFidelity = false
    CHECK(fidelity_allows(0b110, 1));
    CHECK_FALSE(fidelity_allows(0b101, 1)); // MedFidelity = false
    CHECK(fidelity_allows(0b101, 2));
    CHECK_FALSE(fidelity_allows(0b111, 3)); // no such fidelity: 1 << 3 is in no mask
    CHECK_FALSE(fidelity_allows(0b111, -1));

    const fs::path root = fs::temp_directory_path() / "osc_effect_fidelity_test";
    fs::remove_all(root);
    const fs::path emitters = root / "effects" / "Emitters";
    fs::create_directories(emitters);
    const std::string common = "    Lifetime = -1, InterpolateEmission = false,\n"
                               "    EmitIfVisible = false, SnapToWaterline = false,\n"
                               "    LifetimeCurve = { Keys = { { x = 0, y = 50, z = 0 } } },\n"
                               "    EmitRateCurve = { Keys = { { x = 0, y = 1, z = 0 } } },\n";
    std::ofstream(emitters / "every_emit.bp") << "EmitterBlueprint {\n" << common << "}\n";
    std::ofstream(emitters / "lowoff_emit.bp") << "EmitterBlueprint {\n"
                                               << common << "    LowFidelity = false,\n}\n";
    std::ofstream(emitters / "medoff_emit.bp")
        << "EmitterBlueprint {\n"
        << common << "    LowFidelity = true, MedFidelity = false, HighFidelity = true,\n}\n";
    std::ofstream(emitters / "lowbeam.bp")
        << "BeamBlueprint {\n    TextureName = '/t.dds', LowFidelity = false,\n}\n";
    std::ofstream(emitters / "hightrail.bp")
        << "TrailEmitterBlueprint {\n    TrailLength = 5, HighFidelity = false,\n}\n";

    osc::vfs::VirtualFileSystem vfs;
    vfs.mount("/", std::make_unique<osc::vfs::DirectoryMount>(root));
    osc::lua::LuaState lua;
    osc::renderer::EmitterBlueprintCache cache;
    cache.set_vfs(&vfs);
    osc::renderer::BeamBlueprintCache beams;
    beams.set_vfs(&vfs);
    osc::renderer::TrailBlueprintCache trails;
    trails.set_vfs(&vfs);

    // Read from each kind of blueprint.
    const auto* every = cache.get("/effects/emitters/every_emit.bp", lua.raw());
    const auto* lowoff = cache.get("/effects/emitters/lowoff_emit.bp", lua.raw());
    const auto* medoff = cache.get("/effects/emitters/medoff_emit.bp", lua.raw());
    const auto* beam = beams.get("/effects/emitters/lowbeam.bp", lua.raw());
    const auto* trail = trails.get("/effects/emitters/hightrail.bp", lua.raw());
    REQUIRE(every);
    REQUIRE(lowoff);
    REQUIRE(medoff);
    REQUIRE(beam);
    REQUIRE(trail);
    CHECK(every->fidelity == 0b111);
    CHECK(lowoff->fidelity == 0b110);
    CHECK(medoff->fidelity == 0b101);
    CHECK(beam->fidelity == 0b110);
    CHECK(trail->fidelity == 0b011);

    // At low fidelity the particles never make the one that leaves it out,
    // not even once the fidelity is raised (Moho destroyed it).
    osc::renderer::Camera camera;
    osc::renderer::ParticleSystem ps;
    ps.set_fidelity(0);
    const auto emitter = [](osc::u32 id, const char* name) {
        osc::sim::EffectRecord fx;
        fx.id = id;
        fx.type = osc::sim::EffectType::EMITTER_AT_ENTITY;
        fx.blueprint_path = std::string("/effects/emitters/") + name + "_emit.bp";
        fx.framed = true;
        return fx;
    };
    std::vector<osc::sim::WorldSnapshot> ticks(4);
    for (osc::u32 t = 0; t < ticks.size(); ++t) {
        ticks[t].tick = t + 1;
        ticks[t].effects = {emitter(1, "every"), emitter(2, "lowoff"), emitter(3, "medoff")};
        if (t >= 2) ticks[t].effects.push_back(emitter(4, "lowoff")); // made at high
    }
    const auto count = [&](osc::u32 id) {
        size_t n = 0;
        for (const auto& d : ps.drawn()) n += d.effect_id == id ? 1 : 0;
        return n;
    };
    for (size_t t = 0; t < ticks.size(); ++t) {
        if (t == 2) ps.set_fidelity(2);
        const osc::sim::WorldSnapshot& prev = ticks[t == 0 ? 0 : t - 1];
        ps.update(osc::sim::FrameView(&prev, &ticks[t], 1.0f), camera, nullptr, cache, lua.raw(),
                  nullptr);
    }
    CHECK(count(1) == 4);
    CHECK(ps.unmade(2));
    CHECK(count(2) == 0);
    CHECK(count(3) == 4); // medium alone is left out
    CHECK_FALSE(ps.unmade(4));
    CHECK(count(4) == 2);
    fs::remove_all(root);
}

namespace {

osc::sim::EffectRecord light(osc::u32 id, osc::sim::EffectType type, osc::sim::Vector3 at) {
    osc::sim::EffectRecord fx;
    fx.id = id;
    fx.type = type;
    fx.framed = true;
    fx.frame_position = at;
    fx.light_size = 5;
    fx.light_lifetime = 4;
    fx.glow_texture = "/textures/particles/glow_02.dds";
    fx.ramp_texture = "/textures/particles/ramp_blue_22.dds";
    return fx;
}

} // namespace

TEST_CASE("A light is one flat TLight particle where it was made and ramped over its lifetime",
          "[renderer][emitter]") {
    osc::lua::LuaState lua;
    osc::renderer::EmitterBlueprintCache cache;
    osc::renderer::Camera camera;
    osc::renderer::ParticleSystem ps;

    std::vector<osc::sim::WorldSnapshot> ticks(5);
    for (osc::u32 t = 0; t < ticks.size(); ++t) {
        ticks[t].tick = t + 1;
        ticks[t].effects = {light(7, osc::sim::EffectType::LIGHT_PARTICLE,
                                  {10.0f + static_cast<osc::f32>(t), 20, 30})};
    }
    const auto frame = [&](size_t t) {
        const osc::sim::WorldSnapshot& prev = ticks[t == 0 ? 0 : t - 1];
        ps.update(osc::sim::FrameView(&prev, &ticks[t], 0.5f), camera, nullptr, cache, lua.raw(),
                  nullptr);
    };

    frame(0);
    REQUIRE(ps.drawn().size() == 1);
    CHECK(ps.draws_effect(7));
    const auto& d = ps.drawn().front();
    CHECK(d.effect_id == 7);
    CHECK(d.center.x == 10.0f);
    CHECK(d.center.y == 20.0f);
    CHECK(d.center.z == 30.0f);
    CHECK(d.axis_x.x == 5.0f);
    CHECK(d.axis_x.y == 0.0f);
    CHECK(d.axis_x.z == 0.0f);
    CHECK(d.axis_y.x == 0.0f);
    CHECK(d.axis_y.y == 0.0f);
    CHECK(d.axis_y.z == 5.0f);
    CHECK(d.lifetime == 4.0f);
    CHECK(d.blendmode == osc::renderer::kBlendAdd);
    CHECK(ps.instances().front().ramp[0] == 0.125f);
    REQUIRE(ps.groups().size() == 1);
    CHECK(ps.groups().front().light);
    CHECK(ps.groups().front().texture == "/textures/particles/glow_02.dds");
    CHECK(ps.groups().front().ramp == "/textures/particles/ramp_blue_22.dds");

    frame(1);
    frame(2);
    frame(3);
    REQUIRE(ps.drawn().size() == 1);
    CHECK(ps.drawn().front().center.x == 10.0f);
    CHECK(ps.instances().front().ramp[0] == 0.875f);

    frame(4);
    CHECK(ps.drawn().empty());
    CHECK(ps.draws_effect(7));
}

TEST_CASE("A LightParticleIntel is made only where the player's army sees it",
          "[renderer][emitter]") {
    osc::lua::LuaState lua;
    osc::renderer::EmitterBlueprintCache cache;
    osc::renderer::Camera camera;
    osc::renderer::ParticleSystem ps;

    osc::sim::WorldSnapshot snap;
    snap.tick = 1;
    snap.sight.army = 0;
    snap.sight.vision_cell = 10;
    snap.sight.vision_width = snap.sight.vision_height = 10;
    snap.sight.vision.assign(100, 0);
    snap.sight.vision[1 * 10 + 1] = 1;
    const osc::sim::FrameView view(&snap, &snap, 0.5f);
    osc::renderer::ReconView recon;
    recon.set_focus_army(0);
    recon.update(view);
    ps.set_recon(&recon);

    using osc::sim::EffectType;
    snap.effects = {light(1, EffectType::LIGHT_PARTICLE_INTEL, {15, 0, 15}),
                    light(2, EffectType::LIGHT_PARTICLE_INTEL, {55, 0, 55}),
                    light(3, EffectType::LIGHT_PARTICLE, {55, 0, 55})};
    ps.update(view, camera, nullptr, cache, lua.raw(), nullptr);
    CHECK(ps.draws_effect(1));
    CHECK(ps.unmade(2));
    CHECK_FALSE(ps.draws_effect(2));
    CHECK(ps.draws_effect(3));
    CHECK(ps.drawn().size() == 2);
}
