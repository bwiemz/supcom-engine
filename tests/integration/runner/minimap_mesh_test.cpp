// --minimap-mesh-test: the cartographic minimap's meshes (Moho's
// Cartographic::RenderMeshes; minimap_meshes.hpp):
// - a minimap view's camera takes cam_DefaultMiniLOD's LOD scale when made,
//   and its LOD metric is the view's width times it;
// - the meshes drawn are those whose LODs reach the metric (read here from
//   the blueprints apart from the renderer: the largest LODCutoff, 1000
//   where one gives none);
// - at retail's 1.8 a 20 km map's minimap draws none;
// - they draw into the minimap's output, after Stage 1, in their
//   cartographic technique;
// - FAF's patched game draws none while cam_DefaultMiniLOD is 0, retail's
//   keeps the scale its view was made with.

#include "integration_tests.hpp"
#include "render_probe.hpp"

#include "core/ui_registry_keys.hpp"
#include "lua/lua_state.hpp"
#include "map/terrain.hpp"
#include "renderer/cartographic_math.hpp"
#include "renderer/cartographic_technique.hpp"
#include "renderer/renderer.hpp"
#include "sim/entity.hpp"
#include "sim/sim_state.hpp"
#include "ui/ui_control.hpp"
#include "ui/world_view.hpp"

#include <fmt/format.h>
#include <lua.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <string>
#include <unordered_map>

namespace osc::test {

namespace {

/// The test's minimap view, its rect off the pixel grid as retail's is.
constexpr f32 kViewX = 40.4f;
constexpr f32 kViewY = 30.6f;
constexpr f32 kViewW = 241.3f;
constexpr f32 kViewH = 180.8f;

/// A minimap view made now (taking cam_DefaultMiniLOD as it is), the last
/// one destroyed, as retail's minimap.lua makes one.
std::string make_minimap() {
    return fmt::format(
        "do local old = rawget(_G, '__osc_mm_view')\n"
        "if old then old:Destroy() end\n"
        "local v = {{}}\n"
        "setmetatable(v, {{__index = moho.UIWorldView}})\n"
        "moho.UIWorldView.__init(v, GetFrame(0), 'MiniMap', 2, true, 'WorldCamera')\n"
        "v.Depth:Set(function() return 50 end)\n"
        "moho.UIWorldView.SetCartographic(v, true)\n"
        "v.Left:Set(function() return {} end)\n"
        "v.Top:Set(function() return {} end)\n"
        "v.Width:Set(function() return {} end)\n"
        "v.Height:Set(function() return {} end)\n"
        "rawset(_G, '__osc_mm_view', v)\n"
        "end\n",
        kViewX, kViewY, kViewW, kViewH);
}

/// How far a blueprint's mesh shows, read from the UI state's blueprints
/// as Moho's RMeshBlueprint has it: its mesh blueprint (a unit's or prop's
/// Display.MeshBlueprint, or itself), the largest LODCutoff of its LODs,
/// 1000 where one gives none, for ever if one is 0 or less; -1 without one.
f32 reach_of(lua_State* L, const std::string& id) {
    std::string lower = id;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    lua_pushstring(L, "__blueprints");
    lua_rawget(L, LUA_GLOBALSINDEX);
    const int bps = lua_gettop(L);
    f32 reach = -1.0f;
    lua_pushstring(L, lower.c_str());
    lua_rawget(L, bps);
    if (lua_istable(L, -1)) {
        // A unit's or prop's: its Display.MeshBlueprint
        lua_pushstring(L, "Display");
        lua_rawget(L, -2);
        if (lua_istable(L, -1)) {
            lua_pushstring(L, "MeshBlueprint");
            lua_rawget(L, -2);
            if (lua_type(L, -1) == LUA_TSTRING) {
                std::string mesh = lua_tostring(L, -1);
                std::transform(mesh.begin(), mesh.end(), mesh.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                lua_pop(L, 3);
                lua_pushstring(L, mesh.c_str());
                lua_rawget(L, bps);
            } else {
                lua_pop(L, 2); // leaves the blueprint: a mesh blueprint itself
            }
        } else {
            lua_pop(L, 1);
        }
    }
    if (lua_istable(L, -1)) {
        lua_pushstring(L, "LODs");
        lua_rawget(L, -2);
        if (lua_istable(L, -1)) {
            for (int i = 1;; ++i) {
                lua_rawgeti(L, -1, i);
                if (!lua_istable(L, -1)) {
                    lua_pop(L, 1);
                    break;
                }
                lua_pushstring(L, "LODCutoff");
                lua_rawget(L, -2);
                const f32 cutoff =
                    lua_isnumber(L, -1) ? static_cast<f32>(lua_tonumber(L, -1)) : 1000.0f;
                lua_pop(L, 2);
                if (cutoff <= 0.0f) {
                    reach = std::numeric_limits<f32>::infinity();
                    break;
                }
                reach = std::max(reach, cutoff);
            }
        }
        lua_pop(L, 1);
    }
    lua_settop(L, bps - 1);
    return reach;
}

} // namespace

void test_minimap_meshes(TestContext& ctx) {
    spdlog::info("=== Minimap mesh test ===");
    Tally t;
    const map::Terrain* terrain = ctx.sim.terrain();
    lua_State* L = ctx.L;
    lua_pushstring(L, "osc_ui_registry");
    lua_rawget(L, LUA_REGISTRYINDEX);
    auto* registry = static_cast<ui::UIControlRegistry*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    if (!terrain || !registry) {
        t.check(false, "a map is loaded and the UI's controls are up");
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
    const auto map_w = static_cast<f32>(terrain->map_width());
    const auto map_h = static_cast<f32>(terrain->map_height());
    const auto frame = renderer::cartographic_minimap(kViewX, kViewY, kViewW, kViewH, map_w, map_h);
    if (!frame) {
        t.check(false, "the view has a frame");
        return;
    }
    const f32 width = frame->frame.x1 - frame->frame.x0;
    lua_pushstring(L, core::kWorldUiActiveKey);
    lua_pushboolean(L, 1);
    lua_rawset(L, LUA_REGISTRYINDEX);

    // The commanders' warp-in first plays out (their bones hold them at no
    // size, then they wear the phase shield's mesh), and the renderer sees
    // the world as it then is
    for (int i = 0; i < 250; ++i) ctx.sim.tick();
    shots.recapture();

    // Each world entity's mesh and how far it shows, read apart from the
    // renderer (the fog is off: the player sees them all)
    struct Shown {
        u32 id = 0;
        std::string bp;
        f32 reach = -1.0f;
        sim::Vector3 at;
    };
    std::vector<Shown> world;
    std::unordered_map<std::string, f32> reaches;
    ctx.sim.entity_registry().for_each([&](const sim::Entity& e) {
        if (e.destroyed()) return;
        const std::string& id = e.mesh_override().empty() ? e.blueprint_id() : e.mesh_override();
        if (id.empty()) return;
        auto it = reaches.find(id);
        if (it == reaches.end()) it = reaches.emplace(id, reach_of(L, id)).first;
        if (it->second < 0.0f) return; // no mesh blueprint
        world.push_back({e.entity_id(), id, it->second, e.position()});
    });
    const auto expected_at = [&](f32 metric) {
        return static_cast<u32>(std::count_if(world.begin(), world.end(),
                                              [&](const Shown& s) { return s.reach >= metric; }));
    };

    // The minimap at a scale, the world drawn behind it
    shots.set_ui(nullptr);
    (void)shots.shoot_frame(*terrain, map_w * 0.5f, map_h * 0.5f, 200.0f);
    shots.set_ui(registry);
    const auto at_scale = [&](f32 scale) {
        ui::WorldView::set_default_mini_lod(scale);
        const auto made = ctx.lua_state.do_string(make_minimap());
        t.check(static_cast<bool>(made),
                made ? fmt::format("a minimap view is made at cam_DefaultMiniLOD {}", scale)
                     : fmt::format("the minimap view: {}", made.error().message));
        (void)shots.grab();
        return shots.grab();
    };

    // Retail's 1.8: on SCMP_009 (20 km) the metric passes every mesh
    (void)at_scale(ui::WorldView::kDefaultMiniLod);
    const f32 retail_metric = width * ui::WorldView::kDefaultMiniLod;
    t.check(std::abs(r.minimap_lod_metric() - retail_metric) < 0.01f,
            fmt::format("at 1.8 the metric is the view's width {:.1f} times 1.8: {:.1f}", width,
                        r.minimap_lod_metric()));
    t.check(r.minimap_mesh_instances() == 0 && expected_at(retail_metric) == 0,
            fmt::format("and no mesh reaches it: {} drawn ({} expected)",
                        r.minimap_mesh_instances(), expected_at(retail_metric)));

    // A view made at a scale that puts the metric at 160: every commander
    // (175, a Cybran's 215) and whatever else reaches it
    const f32 metric_160 = 160.0f;
    (void)at_scale(metric_160 / width);
    const u32 drawn_160 = r.minimap_mesh_instances();
    t.check(std::abs(r.minimap_lod_metric() - metric_160) < 0.01f && r.minimap_meshes_gathered(),
            fmt::format("made at {:.4f} its metric is {:.1f}", metric_160 / width,
                        r.minimap_lod_metric()));
    t.check(drawn_160 > 0 && drawn_160 == expected_at(metric_160),
            fmt::format("the meshes reaching 160 are drawn: {} of {} ({} entities with a mesh)",
                        drawn_160, expected_at(metric_160), world.size()));
    t.check(r.minimap_mesh_draws(renderer::CartographicTechnique::Unit) > 0,
            fmt::format("in their cartographic technique (CartographicUnit passes: {})",
                        r.minimap_mesh_draws(renderer::CartographicTechnique::Unit)));
    // Into the minimap's output, after Stage 1: each commander's pixels
    // change where its mesh draws (the capture at a pixel a unit)
    std::vector<u32> commanders;
    for (i32 army = 0; army < 8; ++army) {
        if (const u32 id = army_acu_id(ctx.sim, army)) commanders.push_back(id);
    }
    const renderer::CartographicRenderer::View big{1024, 1024, 0.0f, 0.0f, map_w, map_h};
    const auto with = r.capture_cartographic(*terrain, big, nullptr, true);
    const auto without = r.capture_cartographic(*terrain, big, nullptr, false);
    u32 marked = 0;
    if (with && without) {
        for (const u32 id : commanders) {
            const auto* e = ctx.sim.entity_registry().find(id);
            if (!e) continue;
            const auto px = static_cast<i32>(e->position().x / map_w * 1024.0f);
            const auto pz = static_cast<i32>(e->position().z / map_h * 1024.0f);
            u32 changed = 0;
            for (i32 y = std::max(pz - 8, 0); y < std::min(pz + 9, 1024); ++y)
                for (i32 x = std::max(px - 8, 0); x < std::min(px + 9, 1024); ++x) {
                    const size_t i = (static_cast<size_t>(y) * 1024 + static_cast<size_t>(x)) * 4;
                    for (int c = 0; c < 3; ++c)
                        if (with->output[i + c] != without->output[i + c]) {
                            ++changed;
                            break;
                        }
                }
            if (changed > 0) ++marked;
        }
    }
    t.check(!commanders.empty() && marked == commanders.size(),
            fmt::format("drawn into the output: {} of {} commanders change its pixels", marked,
                        commanders.size()));

    // At 190, the commanders reaching only 175 drop out
    (void)at_scale(190.0f / width);
    t.check(r.minimap_mesh_instances() == expected_at(190.0f) && expected_at(190.0f) < drawn_160,
            fmt::format("made for 190: {} drawn ({} expected, fewer than at 160)",
                        r.minimap_mesh_instances(), expected_at(190.0f)));

    // The convar set after the view is made leaves its scale (retail's
    // game); FAF's patched game draws none while it is 0
    const u32 drawn_190 = r.minimap_mesh_instances();
    ui::WorldView::set_default_mini_lod(0.0f);
    (void)shots.grab();
    t.check(r.minimap_meshes_gathered() && std::abs(r.minimap_lod_metric() - 190.0f) < 0.01f &&
                r.minimap_mesh_instances() == drawn_190,
            "retail's game: cam_DefaultMiniLOD 0 later leaves the view's scale");
    r.set_faf_exe(true);
    (void)shots.grab();
    t.check(!r.minimap_meshes_gathered() && r.minimap_mesh_instances() == 0,
            "FAF's patched game: cam_DefaultMiniLOD 0 draws no minimap meshes");
    ui::WorldView::set_default_mini_lod(ui::WorldView::kDefaultMiniLod);
    (void)shots.grab();
    t.check(r.minimap_meshes_gathered() && r.minimap_mesh_instances() == drawn_190,
            "and 1.8 again draws them, at the view's own scale");
    r.set_faf_exe(false);

    (void)ctx.lua_state.do_string(
        "do local v = rawget(_G, '__osc_mm_view') if v then v:Destroy() end "
        "rawset(_G, '__osc_mm_view', nil) end\n");
    ui::WorldView::set_default_mini_lod(ui::WorldView::kDefaultMiniLod);
    lua_pushstring(L, core::kWorldUiActiveKey);
    lua_pushnil(L);
    lua_rawset(L, LUA_REGISTRYINDEX);

    const u32 errors = renderer::Renderer::validation_error_count() - errors_before;
    t.check(errors == 0, fmt::format("no Vulkan validation errors ({})", errors));
    spdlog::info("Minimap meshes: {} passed, {} failed", t.pass, t.fail);
}

} // namespace osc::test
