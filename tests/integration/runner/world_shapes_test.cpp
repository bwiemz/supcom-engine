// --world-shapes-test: FAF's world drawing (FA-Binary-Patches DrawFunc.cpp,
// WorldView.cpp): a world view with its custom rendering on has its
// OnRenderWorld(self, delta) called each frame, and what it draws there with
// UI_DrawCircle, UI_DrawLine and UI_DrawRect is drawn flat in the world,
// over it, at least two pixels thick.
//
// A main world view is made as retail's worldview.lua makes one; its
// OnRenderWorld draws an opaque green ring and a line on dry ground.

#include "integration_tests.hpp"
#include "intel_probe.hpp"
#include "render_probe.hpp"

#include "core/image.hpp"
#include "core/ui_registry_keys.hpp"
#include "lua/lua_state.hpp"
#include "map/terrain.hpp"
#include "renderer/renderer.hpp"
#include "renderer/world_shapes.hpp"
#include "sim/sim_state.hpp"
#include "ui/ui_control.hpp"

#include <fmt/format.h>
#include <lua.h>
#include <spdlog/spdlog.h>

#include <cmath>
#include <string>

namespace osc::test {

void test_world_shapes(TestContext& ctx) {
    spdlog::info("=== World shapes test: FAF's OnRenderWorld drawing ===");
    Tally t;
    const map::Terrain* terrain = ctx.sim.terrain();
    lua_State* L = ctx.L;
    lua_pushstring(L, "osc_ui_registry");
    lua_rawget(L, LUA_REGISTRYINDEX);
    auto* registry = static_cast<ui::UIControlRegistry*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    const auto spot = quiet_spot(ctx.sim);
    if (!terrain || !registry || !spot) {
        t.check(false, "a map, the UI's controls and dry ground");
        return;
    }
    OffscreenShots shots(ctx);
    if (!shots.ok()) {
        t.check(false, "renderer init (no Vulkan?)");
        return;
    }
    renderer::Renderer& r = shots.renderer();
    r.init_ui_caches(&ctx.vfs);
    const u32 errors_before = renderer::Renderer::validation_error_count();
    const f32 x = spot->x;
    const f32 z = spot->z;
    const f32 y = terrain->get_terrain_height(x, z);
    constexpr f32 kRadius = 12.0f;

    // The main world view, drawing a ring round the spot and a line east
    // of it from OnRenderWorld
    const auto made = ctx.lua_state.do_string(fmt::format(
        "do local v = {{}}\n"
        "setmetatable(v, {{__index = moho.UIWorldView}})\n"
        "moho.UIWorldView.__init(v, GetFrame(0), 'WorldCamera', 1, false, 'WorldCamera')\n"
        "v.Left:Set(0) v.Top:Set(0) v.Width:Set(GetFrame(0).Width()) "
        "v.Height:Set(GetFrame(0).Height())\n"
        "v.OnRenderWorld = function(self, delta)\n"
        "    rawset(_G, '__osc_shapes_calls', (rawget(_G, '__osc_shapes_calls') or 0) + 1)\n"
        "    rawset(_G, '__osc_shapes_delta', delta)\n"
        "    UI_DrawCircle({{{0}, {1}, {2}}}, {3}, 'ff00ff00', 0.6)\n"
        "    UI_DrawLine({{{0} + 20, {1}, {2}}}, {{{0} + 40, {1}, {2}}}, 'ffff0000', 0.6)\n"
        "    UI_DrawRect({{{0} - 40, {1}, {2} - 10}}, 20, 'ff0000ff', 0.6)\n"
        "end\n"
        "moho.UIWorldView.SetCustomRender(v, true)\n"
        "rawset(_G, '__osc_shapes_view', v)\n"
        "end\n",
        x, y, z, kRadius));
    t.check(static_cast<bool>(made), made
                                         ? std::string("a world view with an OnRenderWorld is made")
                                         : fmt::format("the view: {}", made.error().message));
    lua_pushstring(L, core::kWorldUiActiveKey);
    lua_pushboolean(L, 1);
    lua_rawset(L, LUA_REGISTRYINDEX);
    (void)shots.shoot_frame(*terrain, x + 10.0f, z, 120.0f);
    shots.set_ui(registry);
    const ImageRGBA8 frame = shots.grab();

    const auto& quads = r.world_shapes().quads();
    t.check(quads.size() == renderer::WorldShapes::kCircleSegments + 1 + 4,
            fmt::format("its OnRenderWorld drew a ring of {}, a line and a rect ({} quads)",
                        renderer::WorldShapes::kCircleSegments, quads.size()));
    lua_pushstring(L, "__osc_shapes_delta");
    lua_rawget(L, LUA_GLOBALSINDEX);
    const bool delta = lua_isnumber(L, -1) != 0;
    lua_pop(L, 1);
    t.check(delta, "OnRenderWorld is given the frame's time");
    // Its frame done, drawing is an error again
    const auto after = ctx.lua_state.do_string("UI_DrawLine({0, 0, 0}, {1, 0, 0}, 'red')\n");
    t.check(!after && after.error().message.find("outside of OnRenderWorld") != std::string::npos,
            "UI_DrawLine after OnRenderWorld has returned is an error");

    // On the ring, the pixels are its green; on the line, its red (each drawn
    // level at the spot's height)
    const auto colour_at = [&](f32 wx, f32 wz, int want_r, int want_g) {
        const auto s = screen_of(r, {wx, y, wz});
        if (!s || frame.pixels.empty()) return false;
        const auto px = static_cast<u32>((*s)[0]);
        const auto py = static_cast<u32>((*s)[1]);
        if (px >= frame.width || py >= frame.height) return false;
        const size_t i = (static_cast<size_t>(py) * frame.width + px) * 4;
        return std::abs(frame.pixels[i] - want_r) <= 8 &&
               std::abs(frame.pixels[i + 1] - want_g) <= 8 && frame.pixels[i + 2] <= 8;
    };
    int ring = 0;
    for (int k = 0; k < 8; ++k) {
        const f32 a = static_cast<f32>(k) * 0.785398f + 0.1f;
        ring += colour_at(x + kRadius * std::cos(a), z + kRadius * std::sin(a), 0, 255) ? 1 : 0;
    }
    const bool line = colour_at(x + 30.0f, z, 255, 0);
    // The rect's four edges, each at two points along it (its +z edge the
    // one a crossed quad would break)
    const auto blue_at = [&](f32 wx, f32 wz) {
        const auto s = screen_of(r, {wx, y, wz});
        if (!s || frame.pixels.empty()) return false;
        const auto px = static_cast<u32>((*s)[0]);
        const auto py = static_cast<u32>((*s)[1]);
        if (px >= frame.width || py >= frame.height) return false;
        const size_t i = (static_cast<size_t>(py) * frame.width + px) * 4;
        return frame.pixels[i] <= 8 && frame.pixels[i + 1] <= 8 && frame.pixels[i + 2] >= 247;
    };
    int edges = 0;
    for (const f32 f : {0.3f, 0.7f}) {
        edges += blue_at(x - 40.0f + 20.0f * f, z - 10.0f) ? 1 : 0; // -z edge
        edges += blue_at(x - 40.0f + 20.0f * f, z + 10.0f) ? 1 : 0; // +z edge
        edges += blue_at(x - 40.0f, z - 10.0f + 20.0f * f) ? 1 : 0; // -x edge
        edges += blue_at(x - 20.0f, z - 10.0f + 20.0f * f) ? 1 : 0; // +x edge
    }
    const bool inside = !colour_at(x, z, 0, 255);
    t.check(ring == 8 && line && inside && edges == 8,
            fmt::format("drawn over the world: {} of 8 points on the ring green, the line red "
                        "({}), the ring's middle not ({}), {} of 8 points on the rect's edges "
                        "blue",
                        ring, line, inside, edges));

    // Off, OnRenderWorld isn't called, and nothing is drawn
    lua_pushstring(L, "__osc_shapes_calls");
    lua_rawget(L, LUA_GLOBALSINDEX);
    const auto calls = static_cast<int>(lua_tonumber(L, -1));
    lua_pop(L, 1);
    (void)ctx.lua_state.do_string(
        "moho.UIWorldView.SetCustomRender(rawget(_G, '__osc_shapes_view'), false)\n");
    (void)shots.grab();
    lua_pushstring(L, "__osc_shapes_calls");
    lua_rawget(L, LUA_GLOBALSINDEX);
    const auto calls_after = static_cast<int>(lua_tonumber(L, -1));
    lua_pop(L, 1);
    t.check(calls_after == calls && r.world_shapes().quads().empty(),
            fmt::format("SetCustomRender(false): not called again ({} then {}), nothing drawn",
                        calls, calls_after));

    // A script error in OnRenderWorld is reported, not fatal
    (void)ctx.lua_state.do_string(
        "do local v = rawget(_G, '__osc_shapes_view')\n"
        "v.OnRenderWorld = function(self) UI_DrawCircle({0, 0, 0}, 1, 'nocolour') end\n"
        "moho.UIWorldView.SetCustomRender(v, true) end\n");
    (void)shots.grab();
    t.check(r.world_shapes().quads().empty(), "an OnRenderWorld that errors draws nothing");

    (void)ctx.lua_state.do_string(fmt::format(
        "do local v = rawget(_G, '__osc_shapes_view')\n"
        "v.OnRenderWorld = function(self)\n"
        "    UI_DrawLine({{{0} + 20, {1}, {2}}}, {{{0} + 40, {1}, {2}}}, 'ffff0000', 0.01)\n"
        "end end\n",
        x, y, z));
    const auto line_width = [](const renderer::Renderer& rr) {
        const auto& quads = rr.world_shapes().quads();
        if (quads.size() != 1) {
            return 0.0f;
        }
        const auto& c = quads[0].corners;
        return std::hypot(c[0].x - c[3].x, c[0].z - c[3].z);
    };
    (void)shots.grab();
    const f32 thin = line_width(r);
    shots.set_ui(nullptr);
    f32 thin_retina = 0.0f;
    {
        OffscreenShots retina(ctx, 2.0f);
        if (retina.ok()) {
            retina.renderer().init_ui_caches(&ctx.vfs);
            (void)retina.shoot_frame(*terrain, x + 10.0f, z, 120.0f);
            retina.set_ui(registry);
            (void)retina.grab();
            thin_retina = line_width(retina.renderer());
            retina.set_ui(nullptr);
        }
    }
    t.check(thin > 0.01f && std::abs(thin_retina - thin) < thin * 0.01f,
            fmt::format("a line thinner than two of the UI's points is two points wide, as wide "
                        "in the world at a display scale of 2 as at 1: {:.4f} and {:.4f}",
                        thin_retina, thin));

    lua_pushstring(L, core::kWorldUiActiveKey);
    lua_pushnil(L);
    lua_rawset(L, LUA_REGISTRYINDEX);
    t.check(renderer::Renderer::validation_error_count() == errors_before,
            "no Vulkan validation errors drawing it");
    spdlog::info("World shapes test: {} passed, {} failed", t.pass, t.fail);
}

} // namespace osc::test
