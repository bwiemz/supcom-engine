// --mesh-texture-test: a mesh blueprint that names empty textures draws, and
// leaves the Lua stack as it found it.
//
// A light assault bot's mesh blueprint loses its LODs' mesh names, so the
// mesh cache loads it the single-LOD way (its BlueprintId's _lod0.scm), and
// its LOD 1 names empty albedo, specular and normal textures. Reading those
// had popped more of the stack than it pushed.

#include "integration_tests.hpp"
#include "intel_probe.hpp"
#include "render_probe.hpp"

#include "lua/lua_state.hpp"
#include "sim/sim_state.hpp"

#include <lua.h>
#include <spdlog/spdlog.h>

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
    (void)spawn_unit(ctx, "__osc_mt_bot", "uel0106", "ARMY_1", *spot);

    OffscreenShots shots(ctx);
    if (!shots.ok()) {
        t.check(false, "renderer init (no Vulkan?)");
        return;
    }
    const int top = lua_gettop(ctx.L);
    (void)shots.shoot(*ctx.sim.terrain(), spot->x, spot->z, 20.0f);
    t.check(lua_gettop(ctx.L) == top, "drawing the bot leaves the Lua stack as it was");

    spdlog::info("=== MESH TEXTURE TEST: {} passed, {} failed ===", t.pass, t.fail);
}

} // namespace osc::test
