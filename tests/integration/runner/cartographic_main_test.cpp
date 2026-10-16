// --cartographic-main-test: a cartographic main view (retail's map options
// and ToggleMainCartographicView call SetCartographic on it). As Moho's
// CUIWorldView::SetOrthographic and WRenViewport::RenderCartographic have
// it:
// - its camera goes orthographic (a minimap view's flag leaves it alone,
//   and a new main view starts in perspective);
// - the scene is the map's cartographic frame through that camera, pixel
//   for pixel the frame drawn over the same world rect, alpha 0 (no glow);
// - the strategic icons are on at every zoom while it draws;
// - a click picks the ground straight under the cursor;
// - SetCartographic(false) brings the world back.

#include "integration_tests.hpp"
#include "intel_probe.hpp"
#include "render_probe.hpp"

#include "core/image.hpp"
#include "core/ui_registry_keys.hpp"
#include "lua/lua_state.hpp"
#include "lua/user_bindings.hpp"
#include "map/terrain.hpp"
#include "renderer/cartographic_renderer.hpp"
#include "renderer/range_overlays.hpp"
#include "renderer/renderer.hpp"
#include "renderer/strategic_icon_renderer.hpp"
#include "renderer/texture_cache.hpp"
#include "sim/category_expr.hpp"
#include "sim/sim_state.hpp"
#include "ui/ui_control.hpp"

#include <fmt/format.h>
#include <lua.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <unordered_set>

namespace osc::test {

namespace {

/// A view as retail's scripts make one: `name` is its global, `camera` and
/// `minimap` its __init's.
std::string make_view(const std::string& name, const std::string& camera, bool minimap) {
    return fmt::format("do local v = {{}}\n"
                       "setmetatable(v, {{__index = moho.UIWorldView}})\n"
                       "moho.UIWorldView.__init(v, GetFrame(0), '{0}', 1, {1}, 'WorldCamera')\n"
                       "v.Left:Set(0) v.Top:Set(0) v.Width:Set(GetFrame(0).Width()) "
                       "v.Height:Set(GetFrame(0).Height())\n"
                       "rawset(_G, '{2}', v)\n"
                       "end\n",
                       camera, minimap ? "true" : "false", name);
}

std::string set_cartographic(const std::string& name, bool on) {
    return fmt::format("moho.UIWorldView.SetCartographic(rawget(_G, '{}'), {})\n", name,
                       on ? "true" : "false");
}

bool is_cartographic(lua_State* L, lua::LuaState& lua, const std::string& name) {
    (void)lua.do_string(fmt::format(
        "rawset(_G, '__osc_cm_is', moho.UIWorldView.IsCartographic(rawget(_G, '{}')))\n", name));
    lua_pushstring(L, "__osc_cm_is");
    lua_rawget(L, LUA_GLOBALSINDEX);
    const bool on = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    return on;
}

} // namespace

void test_cartographic_main(TestContext& ctx) {
    spdlog::info("=== Cartographic main view test ===");
    Tally t;
    const map::Terrain* terrain = ctx.sim.terrain();
    lua_State* L = ctx.L;
    lua_pushstring(L, "osc_ui_registry");
    lua_rawget(L, LUA_REGISTRYINDEX);
    auto* registry = static_cast<ui::UIControlRegistry*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    const u32 acu = army_acu_id(ctx.sim, 0);
    const auto* acu_entity = ctx.sim.entity_registry().find(acu);
    if (!terrain || !registry || !acu_entity) {
        t.check(false, "a map, the UI's controls and ARMY_1's commander");
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
    renderer::Camera& cam = r.camera();
    const f32 x = acu_entity->position().x;
    const f32 z = acu_entity->position().z;

    // ARMY_1's commander, which a perspective view this near shows as its
    // mesh, not its icon; the icon's texture loaded, so only the zoom
    // decides whether it shows
    r.set_player_army(0);
    {
        (void)ctx.lua_state.do_string(
            fmt::format("rawset(_G, '__osc_cm_icon', __blueprints['{}'].StrategicIconName or '')\n",
                        acu_entity->blueprint_id()));
        lua_pushstring(L, "__osc_cm_icon");
        lua_rawget(L, LUA_GLOBALSINDEX);
        std::string name = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "";
        lua_pop(L, 1);
        for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        const renderer::GPUTexture* icon = r.texture_cache().get_blocking(
            fmt::format("{}{}_rest.dds", renderer::StrategicIconRenderer::kIconDirectory, name));
        t.check(icon && icon->width > 0, fmt::format("the commander's icon ({}) is loaded", name));
    }

    // A main view made before the window published its renderer (a --map
    // launch builds FA's interface at boot), made cartographic meanwhile:
    // bound late, its camera takes that mode
    lua_pushstring(L, "__osc_renderer");
    lua_pushnil(L);
    lua_rawset(L, LUA_REGISTRYINDEX);
    (void)ctx.lua_state.do_string(make_view("__osc_cm_early", "WorldCamera", false) +
                                  set_cartographic("__osc_cm_early", true));
    t.check(!cam.ortho(), "a view with no camera yet leaves the camera alone");

    // The views' bindings reach this renderer's camera
    lua_pushstring(L, "__osc_renderer");
    lua_pushlightuserdata(L, &r);
    lua_rawset(L, LUA_REGISTRYINDEX);
    lua::bind_unbound_world_views(L);
    t.check(cam.ortho() && is_cartographic(L, ctx.lua_state, "__osc_cm_early"),
            "bound when the renderer is published, its camera takes its cartographic mode");
    (void)ctx.lua_state.do_string(set_cartographic("__osc_cm_early", false));

    // A minimap view's flag leaves the camera as it is
    const auto made = ctx.lua_state.do_string(make_view("__osc_cm_mini", "MiniMap", true) +
                                              set_cartographic("__osc_cm_mini", true) +
                                              make_view("__osc_cm_main", "WorldCamera", false));
    t.check(static_cast<bool>(made), made ? std::string("a minimap and a main world view are made")
                                          : fmt::format("the views: {}", made.error().message));
    t.check(is_cartographic(L, ctx.lua_state, "__osc_cm_mini") && !cam.ortho(),
            "the minimap is cartographic, the camera still in perspective");
    lua_pushstring(L, core::kWorldUiActiveKey);
    lua_pushboolean(L, 1);
    lua_rawset(L, LUA_REGISTRYINDEX);

    constexpr f32 kDistance = 40.0f; // nearer than the scout's icon shows
    (void)shots.shoot_frame(*terrain, x, z, kDistance);
    const auto scene = [&]() {
        shots.recapture();
        renderer::Renderer::SceneImage image;
        r.request_scene_capture(
            [&](renderer::Renderer::SceneImage captured) { image = std::move(captured); });
        shots.redraw();
        return image;
    };
    const renderer::Renderer::SceneImage world = scene();
    const u32 icons_world = r.strategic_icons().quad_count();
    t.check(
        !r.cartographic_main() && icons_world == 0,
        fmt::format("in perspective, the world is drawn and no icons this near ({})", icons_world));

    // Cartographic: the camera goes orthographic
    (void)ctx.lua_state.do_string(set_cartographic("__osc_cm_main", true));
    t.check(cam.ortho() && is_cartographic(L, ctx.lua_state, "__osc_cm_main"),
            "SetCartographic(true) on the main view: its camera is orthographic");
    const renderer::Renderer::SceneImage map = scene();
    t.check(r.cartographic_main(), "the frame draws the main view cartographic");
    const u32 icons_map = r.strategic_icons().quad_count();
    t.check(icons_map > 0,
            fmt::format("its strategic icons are on at this zoom ({} quads)", icons_map));

    // The scene is the frame drawn over the camera's world rect: the zoom
    // across, centred on the focus, less Moho's shift of half a unit's
    // worth of the view (1/int(zoom) of it across, 1/int(extent) down)
    const u32 w = r.width();
    const u32 h = r.height();
    const f32 zoom = cam.zoom();
    const f32 extent = zoom / (static_cast<f32>(w) / static_cast<f32>(h));
    const f32 x0 = cam.focus_x() - zoom * 0.5f + zoom * 0.5f / std::floor(zoom);
    const f32 z0 = cam.focus_z() - extent * 0.5f + extent * 0.5f / std::floor(extent);
    renderer::CartographicRenderer::View view;
    view.width = w;
    view.height = h;
    view.x0 = x0;
    view.z0 = z0;
    view.x1 = x0 + zoom;
    view.z1 = z0 + extent;
    // How many of the scene's pixels are the frame's (each channel within
    // one step), and how many have alpha
    struct Match {
        size_t same = 0, glow = 0, pixels = 0;
        bool ok = false;
    };
    const auto match = [&](const renderer::Renderer::SceneImage& got,
                           const renderer::Renderer::CartographicOverlayInput* overlays) {
        Match m;
        const auto drawn = r.capture_cartographic(*terrain, view, overlays);
        if (!drawn || got.rgba.size() != static_cast<size_t>(w) * h * 4 ||
            drawn->output.size() != got.rgba.size())
            return m;
        m.ok = true;
        m.pixels = static_cast<size_t>(w) * h;
        for (size_t i = 0; i < m.pixels; ++i) {
            bool same = true;
            for (size_t c = 0; c < 3; ++c) {
                const auto v = static_cast<int>(std::lround(got.rgba[i * 4 + c] * 255.0f));
                same = same && std::abs(v - static_cast<int>(drawn->output[i * 4 + c])) <= 1;
            }
            m.same += same ? 1 : 0;
            m.glow += got.rgba[i * 4 + 3] != 0.0f ? 1 : 0;
        }
        return m;
    };
    {
        const Match m = match(map, nullptr);
        // A pixel whose point lies on a ramp texel's edge may take its
        // neighbour (the camera's interpolation is not the frame's)
        t.check(m.ok && m.same * 1000 >= m.pixels * 995,
                fmt::format("the scene is the cartographic frame over the camera's rect: {} of "
                            "{} pixels",
                            m.same, m.pixels));
        t.check(m.ok && m.glow == 0, fmt::format("with no glow: {} pixels have alpha", m.glow));
        bool differs = world.rgba.size() == map.rgba.size();
        size_t changed = 0;
        for (size_t i = 0; differs && i < map.rgba.size(); ++i)
            changed += std::abs(world.rgba[i] - map.rgba[i]) > 0.02f ? 1 : 0;
        t.check(changed * 2 > map.rgba.size(),
                fmt::format("which is not the world drawn before ({} of {} channels changed)",
                            changed, map.rgba.size()));
    }

    // In a game, the frame takes the fog (RenderFogOfWar into it, as the
    // minimap's does), the army's vision circles, and the range rings at the
    // main view's zoom
    {
        renderer::RangeProfile profile; // retail's DirectFire (rangeoverlayparams.lua)
        profile.name = "DirectFire";
        profile.categories = sim::CategoryExpr::all();
        profile.selected = renderer::range_color(0x08ff5253u);
        profile.inner = {0.02f, 2.0f};
        profile.outer = {0.04f, 4.0f};
        r.range_overlays().set_profile(profile);
        r.set_fog_enabled(true);
        ctx.sim.tick(); // the army's sight, which a tick paints
        const renderer::Renderer::SceneImage fogged = scene();
        renderer::Renderer::CartographicOverlayInput in;
        in.fog = r.minimap_vision().circles();
        const size_t circles = in.fog->size();
        const Match m = match(fogged, &in);
        size_t dimmer = 0;
        for (size_t i = 0; m.ok && i < map.rgba.size(); i += 4)
            dimmer += fogged.rgba[i] + fogged.rgba[i + 1] + fogged.rgba[i + 2] + 0.01f <
                              map.rgba[i] + map.rgba[i + 1] + map.rgba[i + 2]
                          ? 1
                          : 0;
        t.check(circles > 0 && r.minimap_vision().drawn() && dimmer > 0 &&
                    m.same * 1000 >= m.pixels * 995,
                fmt::format("with the fog on, the frame is fogged as the frame drawn with the "
                            "army's {} vision circles: {} of {} pixels ({} dimmed)",
                            circles, m.same, m.pixels, dimmer));
        r.range_overlays().settings().render_selected = true;
        const std::unordered_set<u32> selected{acu};
        shots.recapture();
        shots.redraw(&selected);
        const auto& rings = r.main_frame_ranges().drawn();
        const auto& world_rings = r.range_renderer().drawn();
        t.check(rings.size() == 1 && world_rings.size() == 1 &&
                    rings[0].outer_thickness == world_rings[0].outer_thickness,
                fmt::format("the commander's range ring in the frame, its line {:.3f} thick at "
                            "the main view's zoom ({:.3f} in the world view's)",
                            rings.empty() ? 0.0f : rings[0].outer_thickness,
                            world_rings.empty() ? 0.0f : world_rings[0].outer_thickness));
        r.range_overlays().settings().render_selected = false;
        r.set_fog_enabled(false);
    }

    // A click at a pixel picks the ground straight under it: the point drawn
    // there
    {
        const f32 sx = static_cast<f32>(w) * 0.3f;
        const f32 sy = static_cast<f32>(h) * 0.7f;
        f32 px = 0;
        f32 py = 0;
        f32 pz = 0;
        const bool hit =
            cam.pick_ground(sx, sy, static_cast<f32>(w), static_cast<f32>(h), terrain, px, py, pz);
        const auto back = hit ? screen_of(r, {px, py, pz}) : std::nullopt;
        t.check(back && std::abs((*back)[0] - sx) < 0.5f && std::abs((*back)[1] - sy) < 0.5f,
                fmt::format("a click picks the ground straight under the cursor ({:.1f}, {:.1f})",
                            px, pz));
    }

    // Off again: the world, in perspective
    (void)ctx.lua_state.do_string(set_cartographic("__osc_cm_main", false));
    const renderer::Renderer::SceneImage again = scene();
    t.check(!cam.ortho() && !r.cartographic_main() && r.strategic_icons().quad_count() == 0,
            "SetCartographic(false): the world in perspective, no icons this near");
    // (a tick on: the commander has moved a little since)
    size_t kept = 0;
    for (size_t i = 0; again.rgba.size() == world.rgba.size() && i < world.rgba.size(); ++i)
        kept += std::abs(again.rgba[i] - world.rgba[i]) < 0.02f ? 1 : 0;
    t.check(
        kept * 100 >= world.rgba.size() * 98,
        fmt::format("the world is drawn as it was ({} of {} channels)", kept, world.rgba.size()));

    // A new main view's camera starts in perspective (its Register then
    // restores a saved mode)
    (void)ctx.lua_state.do_string(set_cartographic("__osc_cm_main", true));
    t.check(cam.ortho(), "cartographic again");
    (void)ctx.lua_state.do_string(make_view("__osc_cm_main2", "WorldCamera", false));
    t.check(!cam.ortho(), "a new main view: the camera in perspective");

    t.check(renderer::Renderer::validation_error_count() == errors_before,
            "no Vulkan validation errors");
    lua_pushstring(L, "__osc_renderer");
    lua_pushnil(L);
    lua_rawset(L, LUA_REGISTRYINDEX);
    spdlog::info("Cartographic main view test: {} passed, {} failed", t.pass, t.fail);
}

} // namespace osc::test
