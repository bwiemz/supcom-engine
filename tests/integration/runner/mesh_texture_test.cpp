// --mesh-texture-test: a mesh blueprint that names empty textures draws, and
// leaves the Lua stack as it found it.
//
// A light assault bot's mesh blueprint loses its LODs' mesh names, so the
// mesh cache loads it the single-LOD way (its BlueprintId's _lod0.scm), and
// its LOD 1 names empty albedo, specular and normal textures. Reading those
// had popped more of the stack than it pushed. The bot must be drawn with its
// own mesh and the textures named after its mesh blueprint.

#include "integration_tests.hpp"
#include "intel_probe.hpp"
#include "render_probe.hpp"

#include "lua/lua_state.hpp"
#include "renderer/mesh_cache.hpp"
#include "renderer/renderer.hpp"
#include "renderer/unit_renderer.hpp"
#include "sim/sim_state.hpp"

#include <lua.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <string>

namespace osc::test {

void test_mesh_texture(TestContext& ctx) {
    spdlog::info("=== MESH TEXTURE TEST: empty texture names leave the stack alone ===");
    Tally t;
    const auto spot = quiet_spot(ctx.sim);
    if (!spot) {
        t.check(false, "dry ground 60 from every unit");
        return;
    }
    const auto ran = ctx.lua_state.do_string(R"(
        local mesh = rawget(_G, '__blueprints')['/units/uel0106/uel0106_mesh']
        mesh.LODs = { { AlbedoName = '', SpecularName = '', NormalsName = '' } }
    )");
    t.check(static_cast<bool>(ran), "the mesh blueprint names empty textures");
    const u32 bot = spawn_unit(ctx, "__osc_mt_bot", "uel0106", "ARMY_1", *spot);
    t.check(bot != 0, "the bot is made");

    OffscreenShots shots(ctx);
    if (!shots.ok()) {
        t.check(false, "renderer init (no Vulkan?)");
        return;
    }
    const int top = lua_gettop(ctx.L);
    (void)shots.shoot(*ctx.sim.terrain(), spot->x, spot->z, 20.0f);
    t.check(lua_gettop(ctx.L) == top, "drawing the bot leaves the Lua stack as it was");

    const auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    };
    const renderer::GPUMesh* mesh = nullptr;
    for (const auto& g : shots.renderer().unit_renderer().mesh_groups())
        if (g.mesh && g.instance_count > 0 &&
            lower(g.mesh->texture_path).find("/units/uel0106/") != std::string::npos)
            mesh = g.mesh;
    t.check(mesh != nullptr, "the bot is drawn with its own mesh");
    if (!mesh) return;
    t.check(lower(mesh->texture_path) == "/units/uel0106/uel0106_albedo.dds" &&
                lower(mesh->specteam_path) == "/units/uel0106/uel0106_specteam.dds" &&
                lower(mesh->normal_path) == "/units/uel0106/uel0106_normalsts.dds",
            "its textures are the ones named after its mesh blueprint: " + mesh->texture_path +
                ", " + mesh->specteam_path + ", " + mesh->normal_path);

    spdlog::info("=== MESH TEXTURE TEST: {} passed, {} failed ===", t.pass, t.fail);
}

} // namespace osc::test
