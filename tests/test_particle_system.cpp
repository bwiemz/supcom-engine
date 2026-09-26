// The renderer runs an emitter as long as Moho does: its Lifetime in ticks,
// rounded up, however long its Repeattime; a negative Lifetime emits on.

#include <catch2/catch_test_macros.hpp>

#include "lua/lua_state.hpp"
#include "renderer/emitter_blueprint.hpp"
#include "renderer/particle_system.hpp"
#include "sim/world_snapshot.hpp"
#include "vfs/directory_mount.hpp"
#include "vfs/virtual_file_system.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

namespace {

namespace fs = std::filesystem;

// An emitter that emits 10 particles a tick, each living 50 ticks.
void write_emitter(const fs::path& dir, const std::string& name, const std::string& timing) {
    std::ofstream(dir / (name + "_emit.bp"))
        << "EmitterBlueprint {\n"
        << timing
        << "    EmitRateCurve = { XRange = 1, Keys = { { x = 0, y = 10, z = 0 } } },\n"
           "    LifetimeCurve = { XRange = 1, Keys = { { x = 0, y = 50, z = 0 } } },\n"
           "}\n";
}

} // namespace

TEST_CASE("Emitters emit for their Lifetime in whole ticks, not their Repeattime",
          "[renderer][emitter]") {
    const fs::path root = fs::temp_directory_path() / "osc_particle_system_test";
    fs::remove_all(root);
    const fs::path dir = root / "effects" / "Emitters";
    fs::create_directories(dir);
    write_emitter(dir, "flash", "    Lifetime = 0.1,\n    Repeattime = 0.1,\n");
    write_emitter(dir, "plume", "    Lifetime = 2,\n    Repeattime = 6,\n");
    write_emitter(dir, "smoke", "    Lifetime = -1,\n    Repeattime = 50,\n");
    write_emitter(dir, "none", "    Lifetime = 0,\n");

    osc::vfs::VirtualFileSystem vfs;
    vfs.mount("/", std::make_unique<osc::vfs::DirectoryMount>(root));
    osc::lua::LuaState lua;
    osc::renderer::EmitterBlueprintCache cache;
    cache.set_vfs(&vfs);

    osc::sim::WorldSnapshot snap;
    const std::array<const char*, 4> names = {"flash", "plume", "smoke", "none"};
    for (size_t i = 0; i < names.size(); ++i) {
        osc::sim::EffectRecord fx;
        fx.id = static_cast<osc::u32>(i + 1);
        fx.blueprint_path = std::string("/effects/emitters/") + names[i] + "_emit.bp";
        snap.effects.push_back(fx);
    }
    osc::renderer::ParticleSystem ps;
    ps.sync_effects(osc::sim::FrameView(&snap, &snap, 1.0f), cache, lua.raw());
    REQUIRE(ps.emitter_count() == 4);

    const auto find = [&](osc::u32 id) -> const osc::renderer::EmitterState* {
        for (const auto& es : ps.emitters())
            if (es.effect_id == id) return &es;
        return nullptr; // done, and its particles gone
    };
    const auto active = [&](osc::u32 id) {
        const auto* es = find(id);
        return es && es->active;
    };
    const auto particles = [&](osc::u32 id) {
        const auto* es = find(id);
        return es ? es->particles.size() : 0;
    };

    ps.update(0.05f); // half a tick
    CHECK(active(1));
    CHECK(particles(1) > 0); // a muzzle flash emits in its one tick
    CHECK_FALSE(active(4));  // Lifetime 0 ends before it emits
    CHECK(particles(4) == 0);

    ps.update(0.06f); // just past one tick
    CHECK_FALSE(active(1));
    CHECK(particles(1) > 0); // what it emitted fades out
    CHECK(active(2));

    ps.update(0.1f); // just past two ticks
    CHECK_FALSE(active(2));
    ps.update(1.0f); // past its Repeattime of 6: it does not come back
    CHECK_FALSE(active(2));
    CHECK(active(3)); // negative Lifetime: emits on

    fs::remove_all(root);
}
