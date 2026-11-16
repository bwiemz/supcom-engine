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
#include "support/effect_blueprints.hpp"

extern "C" {
#include <lua.h>
}

#include <string>

TEST_CASE("emitter blueprints read as FA writes them", "[renderer][emitter]") {
    // Retail keeps them in effects.scd under effects/Emitters/, and each file
    // is an `EmitterBlueprint { ... }` statement, not a `return`.
    osc::lua::LuaState lua;
    lua_State* L = lua.raw();
    osc::test::EffectBlueprints blueprints(lua);
    blueprints.add("/effects/Emitters/mist_emit.bp", R"(
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
)");
    // Some of retail's blueprints spell it BlendMode.
    blueprints.add("/effects/Emitters/spark_emit.bp", R"(
EmitterBlueprint {
    BlendMode = 3,
    Lifetime = -1,
}
)");
    blueprints.add("/effects/Emitters/ray_beam.bp", "BeamBlueprint { Length = 4 }\n");
    osc::renderer::EmitterBlueprintCache cache;

    SECTION("no blueprints, no blueprint") {
        CHECK(cache.get("/effects/emitters/mist_emit.bp", L) == nullptr);
    }

    cache.set_blueprints(blueprints.store());
    const int top = lua_gettop(L);

    SECTION("reads and parses the registered blueprint, and leaves the stack") {
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
    }

    SECTION("busy draws defer cache misses and retry after the sim settles") {
        blueprints.add("/effects/emitters/deferred_emit.bp", "EmitterBlueprint { Lifetime = 12 }");
        blueprints.add("/effects/emitters/deferred_beam.bp", "BeamBlueprint { Lifetime = 13 }");
        blueprints.add("/effects/emitters/deferred_trail.bp",
                       "TrailEmitterBlueprint { Lifetime = 14 }");
        osc::renderer::BeamBlueprintCache beams;
        osc::renderer::TrailBlueprintCache trails;
        beams.set_blueprints(blueprints.store());
        trails.set_blueprints(blueprints.store());
        const std::string emitter_path = "/effects/emitters/deferred_emit.bp";
        const std::string beam_path = "/effects/emitters/deferred_beam.bp";
        const std::string trail_path = "/effects/emitters/deferred_trail.bp";
        osc::renderer::TrailRenderer renderer;
        osc::renderer::TextureCache textures;
        osc::renderer::Camera camera;
        osc::sim::WorldSnapshot snapshot;
        snapshot.tick = 12;
        osc::sim::EffectRecord effect;
        effect.id = 10;
        effect.type = osc::sim::EffectType::TRAIL_EMITTER;
        effect.anchored = true;
        effect.blueprint_path = trail_path;
        snapshot.effects.push_back(effect);
        const osc::sim::FrameView view(&snapshot, &snapshot, 1.0f);
        {
            const osc::renderer::RenderScriptScope busy(false);
            CHECK(cache.get(emitter_path, L) == nullptr);
            CHECK(beams.get(beam_path, L) == nullptr);
            CHECK(trails.get(trail_path, L) == nullptr);
            renderer.update(view, camera, nullptr, trails, textures, L, nullptr, 0);
            CHECK_FALSE(renderer.draws_effect(10));
        }
        const auto* emitter = cache.get(emitter_path, L);
        const auto* beam = beams.get(beam_path, L);
        const auto* trail = trails.get(trail_path, L);
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
            CHECK(cache.get(emitter_path, L) == emitter);
            CHECK(beams.get(beam_path, L) == beam);
            CHECK(trails.get(trail_path, L) == trail);
        }
        CHECK(lua_gettop(L) == top);
    }

    SECTION("names of no emitter blueprint fail, and stay failed") {
        CHECK(cache.get("/effects/emitters/missing_emit.bp", L) == nullptr);
        CHECK(cache.get("/effects/emitters/missing_emit.bp", L) == nullptr);
        // A beam's: Moho looks an emitter up among the emitters alone.
        CHECK(cache.get("/effects/emitters/ray_beam.bp", L) == nullptr);
        CHECK(lua_gettop(L) == top);
    }
}

TEST_CASE("Effects draw with their registered blueprints: a mod's, not the file's",
          "[renderer][emitter]") {
    // Moho makes an effect from the blueprint the rules registered under its
    // name (CEffectManagerImpl's GetEmitterBlueprint, GetBeamBlueprint and
    // GetTrailBlueprint lookups), so what Blueprints.lua registered -- after
    // the mods' blueprints, their Merges and ModBlueprints -- is what draws;
    // it never reads the file again. A mod replaces retail's effect blueprint
    // with one whose Source names that file, so that it takes the same id.
    osc::lua::LuaState lua;
    lua_State* L = lua.raw();
    osc::test::EffectBlueprints blueprints(lua);
    blueprints.add("/effects/Emitters/flash_01_emit.bp",
                   "EmitterBlueprint { Lifetime = 0.1,"
                   " Texture = [[/textures/particles/flare_lens_add_02.dds]] }\n");
    blueprints.add("/effects/Emitters/ray_01_beam.bp",
                   "BeamBlueprint { Length = 4, TextureName = '/textures/ray.dds' }\n");
    blueprints.add("/effects/Emitters/smoke_01_trail.bp",
                   "TrailEmitterBlueprint { TrailLength = 5, Size = 0.5 }\n");
    // The mod's, run after retail's as LoadBlueprints runs the active mods'.
    blueprints.add("/mods/osc/effects/flash_emit.bp",
                   "EmitterBlueprint { Source = '/effects/emitters/flash_01_emit.bp',\n"
                   "    Lifetime = 7, Texture = '/Textures/Particles/Glow_02.dds' }\n");
    blueprints.add("/mods/osc/effects/ray_beam.bp",
                   "BeamBlueprint { Source = '/effects/emitters/ray_01_beam.bp', Length = 9 }\n");
    blueprints.add("/mods/osc/effects/smoke_trail.bp",
                   "TrailEmitterBlueprint { Source = '/effects/emitters/smoke_01_trail.bp',\n"
                   "    TrailLength = 12 }\n");

    osc::renderer::EmitterBlueprintCache emitters;
    osc::renderer::BeamBlueprintCache beams;
    osc::renderer::TrailBlueprintCache trails;
    emitters.set_blueprints(blueprints.store());
    beams.set_blueprints(blueprints.store());
    trails.set_blueprints(blueprints.store());

    const auto* flash = emitters.get("/effects/emitters/flash_01_emit.bp", L);
    REQUIRE(flash);
    CHECK(flash->lifetime == 7.0f);
    CHECK(flash->texture == "/textures/particles/glow_02.dds");
    const auto* ray = beams.get("/effects/emitters/ray_01_beam.bp", L);
    REQUIRE(ray);
    CHECK(ray->length == 9.0f);
    CHECK(ray->texture.empty()); // the mod's replaced it whole: no Merge
    const auto* smoke = trails.get("/effects/emitters/smoke_01_trail.bp", L);
    REQUIRE(smoke);
    CHECK(smoke->trail_length == 12.0f);
    CHECK(smoke->size == 0.0f);

    // The name as STR_InitFilename makes it: in any case, either slash.
    const auto* named = emitters.get("\\EFFECTS\\Emitters\\Flash_01_emit.bp", L);
    REQUIRE(named);
    CHECK(named->lifetime == 7.0f);
    // The mod's own file names no blueprint of its own: its Source does.
    CHECK(emitters.get("/mods/osc/effects/flash_emit.bp", L) == nullptr);
}
