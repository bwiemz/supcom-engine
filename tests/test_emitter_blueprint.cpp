#include <catch2/catch_test_macros.hpp>

#include "lua/lua_state.hpp"
#include "renderer/emitter_blueprint.hpp"
#include "vfs/directory_mount.hpp"
#include "vfs/virtual_file_system.hpp"

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
    const fs::path root = fs::temp_directory_path() / "osc_emitter_bp_test";
    fs::remove_all(root);
    fs::create_directories(root / "effects" / "Emitters");
    {
        std::ofstream(root / "effects" / "Emitters" / "mist_emit.bp") << R"(
EmitterBlueprint {
    BlueprintId = 'mist',
    Lifetime = 80.00,
    Repeattime = 120.00,
    Blendmode = 3.00,
    LODCutoff = 100.00,
    Texture = [[/textures/particles/cloud_smoke_alpha_10.dds]],
    RampTexture = [[/textures/particles/ramp_white_02.dds]],
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
        CHECK(bp->blendmode == 3u);
        CHECK(bp->lod_cutoff == 100.0f);
        CHECK(bp->texture_path == "/textures/particles/cloud_smoke_alpha_10.dds");
        CHECK(bp->ramp_texture_path == "/textures/particles/ramp_white_02.dds");
        const auto* spark = cache.get("/effects/emitters/spark_emit.bp", L);
        REQUIRE(spark != nullptr);
        CHECK(spark->blendmode == 3u);
        CHECK(spark->lifetime == -1.0f);
        CHECK(spark->repeattime == 0.0f);                            // Moho's default
        CHECK(cache.get("/effects/emitters/mist_emit.bp", L) == bp); // cached
        CHECK(lua_gettop(L) == top);
        lua_getglobal(L, "EmitterBlueprint");
        CHECK(lua_isnil(L, -1));
        lua_pop(L, 1);
    }

    SECTION("missing or emitter-less files fail, and stay failed") {
        CHECK(cache.get("/effects/emitters/missing_emit.bp", L) == nullptr);
        CHECK(cache.get("/effects/emitters/missing_emit.bp", L) == nullptr);
        CHECK(cache.get("/effects/emitters/empty_emit.bp", L) == nullptr);
        CHECK(lua_gettop(L) == top);
    }
    fs::remove_all(root);
}
