#include <catch2/catch_test_macros.hpp>

#include "lua/lua_state.hpp"
#include "renderer/emitter_blueprint.hpp"
#include "renderer/beam_blueprint.hpp"
#include "renderer/trail_blueprint.hpp"
#include "renderer/trail_renderer.hpp"
#include "renderer/texture_cache.hpp"
#include "renderer/camera.hpp"
#include "sim/world_snapshot.hpp"
#include "renderer/render_scripts.hpp"
#include "vfs/directory_mount.hpp"
#include "vfs/virtual_file_system.hpp"
#include "support/temp_path.hpp"

extern "C" {
#include <lua.h>
}

#include <filesystem>
#include <fstream>
#include <memory>

TEST_CASE("emitter blueprints load through the VFS as FA writes them", "[renderer][emitter]") {
    // Retail keeps them in effects.scd under effects/Emitters/, and each file
    // is an `EmitterBlueprint { ... }` statement, not a `return`.
    namespace fs = std::filesystem;
    const fs::path root = osc::test::unique_temp_path("osc_emitter_bp_test");
    fs::create_directories(root / "effects" / "Emitters");
    {
        std::ofstream(root / "effects" / "Emitters" / "mist_emit.bp") << R"(
EmitterBlueprint {
    BlueprintId = 'mist',
    Lifetime = 80.00,
    Repeattime = 120.00,
    Blendmode = 3.00,
    LODCutoff = 100.00,
    TextureFramecount = 4.00,
    Texture = [[/textures/particles/cloud_smoke_alpha_10.dds]],
    RampTexture = [[/textures/particles/ramp_white_02.dds]],
    LifetimeCurve = {
        XRange = 100.00,
        Keys = {
            { x=60.000,y=12.000,z=2.000 },
            { x=10.000,y=20.000,z=6.000 },
        },
    },
}
)";
        // Some of retail's blueprints spell it BlendMode.
        std::ofstream(root / "effects" / "Emitters" / "spark_emit.bp") << R"(
EmitterBlueprint {
    BlendMode = 3,
    Lifetime = -1,
}
)";
        std::ofstream(root / "effects" / "Emitters" / "empty_emit.bp") << "local x = 1\n";
    }
    osc::vfs::VirtualFileSystem vfs;
    vfs.mount("/", std::make_unique<osc::vfs::DirectoryMount>(root));

    osc::lua::LuaState lua;
    lua_State* L = lua.raw();
    osc::renderer::EmitterBlueprintCache cache;

    SECTION("no VFS, no blueprint") {
        CHECK(cache.get("/effects/emitters/mist_emit.bp", L) == nullptr);
    }

    cache.set_vfs(&vfs);
    const int top = lua_gettop(L);

    SECTION("loads, parses, and restores the global") {
        const auto* bp = cache.get("/effects/emitters/mist_emit.bp", L);
        REQUIRE(bp != nullptr);
        CHECK(bp->lifetime == 80.0f);
        CHECK(bp->repeattime == 120.0f); // Moho's spelling
        CHECK(bp->blendmode == 3);
        CHECK(bp->lod_cutoff == 100.0f);
        CHECK(bp->frame_count == 4.0f); // TextureFramecount, as retail spells it
        CHECK(bp->animated());
        CHECK(bp->texture == "/textures/particles/cloud_smoke_alpha_10.dds");
        CHECK(bp->ramp_texture == "/textures/particles/ramp_white_02.dds");
        // Keys in order of x, however written; the longest life their peak.
        const auto& life = bp->curve(osc::renderer::kLifetime).keys;
        REQUIRE(life.size() == 2);
        CHECK(life[0].x == 10.0f);
        CHECK(life[1].x == 60.0f);
        CHECK(bp->max_lifetime == 23); // ceil(20 + 6/2)
        const auto* spark = cache.get("/effects/emitters/spark_emit.bp", L);
        REQUIRE(spark != nullptr);
        CHECK(spark->blendmode == 3);
        CHECK(spark->lifetime == -1.0f);
        // Moho's defaults (REmitterBlueprint).
        CHECK(spark->repeattime == 0.0f);
        CHECK(spark->lod_cutoff == 100.0f);
        CHECK(spark->local_velocity);
        CHECK(spark->snap_to_waterline);
        CHECK(spark->interpolate_emission);
        CHECK_FALSE(spark->flat);
        CHECK(spark->strip_count == 1.0f);
        CHECK_FALSE(spark->animated());
        CHECK(cache.get("/effects/emitters/mist_emit.bp", L) == bp); // cached
        CHECK(lua_gettop(L) == top);
        lua_getglobal(L, "EmitterBlueprint");
        CHECK(lua_isnil(L, -1));
        lua_pop(L, 1);
    }

    SECTION("busy draws defer script cache misses and retry after the sim settles") {
        std::ofstream(root / "effects" / "Emitters" / "deferred.bp") << R"(
            loads = (loads or 0) + 1
            EmitterBlueprint { Lifetime = 12 }
            BeamBlueprint { Lifetime = 13 }
            TrailEmitterBlueprint { Lifetime = 14 }
        )";
        osc::renderer::BeamBlueprintCache beams;
        osc::renderer::TrailBlueprintCache trails;
        beams.set_vfs(&vfs);
        trails.set_vfs(&vfs);
        const std::string path = "/effects/emitters/deferred.bp";
        osc::renderer::TrailRenderer renderer;
        osc::renderer::TextureCache textures;
        osc::renderer::Camera camera;
        osc::sim::WorldSnapshot snapshot;
        snapshot.tick = 12;
        osc::sim::EffectRecord effect;
        effect.id = 10;
        effect.type = osc::sim::EffectType::TRAIL_EMITTER;
        effect.anchored = true;
        effect.blueprint_path = path;
        snapshot.effects.push_back(effect);
        const osc::sim::FrameView view(&snapshot, &snapshot, 1.0f);
        {
            const osc::renderer::RenderScriptScope busy(false);
            CHECK(cache.get(path, L) == nullptr);
            CHECK(beams.get(path, L) == nullptr);
            CHECK(trails.get(path, L) == nullptr);
            renderer.update(view, camera, nullptr, trails, textures, L, nullptr, 0);
            CHECK_FALSE(renderer.draws_effect(10));
            lua_getglobal(L, "loads");
            CHECK(lua_isnil(L, -1));
            lua_pop(L, 1);
        }
        const auto* emitter = cache.get(path, L);
        const auto* beam = beams.get(path, L);
        const auto* trail = trails.get(path, L);
        REQUIRE(emitter);
        REQUIRE(beam);
        REQUIRE(trail);
        CHECK(emitter->lifetime == 12);
        CHECK(beam->lifetime == 13);
        CHECK(trail->lifetime == 14);
        renderer.update(view, camera, nullptr, trails, textures, L, nullptr, 0);
        CHECK(renderer.draws_effect(10)); // same tick, now settled
        {
            const osc::renderer::RenderScriptScope busy(false);
            CHECK(cache.get(path, L) == emitter);
            CHECK(beams.get(path, L) == beam);
            CHECK(trails.get(path, L) == trail);
            lua_getglobal(L, "loads");
            CHECK(lua_tonumber(L, -1) == 3);
            lua_pop(L, 1);
        }
        CHECK(lua_gettop(L) == top);
    }

    SECTION("missing or emitter-less files fail, and stay failed") {
        CHECK(cache.get("/effects/emitters/missing_emit.bp", L) == nullptr);
        CHECK(cache.get("/effects/emitters/missing_emit.bp", L) == nullptr);
        CHECK(cache.get("/effects/emitters/empty_emit.bp", L) == nullptr);
        CHECK(lua_gettop(L) == top);
    }
    fs::remove_all(root);
}
