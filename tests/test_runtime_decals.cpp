#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "renderer/runtime_decals.hpp"
#include "sim/decal.hpp"
#include "sim/world_snapshot.hpp"

#include <cmath>
#include <memory>
#include <string>

using namespace osc;
using Catch::Approx;
using renderer::RuntimeDecals;

namespace {

/// A world whose snapshot the tests edit between ticks.
struct World {
    sim::WorldSnapshot snap;
    RuntimeDecals decals;

    sim::EffectRecord& add(u32 id, bool splat, u32 seen_by, u32 remove_tick = 0,
                           const std::string& type = "Albedo") {
        auto spec = std::make_shared<sim::DecalSpec>();
        spec->splat = splat;
        spec->type = splat ? "" : type;
        spec->texture1 = "scorch_001";
        spec->texture2 = splat ? "" : "/textures/spec.dds";
        spec->position = {10, 0, 10};
        spec->size_x = 3;
        spec->size_z = 4;
        spec->remove_tick = remove_tick;
        sim::EffectRecord& r = snap.effects.emplace_back();
        r.id = id;
        r.type = splat ? sim::EffectType::SPLAT : sim::EffectType::DECAL;
        r.decal = std::move(spec);
        r.seen_by = seen_by;
        return r;
    }
    void remove(u32 id) {
        std::erase_if(snap.effects, [id](const sim::EffectRecord& r) { return r.id == id; });
    }
    void tick(u32 t, i32 focus = 0) {
        snap.tick = t;
        decals.update(snap, focus);
    }
};

} // namespace

TEST_CASE("Runtime decal textures resolve as Moho resolves them", "[runtime_decals]") {
    CHECK(renderer::resolve_decal_texture("scorch_001", false) ==
          "/env/common/decals/scorch_001.dds");
    CHECK(renderer::resolve_decal_texture("scorch_001", true) ==
          "/env/common/splats/scorch_001.dds");
    CHECK(renderer::resolve_decal_texture("/mods/x/a.dds", true) == "/mods/x/a.dds");
    CHECK(renderer::resolve_decal_texture("\\env\\a.dds", false) == "\\env\\a.dds");
    CHECK(renderer::resolve_decal_texture("", false).empty());
}

TEST_CASE("Runtime decal types by name", "[runtime_decals]") {
    CHECK(renderer::decal_type_named("Albedo") == map::DecalType::Albedo);
    CHECK(renderer::decal_type_named("Alpha Normals") == map::DecalType::AlphaNormals);
    CHECK(renderer::decal_type_named("Glow Mask") == map::DecalType::GlowMask);
    CHECK(renderer::decal_type_named("AlbedoXP") == map::DecalType::AlbedoXP);
    CHECK_FALSE(renderer::decal_type_named("Undefined"));
    CHECK_FALSE(renderer::decal_type_named("albedo"));
}

TEST_CASE("The focus army's decals are added, an observer's all", "[runtime_decals]") {
    World w;
    w.add(1, false, 0b01);
    w.add(2, true, 0b10);
    w.add(3, false, 0b10, 0, "Nonsense"); // dropped
    w.tick(5, 0);
    REQUIRE(w.decals.decals().size() == 1);
    CHECK(w.decals.decals()[0].id == 1);
    CHECK(w.decals.splats().empty());

    World o;
    o.add(1, false, 0b01);
    o.add(2, true, 0b10);
    o.add(3, false, 0b10, 0, "Nonsense");
    o.tick(5, -1);
    CHECK(o.decals.decals().size() == 1);
    REQUIRE(o.decals.splats().size() == 1);
    const RuntimeDecals::Decal& s = o.decals.splats()[0];
    CHECK(s.info.type == map::DecalType::Albedo);
    CHECK(s.info.texture_path == "/env/common/splats/scorch_001.dds");
    CHECK(s.info.texture2_path.empty());
    const RuntimeDecals::Decal& d = o.decals.decals()[0];
    CHECK(d.info.texture_path == "/env/common/decals/scorch_001.dds");
    CHECK(d.info.texture2_path == "/textures/spec.dds");
    CHECK(d.alpha == 1.0f);
}

TEST_CASE("A runtime decal's cutoff: its own, or its diagonal", "[runtime_decals]") {
    World w;
    w.add(1, false, 1).decal = [] {
        auto spec = std::make_shared<sim::DecalSpec>();
        spec->type = "Albedo";
        spec->size_x = 3;
        spec->size_z = 4;
        spec->lod = 150;
        return spec;
    }();
    w.add(2, false, 1);
    w.tick(1);
    REQUIRE(w.decals.decals().size() == 2);
    CHECK(w.decals.decals()[0].info.cut_off_lod == 150.0f);
    CHECK(w.decals.decals()[1].info.cut_off_lod == Approx(5.0f));
    CHECK(w.decals.decals()[1].info.near_cut_off_lod == 0.0f);
}

TEST_CASE("A removed decal fades 0.2 a beat, a splat 0.03", "[runtime_decals]") {
    World w;
    w.add(1, false, 1);
    w.add(2, true, 1);
    w.tick(10);
    const u32 gen = w.decals.generation();
    w.remove(1);
    w.remove(2);
    w.tick(11);
    REQUIRE(w.decals.decals().size() == 1);
    CHECK(w.decals.decals()[0].alpha == Approx(0.8f));
    CHECK(w.decals.splats()[0].alpha == Approx(0.97f));
    w.tick(12);
    CHECK(w.decals.decals()[0].alpha == Approx(0.6f));
    // float32 as Moho's: five steps leave a crumb, the sixth clears it.
    w.tick(15);
    REQUIRE(w.decals.decals().size() == 1);
    CHECK(w.decals.decals()[0].alpha < 1e-6f);
    CHECK(w.decals.generation() == gen);
    w.tick(16);
    CHECK(w.decals.decals().empty());
    CHECK(w.decals.generation() != gen);
    CHECK(w.decals.splats()[0].alpha == Approx(1.0f - 6 * 0.03f));
    // 33 beats leave a hundredth (one catch-up over the ticks since); the
    // next clears it.
    w.tick(43);
    REQUIRE(w.decals.splats().size() == 1);
    CHECK(w.decals.splats()[0].alpha < 0.03f);
    w.tick(46);
    CHECK(w.decals.splats().empty());
}

TEST_CASE("A decal fades after its removal tick without being removed", "[runtime_decals]") {
    World w;
    w.add(1, false, 1, 20);
    w.tick(18);
    w.tick(20);
    CHECK(w.decals.decals()[0].alpha == 1.0f);
    w.tick(21);
    CHECK(w.decals.decals()[0].alpha == Approx(0.8f));
}

TEST_CASE("A decal the focus stops seeing is removed; a new game clears", "[runtime_decals]") {
    World w;
    w.add(1, false, 0b11);
    w.tick(3, 0);
    w.tick(4, 1); // the same decal, seen by the new focus
    CHECK(w.decals.decals()[0].alpha == 1.0f);
    w.snap.effects[0].seen_by = 0b01;
    w.tick(5, 1);
    CHECK(w.decals.decals()[0].alpha == Approx(0.8f));
    CHECK_FALSE(w.decals.decals()[0].live);
    w.tick(2, 1); // an earlier tick: a new game
    CHECK(w.decals.decals().empty());
}
