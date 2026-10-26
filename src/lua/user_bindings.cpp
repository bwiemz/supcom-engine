// The UI state's bindings that reach the game's view and input: the camera,
// the world view's projection, selection, the mouse and keys, map previews
// and the UI's own orders (M191). They live apart from the Lua library, which
// the sim uses, so that library needs no renderer: osc_lua_user links both.

#include "blueprints/blueprint_store.hpp"
#include "core/color.hpp"
#include "core/localization.hpp"
#include "platform/executable.hpp"
#include "lua/user_bindings.hpp"
#include "lua/arg_count.hpp"

#include "lua/engine_bindings.hpp"
#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "lua/moho_bindings_internal.hpp"
#include "lua/order_helpers.hpp"
#include "map/scmap_parser.hpp"
#include "map/terrain.hpp"
#include "renderer/command_graph_renderer.hpp"
#include "renderer/input_handler.hpp"
#include "renderer/range_overlays.hpp"
#include "renderer/frustum.hpp"
#include "renderer/renderer.hpp"
#include "renderer/terrain_preview.hpp"
#include "renderer/world_shapes.hpp"
#include "sim/entity_registry.hpp"
#include "sim/category_expr.hpp"
#include "sim/lua_bytes.hpp"
#include "sim/sim_callback_queue.hpp"
#include "sim/script_class.hpp"
#include "sim/sim_state.hpp"
#include "sim/thread_manager.hpp"
#include "sim/unit.hpp"
#include "sim/unit_command.hpp"
#include "sim/waitable.hpp"
#include "ui/console.hpp"
#include "ui/key_codes.hpp"
#include "ui/ui_control.hpp"
#include "ui/world_view.hpp"
#include "ui/wld_ui_provider.hpp"
#include "vfs/virtual_file_system.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_set>
#include <optional>
#include <cctype>
#include <charconv>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace osc::lua {

namespace {
// Headless (no renderer) the camera methods answer with the renderer
// camera's defaults, so UI scripts run the same in tests.
// A 1024 map's farthest zoom at 4:3 (the renderer camera's defaults)
constexpr f32 kHeadlessMaxZoom = 1024.0f * 1.4f * (1024.0f / 768.0f);

/// Add methods to a moho class table (moho[cls]) that register_moho_bindings
/// made. Missing, it is a setup error: the UI's classes would lack them.
void add_class_methods(lua_State* L, const char* cls,
                       std::initializer_list<std::pair<const char*, lua_CFunction>> methods) {
    lua_pushstring(L, "moho");
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (lua_istable(L, -1)) {
        lua_pushstring(L, cls);
        lua_rawget(L, -2);
        if (lua_istable(L, -1)) {
            for (const auto& [name, func] : methods) {
                lua_pushstring(L, name);
                lua_pushcfunction(L, func);
                lua_rawset(L, -3);
            }
            lua_pop(L, 2);
            return;
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    spdlog::error("register_user_bindings: moho.{} is missing (register_moho_bindings first)", cls);
}

int l_UISelectionByCategory(lua_State* L); // with the console's UI_SelectByCategory, below

} // namespace

static renderer::Renderer* get_renderer(lua_State* L) {
    lua_pushstring(L, "__osc_renderer");
    lua_rawget(L, LUA_REGISTRYINDEX);
    auto* r = static_cast<renderer::Renderer*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return r;
}

static int mappreview_SetTextureFromMap(lua_State* L) {
    auto* ctrl = check_control(L);
    if (!ctrl) return 0;
    const char* scmap_path = luaL_checkstring(L, 2);

    auto* vfs = lua::LuaState::get_vfs(L);
    if (!vfs) return 0;

    auto file_data = vfs->read_file(scmap_path);
    if (!file_data || file_data->empty()) {
        spdlog::warn("MapPreview: SCMAP not found '{}'", scmap_path);
        return 0;
    }

    // Parse SCMAP to extract preview DDS + heightmap
    auto parse_result =
        osc::map::parse_scmap(std::vector<u8>(file_data->begin(), file_data->end()));
    if (!parse_result) {
        spdlog::warn("MapPreview: SCMAP parse failed: {}", parse_result.error().message);
        return 0;
    }
    auto& scmap = parse_result.value();

    std::string tex_key = std::string("__osc_mappreview_") + scmap_path;

    auto* r = get_renderer(L);

    // Try embedded preview DDS first
    if (!scmap.preview_dds.empty() && r) {
        auto* gpu_tex = r->texture_cache().get_raw(tex_key, scmap.preview_dds);
        if (gpu_tex) {
            ctrl->set_texture_path(tex_key);
            ctrl->set_has_solid_color(false);
            spdlog::info("MapPreview: loaded SCMAP preview DDS for '{}'", scmap_path);
            return 0;
        }
    }

    // Fallback: generate from heightmap
    if (!scmap.heightmap.empty() && r) {
        auto pixels = ::osc::renderer::generate_terrain_preview(
            scmap.heightmap.data(), scmap.map_width, scmap.map_height, scmap.height_scale,
            scmap.water_elevation, scmap.has_water, 512);

        auto* gpu_tex = r->texture_cache().upload_rgba(tex_key, pixels.data(), 512, 512);
        if (gpu_tex) {
            ctrl->set_texture_path(tex_key);
            ctrl->set_has_solid_color(false);
            spdlog::info("MapPreview: generated heightmap preview for '{}'", scmap_path);
        }
    }

    return 0;
}

/// InternalCreateWldUIProvider(self) — real WldUIProvider backing
static int l_InternalCreateWldUIProvider(lua_State* L) {
    if (!lua_istable(L, 1))
        return luaL_error(L, "InternalCreateWldUIProvider: arg 1 must be self table");

    // Retrieve the long-lived WldUIProvider stored in registry by main.cpp
    lua_pushstring(L, "__osc_wld_ui_provider");
    lua_rawget(L, LUA_REGISTRYINDEX);
    auto* provider = static_cast<ui::WldUIProvider*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    if (!provider) return luaL_error(L, "WldUIProvider not initialized");

    // Store as _c_object lightuserdata
    lua_pushstring(L, "_c_object");
    lua_pushlightuserdata(L, provider);
    lua_rawset(L, 1);

    // The engine drives this object (StartLoadingDialog, CreateGameInterface,
    // ...), and gamemain overrides those per instance, so keep the object
    // itself -- the newest provider replaces an older one.
    lua_pushstring(L, ui::WldUIProvider::kLuaObjectKey);
    lua_pushvalue(L, 1);
    lua_rawset(L, LUA_REGISTRYINDEX);

    // Don't set metatable — FA's ClassUI system handles metatables via __index chain.
    // Setting it here would overwrite the class hierarchy and break Lua-side overrides.

    spdlog::info("InternalCreateWldUIProvider: created real provider");
    return 0;
}

constexpr const char* kViewCamerasKey = "__osc_view_cameras";

static void size_world_view(lua_State* L, ui::WorldView* wv) {
    if (auto* r = get_renderer(L)) {
        const renderer::SceneView v = r->ui_view_of(wv->camera());
        wv->set_viewport(static_cast<u32>(std::lround(v.w)), static_cast<u32>(std::lround(v.h)));
    }
}

/// __init(self, parentControl, cameraName, depth, isMiniMap, trackCamera)
/// Bind a WorldView to the camera of its name (CUIWorldView's constructor) and the terrain, at
/// construction: retail's WorldView class overrides Register in Lua without calling the engine's.
static void bind_world_view(lua_State* L, ui::WorldView* wv, const std::string& name) {
    lua_pushstring(L, kViewCamerasKey);
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushstring(L, kViewCamerasKey);
        lua_pushvalue(L, -2);
        lua_rawset(L, LUA_REGISTRYINDEX);
    }
    lua_pushstring(L, name.c_str());
    lua_pushboolean(L, 1);
    lua_rawset(L, -3);
    lua_pop(L, 1);
    auto* r = get_renderer(L);
    if (r) {
        wv->set_renderer(r);
        renderer::Camera& own = r->add_camera(name);
        wv->set_own_camera(name, &own);
        if (wv->is_minimap()) {
            wv->register_camera("WorldCamera", &r->camera());
        } else {
            wv->register_camera(name, &own);
        }
        size_world_view(L, wv);
    } else {
        wv->register_camera(name, nullptr);
        wv->set_own_camera(name, nullptr);
    }
    auto* sim = get_sim(L);
    if (sim && sim->terrain()) wv->set_terrain(sim->terrain());
    if (!wv->is_minimap()) {
        // A new view's camera looks in perspective (CameraImpl's mIsOrtho
        // starts 0), its Register then restoring a saved cartographic mode;
        // one bound late keeps the mode it was given meanwhile
        if (wv->camera()) {
            wv->camera()->set_ortho(wv->is_cartographic());
        }
        if (name == renderer::CameraSet::kWorldCamera) {
            lua_pushstring(L, "__osc_world_view");
            lua_pushlightuserdata(L, wv);
            lua_rawset(L, LUA_REGISTRYINDEX);
        }
    }
    spdlog::debug("WorldView bound: camera='{}', viewport={}x{}", wv->camera_name(),
                  wv->viewport_width(), wv->viewport_height());
}

void bind_unbound_world_views(lua_State* L) {
    auto* reg = get_ui_registry(L);
    if (!reg || !get_renderer(L)) return;
    for (const auto& control : reg->all()) {
        auto* wv = dynamic_cast<ui::WorldView*>(control.get());
        if (wv && !wv->destroyed() && !wv->camera()) {
            const std::string name = wv->camera_name();
            bind_world_view(L, wv, name);
        }
    }
}

static int worldview_init(lua_State* L) {
    auto* reg = get_ui_registry(L);
    if (!reg) return luaL_error(L, "UIWorldView.__init: no UIControlRegistry");
    if (!lua_istable(L, 1)) return luaL_error(L, "UIWorldView.__init: self must be table");

    // Create a WorldView subclass instead of a generic UIControl
    auto wv = std::make_unique<ui::WorldView>();
    auto* wv_ptr = wv.get();
    u32 id = reg->add(std::move(wv));

    lua_pushvalue(L, 1);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    wv_ptr->set_lua_table_ref(ref);

    lua_pushstring(L, "_c_object");
    lua_pushlightuserdata(L, wv_ptr);
    lua_rawset(L, 1);

    // Set parent if provided
    if (lua_istable(L, 2)) {
        auto* parent = check_control(L, 2);
        if (parent) {
            attach_to_parent(L, *wv_ptr, *parent); // set_parent calls add_child
        }
    }

    // Store camera name and check minimap flag
    std::string cam_name = "WorldCamera";
    if (lua_type(L, 3) == LUA_TSTRING) {
        cam_name = lua_tostring(L, 3);
        wv_ptr->set_name(std::string("WorldView_") + cam_name);
    }
    if (lua_isboolean(L, 5) && lua_toboolean(L, 5)) {
        wv_ptr->set_minimap(true);
    }

    // Create 7 layout LazyVars
    create_lazyvar(L, 1, "Left");
    create_lazyvar(L, 1, "Top");
    create_lazyvar(L, 1, "Right");
    create_lazyvar(L, 1, "Bottom");
    create_lazyvar(L, 1, "Width");
    create_lazyvar(L, 1, "Height");
    create_lazyvar(L, 1, "Depth");

    call_on_init(L, 1, "UIWorldView.__init");

    bind_world_view(L, wv_ptr, cam_name);
    spdlog::debug("UIWorldView.__init: WorldView control #{}, camera='{}'", id, cam_name);
    return 0;
}

static bool read_vec3(lua_State* L, int idx, f32 out[3]);

static int worldview_Project(lua_State* L) {
    auto* wv = check_world_view(L);
    if (!wv || !wv->camera()) {
        push_vector2(L, 0, 0);
        return 1;
    }

    size_world_view(L, wv);

    f32 at[3] = {};
    (void)read_vec3(L, 2, at);
    f32 sx = 0, sy = 0;
    wv->project(at[0], at[1], at[2], sx, sy);
    push_vector2(L, sx, sy);
    return 1;
}

static int worldview_ProjectMultiple(lua_State* L) {
    // Return empty table for now (rarely used)
    lua_newtable(L);
    return 1;
}

/// worldview:HitTest(x, y) — always returns true (the world view covers its area)
/// worldview:Register(cameraName, terrain, ...) — associate with renderer camera/terrain
static int worldview_Register(lua_State* L) {
    auto* wv = check_world_view(L);
    if (wv) {
        // Without a name, its own camera's: a minimap's camera_name() is the
        // WorldCamera it tracks
        bind_world_view(L, wv,
                        lua_type(L, 2) == LUA_TSTRING ? lua_tostring(L, 2) : wv->own_camera_name());
    }
    return 0;
}

/// A vector's three numbers ({x, y, z}, as SCR_ToLua writes one).
static bool read_vec3(lua_State* L, int idx, f32 out[3]) {
    if (!lua_istable(L, idx)) return false;
    bool ok = true;
    for (int i = 0; i < 3; ++i) {
        lua_rawgeti(L, idx, i + 1);
        ok = ok && lua_isnumber(L, -1);
        out[i] = static_cast<f32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
    }
    return ok;
}

// --- FAF's world drawing (FA-Binary-Patches DrawFunc.cpp) ---
// UI_DrawLine, UI_DrawCircle and UI_DrawRect draw flat in the world, over it,
// from a world view's OnRenderWorld (the renderer collects the shapes then);
// called at any other time they raise an error, as FAF's do.

static renderer::WorldShapes* drawing_shapes(lua_State* L, const char* what) {
    lua_pushstring(L, renderer::kWorldShapesKey);
    lua_rawget(L, LUA_REGISTRYINDEX);
    auto* shapes = static_cast<renderer::WorldShapes*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    if (!shapes) luaL_error(L, "Attempt to call %s outside of OnRenderWorld", what);
    return shapes;
}

static sim::Vector3 shape_vector(lua_State* L, int idx) {
    f32 v[3] = {0, 0, 0};
    if (!read_vec3(L, idx, v)) luaL_argerror(L, idx, "vector expected");
    return {v[0], v[1], v[2]};
}

/// A shape's colour, as SCR_DecodeColor reads it
static u32 shape_color(lua_State* L, int idx) {
    const char* text = luaL_checkstring(L, idx);
    const std::optional<u32> color = decode_color(text);
    if (!color) luaL_error(L, "Unknown color: %s", text);
    return color.value_or(0u);
}

static f32 shape_thickness(lua_State* L, int idx) {
    return static_cast<f32>(luaL_optnumber(L, idx, renderer::WorldShapes::kDefaultThickness));
}

// UI_DrawLine(pos1, pos2, color, thickness = 0.15)
static int l_UI_DrawLine(lua_State* L) {
    auto* shapes = drawing_shapes(L, "DrawLine");
    const sim::Vector3 a = shape_vector(L, 1);
    const sim::Vector3 b = shape_vector(L, 2);
    const u32 color = shape_color(L, 3);
    shapes->line(a, b, color, shape_thickness(L, 4));
    return 0;
}

// UI_DrawCircle(pos, radius, color, thickness = 0.15)
static int l_UI_DrawCircle(lua_State* L) {
    auto* shapes = drawing_shapes(L, "DrawCircle");
    const sim::Vector3 at = shape_vector(L, 1);
    const auto radius = static_cast<f32>(luaL_checknumber(L, 2));
    const u32 color = shape_color(L, 3);
    shapes->circle(at, radius, color, shape_thickness(L, 4));
    return 0;
}

// UI_DrawRect(pos, size, color, thickness = 0.15)
static int l_UI_DrawRect(lua_State* L) {
    auto* shapes = drawing_shapes(L, "DrawRect");
    const sim::Vector3 at = shape_vector(L, 1);
    const auto size = static_cast<f32>(luaL_checknumber(L, 2));
    const u32 color = shape_color(L, 3);
    shapes->rect(at, size, color, shape_thickness(L, 4));
    return 0;
}

static void push_vec3(lua_State* L, f32 x, f32 y, f32 z) {
    lua_newtable(L);
    lua_pushnumber(L, x);
    lua_rawseti(L, -2, 1);
    lua_pushnumber(L, y);
    lua_rawseti(L, -2, 2);
    lua_pushnumber(L, z);
    lua_rawseti(L, -2, 3);
}

/// A named number of a table (nil or missing: `fallback`).
static f32 field_number(lua_State* L, int idx, const char* name, f32 fallback) {
    lua_pushstring(L, name);
    lua_rawget(L, idx);
    const f32 v = lua_isnumber(L, -1) ? static_cast<f32>(lua_tonumber(L, -1)) : fallback;
    lua_pop(L, 1);
    return v;
}

static renderer::Camera* check_camera(lua_State* L) {
    auto* r = get_renderer(L);
    if (!lua_istable(L, 1) || !r) {
        return nullptr;
    }
    lua_pushstring(L, "_c_object");
    lua_rawget(L, 1);
    const void* tagged = lua_touserdata(L, -1);
    lua_pop(L, 1);
    // A script can write the table: the pointer is a tag to check, not one to trust
    return r->cameras().find_tag(tagged);
}

/// camera:SaveSettings() -> {Focus, Zoom, Pitch, Heading} (Moho's: the
/// focus, the target zoom, the pitch and heading)
static int camera_SaveSettings(lua_State* L) {
    auto* cam = check_camera(L);
    lua_newtable(L);
    if (!cam) {
        return 1;
    }
    const auto& c = *cam;
    lua_pushstring(L, "Focus");
    push_vec3(L, c.focus_x(), c.focus_y(), c.focus_z());
    lua_rawset(L, -3);
    lua_pushstring(L, "Zoom");
    lua_pushnumber(L, c.zoom());
    lua_rawset(L, -3);
    lua_pushstring(L, "Pitch");
    lua_pushnumber(L, c.pitch());
    lua_rawset(L, -3);
    lua_pushstring(L, "Heading");
    lua_pushnumber(L, c.heading());
    lua_rawset(L, -3);
    return 1;
}

/// camera:RestoreSettings(settings): TargetManual to them at once, then the
/// rotation reverts (the pitch glides back to the zoom's)
static int camera_RestoreSettings(lua_State* L) {
    if (!lua_istable(L, 2)) return 0;
    auto* cam = check_camera(L);
    if (!cam) {
        return 0;
    }
    auto& c = *cam;
    f32 focus[3] = {c.focus_x(), c.focus_y(), c.focus_z()};
    lua_pushstring(L, "Focus");
    lua_rawget(L, 2);
    (void)read_vec3(L, lua_gettop(L), focus);
    lua_pop(L, 1);
    c.target_manual(focus[0], focus[1], focus[2], field_number(L, 2, "Heading", c.heading()),
                    field_number(L, 2, "Pitch", c.pitch()), field_number(L, 2, "Zoom", c.zoom()));
    c.revert_rotation();
    return 0;
}

/// camera:SetZoom(zoom, seconds): TargetManual at the target, heading and
/// pitch, over the seconds
static int camera_SetZoom(lua_State* L) {
    auto* cam = check_camera(L);
    if (!cam) {
        return 0;
    }
    auto& c = *cam;
    c.target_manual(c.target_x(), c.target_y(), c.target_z(), c.heading(), c.pitch(),
                    static_cast<f32>(luaL_checknumber(L, 2)), static_cast<f32>(lua_tonumber(L, 3)));
    return 0;
}

/// camera:GetZoom(): the target zoom (CameraGetTargetZoom)
static int camera_GetZoom(lua_State* L) {
    auto* cam = check_camera(L);
    lua_pushnumber(L, cam ? cam->zoom() : kHeadlessMaxZoom);
    return 1;
}

/// camera:SetMaxZoomMult(mult) -- scales the far zoom limit.
static int camera_SetMaxZoomMult(lua_State* L) {
    auto* cam = check_camera(L);
    if (cam) {
        cam->set_max_zoom_mult(static_cast<f32>(luaL_checknumber(L, 2)));
    }
    return 0;
}

/// camera:GetMinZoom(): cam_NearZoom
static int camera_GetMinZoom(lua_State* L) {
    lua_pushnumber(L, renderer::Camera::kNearZoom);
    return 1;
}

static int camera_GetMaxZoom(lua_State* L) {
    auto* cam = check_camera(L);
    lua_pushnumber(L, cam ? cam->max_zoom() : kHeadlessMaxZoom);
    return 1;
}

/// camera:SetTargetZoom(zoom): the zoom the view glides toward (mNearZoom)
static int camera_SetTargetZoom(lua_State* L) {
    auto* cam = check_camera(L);
    if (cam) {
        cam->set_requested_zoom(static_cast<f32>(luaL_checknumber(L, 2)));
    }
    return 0;
}

/// camera:GetTargetZoom(): the zoom the view glides toward
static int camera_GetTargetZoom(lua_State* L) {
    auto* cam = check_camera(L);
    lua_pushnumber(L, cam ? cam->requested_zoom() : kHeadlessMaxZoom);
    return 1;
}

static int camera_Reset(lua_State* L) {
    auto* cam = check_camera(L);
    if (cam) {
        cam->reset();
    }
    return 0;
}

/// camera:MoveTo(position, orientationHPR, zoom, seconds) and
/// camera:SnapTo(position, orientationHPR, zoom): TargetManual, heading and
/// pitch from the orientation, over the seconds (SnapTo's none)
/// AddCommandFeedbackBlip(meshInfo, duration): an order's mark in the world
/// for `duration` seconds (faf-re cfunc_AddCommandFeedbackBlipL), as FA's
/// commandmode.lua puts one where each order goes: {Position, MeshName or
/// BlueprintID, TextureName, ShaderName, UniformScale}.
static int l_AddCommandFeedbackBlip(lua_State* L) {
    auto* r = get_renderer(L);
    if (!r || !lua_istable(L, 1)) return 0;
    const auto string_field = [L](const char* name) {
        lua_pushstring(L, name);
        lua_gettable(L, 1);
        std::string value = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "";
        lua_pop(L, 1);
        return value;
    };
    renderer::FeedbackBlipSpec spec;
    spec.mesh_name = string_field("MeshName");
    spec.blueprint_id = string_field("BlueprintID");
    spec.texture_name = string_field("TextureName");
    spec.shader_name = string_field("ShaderName");
    // Read unguarded, as Moho's is: none is 0.
    lua_pushstring(L, "UniformScale");
    lua_gettable(L, 1);
    spec.uniform_scale = static_cast<f32>(lua_tonumber(L, -1));
    lua_pop(L, 1);
    lua_pushstring(L, "Position");
    lua_gettable(L, 1);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return 0;
    }
    std::array<f32, 3> xyz{};
    for (int i = 0; i < 3; ++i) {
        lua_rawgeti(L, -1, i + 1);
        xyz[static_cast<size_t>(i)] = static_cast<f32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    spec.position = {xyz[0], xyz[1], xyz[2]};
    spec.duration = static_cast<f32>(lua_tonumber(L, 2));
    r->add_command_feedback_blip(std::move(spec));
    return 0;
}

/// SetInvertMidMouseButton(flag): the middle button drags the ground the
/// other way (FAF's options menu; Moho's UI_SetInvertMidMouseScrub).
static int l_SetInvertMidMouseButton(lua_State* L) {
    if (auto* r = get_renderer(L)) r->camera().set_invert_middle(lua_toboolean(L, 1) != 0);
    return 0;
}

static int camera_MoveTo(lua_State* L) {
    auto* cam = check_camera(L);
    if (!cam) {
        return 0;
    }
    auto& c = *cam;
    f32 pos[3] = {c.target_x(), c.focus_y(), c.target_z()};
    (void)read_vec3(L, 2, pos);
    f32 hpr[3] = {c.heading(), c.pitch(), 0.0f};
    (void)read_vec3(L, 3, hpr);
    const f32 zoom = lua_isnumber(L, 4) ? static_cast<f32>(lua_tonumber(L, 4)) : c.zoom();
    c.target_manual(pos[0], pos[1], pos[2], hpr[0], hpr[1], zoom,
                    static_cast<f32>(lua_tonumber(L, 5)));
    return 0;
}

/// camera:MoveToRegion(rect, [seconds]): TargetBox over the rect ({x0, y0,
/// x1, y1} or a Rect's fields, y being map z), its corners on their cells'
/// middles at the ground's height there (cfunc_CameraImplMoveToRegionL)
static int camera_MoveToRegion(lua_State* L) {
    auto* cam = check_camera(L);
    if (!cam || !lua_istable(L, 2)) {
        return 0;
    }
    f32 v[4] = {0, 0, 0, 0};
    static const char* const keys[4] = {"x0", "y0", "x1", "y1"};
    for (int i = 0; i < 4; ++i) {
        lua_pushstring(L, keys[i]);
        lua_rawget(L, 2);
        if (!lua_isnumber(L, -1)) {
            lua_pop(L, 1);
            lua_rawgeti(L, 2, i + 1);
        }
        v[i] = static_cast<f32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
    }
    const auto cell = [](f32 x) { return static_cast<f32>(static_cast<int>(x - 0.5f)) + 0.5f; };
    const f32 x0 = cell(v[0]);
    const f32 z0 = cell(v[1]);
    const f32 x1 = cell(v[2]);
    const f32 z1 = cell(v[3]);
    const auto height = [&](f32 x, f32 z) {
        auto* sim = get_sim(L);
        return sim && sim->terrain() ? sim->terrain()->get_terrain_height(x, z) : 0.0f;
    };
    cam->target_box({x0, height(x0, z0), z0}, {x1, height(x1, z1), z1},
                    static_cast<f32>(lua_tonumber(L, 3)));
    return 0;
}

/// A table of entity ids (strings, as retail passes them, or numbers), or a
/// single one.
static std::vector<u32> read_entity_ids(lua_State* L, int idx) {
    std::vector<u32> ids;
    const auto one = [&](int at) {
        if (lua_isnumber(L, at)) ids.push_back(sim::to_entity_id(L, at));
    };
    if (!lua_istable(L, idx)) {
        one(idx);
        return ids;
    }
    const int n = luaL_getn(L, idx);
    for (int i = 1; i <= n; ++i) {
        lua_rawgeti(L, idx, i);
        one(lua_gettop(L));
        lua_pop(L, 1);
    }
    return ids;
}

/// A camera's move as a UI thread waits on it.
struct CameraWait : sim::Waitable {
    bool done = false;
    bool is_done() const override { return done; }
    bool is_cancelled() const override { return false; }
};

/// WaitFor(camera) in a UI thread (the UI's WaitFor hook): parks the thread
/// until the camera's event is signalled, as Moho's CScriptEvent does when a
/// move ends; at once when none is under way. usercamera.lua waits so on the
/// sim's camera requests before calling it back.
static int ui_wait_camera(lua_State* L) {
    renderer::Camera* camera = check_camera(L);
    lua_pushstring(L, "__osc_ui_thread_manager");
    lua_rawget(L, LUA_REGISTRYINDEX);
    auto* threads = static_cast<sim::ThreadManager*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    if (!camera || !threads || camera->signaled()) {
        return 0;
    }
    // The thread manager keeps the waitable only while the thread waits;
    // the camera's waiter keeps it alive until the wake
    auto wait = std::make_shared<CameraWait>();
    camera->on_signal([wait, threads] {
        wait->done = true;
        threads->wake(*wait, 0); // only the thread that waited, if it still lives
    });
    lua_pushlightuserdata(L, static_cast<sim::Waitable*>(wait.get()));
    return lua_yield(L, 1);
}

/// camera:TargetEntities(ids, zoom, seconds): to the first, not followed
static int camera_TargetEntities(lua_State* L) {
    if (auto* cam = check_camera(L)) {
        cam->target_entities(read_entity_ids(L, 2), false, static_cast<f32>(lua_tonumber(L, 3)),
                             static_cast<f32>(lua_tonumber(L, 4)));
    }
    return 0;
}

/// camera:TrackEntities(ids, zoom, seconds): to the first, followed
static int camera_TrackEntities(lua_State* L) {
    if (auto* cam = check_camera(L)) {
        cam->target_entities(read_entity_ids(L, 2), true, static_cast<f32>(lua_tonumber(L, 3)),
                             static_cast<f32>(lua_tonumber(L, 4)));
    }
    return 0;
}

/// camera:NoseCam(id, pitchAdjust, zoom, seconds, transition)
static int camera_NoseCam(lua_State* L) {
    if (auto* cam = check_camera(L)) {
        cam->target_nose_cam(read_entity_ids(L, 2), static_cast<f32>(lua_tonumber(L, 3)),
                             static_cast<f32>(lua_tonumber(L, 4)),
                             static_cast<f32>(lua_tonumber(L, 5)),
                             static_cast<f32>(lua_tonumber(L, 6)));
    }
    return 0;
}

/// camera:Spin(headingRate, [zoomRate]): revolutions and zoom a second
static int camera_Spin(lua_State* L) {
    if (auto* cam = check_camera(L)) {
        cam->spin_rates(static_cast<f32>(lua_tonumber(L, 2)), static_cast<f32>(lua_tonumber(L, 3)));
    }
    return 0;
}

/// camera:SetAccMode(name): Linear, FastInSlowOut or SlowInOut
static int camera_SetAccMode(lua_State* L) {
    auto* cam = check_camera(L);
    if (cam && lua_type(L, 2) == LUA_TSTRING) {
        (void)cam->set_acc_mode(lua_tostring(L, 2));
    }
    return 0;
}

static int camera_EnableEaseInOut(lua_State* L) {
    if (auto* cam = check_camera(L)) {
        cam->set_ease_in_out(true);
    }
    return 0;
}

static int camera_DisableEaseInOut(lua_State* L) {
    if (auto* cam = check_camera(L)) {
        cam->set_ease_in_out(false);
    }
    return 0;
}

static int camera_HoldRotation(lua_State* L) {
    if (auto* cam = check_camera(L)) {
        cam->hold_rotation();
    }
    return 0;
}

static int camera_RevertRotation(lua_State* L) {
    if (auto* cam = check_camera(L)) {
        cam->revert_rotation();
    }
    return 0;
}

static int camera_UseGameClock(lua_State* L) {
    if (auto* cam = check_camera(L)) {
        cam->use_game_clock(true);
    }
    return 0;
}

static int camera_UseSystemClock(lua_State* L) {
    if (auto* cam = check_camera(L)) {
        cam->use_game_clock(false);
    }
    return 0;
}

/// camera:GetFocusPosition() -> the point the camera looks at (its focus,
/// on the ground or the water's surface)
static int camera_GetFocusPosition(lua_State* L) {
    auto* cam = check_camera(L);
    if (!cam) {
        push_vec3(L, 512.0f, 0.0f, 512.0f);
        return 1;
    }
    const auto& c = *cam;
    push_vec3(L, c.focus_x(), c.focus_y(), c.focus_z());
    return 1;
}

static int camera_GetHeading(lua_State* L) {
    auto* cam = check_camera(L);
    lua_pushnumber(L, cam ? cam->heading() : renderer::Camera::kPi);
    return 1;
}

static int camera_GetPitch(lua_State* L) {
    auto* cam = check_camera(L);
    lua_pushnumber(L, cam ? cam->pitch()
                          : renderer::Camera::kFarPitchDeg * renderer::Camera::kPi / 180.0f);
    return 1;
}

/// UISelectAndZoomTo(unit [, seconds]): select that unit alone and frame it
/// (TargetEntityBox): its box, 20 wider each way on x and z, over the
/// seconds; at once, the camera then follows nothing. The unit's box here is
/// its footprint (the engine has no mesh box on the UI's side).
static int l_SelectUnits(lua_State* L);
static int l_UISelectAndZoomTo(lua_State* L) {
    if (!lua_istable(L, 1)) return 0;
    const f32 seconds = static_cast<f32>(lua_tonumber(L, 2));
    auto* e = check_entity(L, 1);
    lua_newtable(L);
    lua_pushvalue(L, 1);
    lua_rawseti(L, -2, 1);
    lua_replace(L, 1); // arg 1 = {unit}
    lua_settop(L, 1);
    l_SelectUnits(L);
    auto* r = get_renderer(L);
    if (!r || !e || e->destroyed()) return 0;
    constexpr f32 kExpand = 20.0f; // cam_EntityBoxExpand
    const auto pos = e->position();
    const f32 hx = e->footprint_size_x() * 0.5f + kExpand;
    const f32 hz = e->footprint_size_z() * 0.5f + kExpand;
    r->camera().target_box({pos.x - hx, pos.y, pos.z - hz}, {pos.x + hx, pos.y, pos.z + hz},
                           seconds);
    if (seconds == 0.0f) r->camera().target_nothing();
    return 0;
}

/// UIZoomTo(units, [seconds]): frame the units (cfunc_UIZoomToL): their box
/// padded 20 each way, its height their mean height give or take half its
/// wider side; TargetBox over the seconds, then the camera follows nothing.
static int l_UIZoomTo(lua_State* L) {
    if (!lua_istable(L, 1)) return 0;
    auto* r = get_renderer(L);
    if (!r) return 0;
    constexpr f32 kMargin = 20.0f;
    f32 min_x = std::numeric_limits<f32>::infinity();
    f32 min_z = min_x;
    f32 max_x = -min_x;
    f32 max_z = -min_x;
    f32 sum_y = 0.0f;
    int count = 0;
    const int n = luaL_getn(L, 1);
    for (int i = 1; i <= n; ++i) {
        lua_rawgeti(L, 1, i);
        if (auto* e = check_entity(L, lua_gettop(L)); e && !e->destroyed()) {
            const auto pos = e->position();
            min_x = std::min(min_x, pos.x);
            max_x = std::max(max_x, pos.x);
            min_z = std::min(min_z, pos.z);
            max_z = std::max(max_z, pos.z);
            sum_y += pos.y;
            ++count;
        }
        lua_pop(L, 1); // array element
    }
    if (count == 0) return 0;
    const f32 mean_y = sum_y / static_cast<f32>(count);
    const f32 half = std::max(max_x - min_x, max_z - min_z) * 0.5f;
    r->camera().target_box({min_x - kMargin, mean_y - half - kMargin, min_z - kMargin},
                           {max_x + kMargin, mean_y + half + kMargin, max_z + kMargin},
                           static_cast<f32>(lua_tonumber(L, 2)));
    r->camera().target_nothing();
    return 0;
}

/// UnProject(worldview, Vector2(x, y)) -- the point on the ground under a
/// screen point of the view, as Project's inverse (retail's worldview.lua
/// moves a dragged ping there). NaNs where the view sees no ground there,
/// which that script checks for.
static int l_UnProject(lua_State* L) {
    auto* wv = check_world_view(L, 1);
    f32 sx = 0.0f, sy = 0.0f;
    if (lua_istable(L, 2)) {
        const auto component = [&](int index, const char* name) {
            lua_rawgeti(L, 2, index);
            if (!lua_isnumber(L, -1)) {
                lua_pop(L, 1);
                lua_pushstring(L, name);
                lua_gettable(L, 2); // x/y through a vector's metatable
            }
            const f32 v = lua_isnumber(L, -1) ? static_cast<f32>(lua_tonumber(L, -1)) : 0.0f;
            lua_pop(L, 1);
            return v;
        };
        sx = component(1, "x");
        sy = component(2, "y");
    }
    if (wv) {
        size_world_view(L, wv);
    }
    sim::Vector3 at{};
    if (!wv || !wv->camera() || !wv->get_mouse_world_pos(sx, sy, at.x, at.y, at.z)) {
        const f32 nan = std::numeric_limits<f32>::quiet_NaN();
        at = {nan, nan, nan};
    }
    push_vector3(L, at);
    return 1;
}

/// GetMouseScreenPos() -> Vector2: the cursor's position on the screen, in
/// the UI's pixels, as Moho's (faf-re cfunc_GetMouseScreenPosL: the
/// session's CursorScreenPos; "No session started." without one). FAF's
/// reticles, area attack and painting follow the cursor by it.
static int l_GetMouseScreenPos(lua_State* L) {
    if (lua_gettop(L) != 0)
        return arg_count_error(L, "vector GetMouseScreenPos()", 0, lua_gettop(L));
    if (!session_active(L)) return luaL_error(L, kNoSessionStarted);
    f64 x = 0, y = 0;
    if (auto* r = get_renderer(L)) r->ui_mouse_position(x, y);
    push_vector2(L, static_cast<f32>(x), static_cast<f32>(y));
    return 1;
}

/// GetAntiAliasingOptions() -> the antialiasing option keys the display
/// offers: Moho's lists 0 (off), then a packed key for each multisample
/// mode of the device (faf-re cfunc_GetAntiAliasingOptionsL). The engine
/// draws without multisampling, so off alone, as the options screen's
/// antialiasing states list (publish_fidelity_options); retail's and
/// FAF's fidelity presets pick their antialiasing from it.
static int l_GetAntiAliasingOptions(lua_State* L) {
    if (lua_gettop(L) != 0)
        return arg_count_error(L, "obj GetAntiAliasingOptions()", 0, lua_gettop(L));
    lua_newtable(L);
    lua_pushnumber(L, 0);
    lua_rawseti(L, -2, 1);
    return 1;
}

/// Where the cursor points on the ground (the session's CursorWorldPos):
/// 0, 0, 0 without a world view.
static sim::Vector3 cursor_world_pos(lua_State* L) {
    lua_pushstring(L, "__osc_world_view");
    lua_rawget(L, LUA_REGISTRYINDEX);
    auto* wv = static_cast<ui::WorldView*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    sim::Vector3 at{};
    if (!wv || !wv->camera()) return at;
    if (auto* r = get_renderer(L)) {
        f64 dx = 0, dy = 0;
        r->ui_mouse_position(dx, dy);
        renderer::pick_ground_in(r->ui_views(), static_cast<f32>(dx), static_cast<f32>(dy),
                                 wv->terrain(), at.x, at.y, at.z);
    }
    return at;
}

/// CreateUnitAtMouse(blueprintId, armyIndex, offsetX, offsetZ, rotation): a
/// cheat, as Moho's (faf-re cfunc_CreateUnitAtMouseL): a unit of the
/// blueprint for the 0-based army, at the cursor's place on the ground
/// moved by the offsets, facing `rotation`. It errors as Moho's does: five
/// arguments, "No active session.", "Unknown unit kind: <id>", the army in
/// range. The sim makes the unit (kCreateUnitCallback) through the command
/// stream, and only with cheats on.
static int l_CreateUnitAtMouse(lua_State* L) {
    if (lua_gettop(L) != 5) return arg_count_error(L, "CreateUnitAtMouse", 5, lua_gettop(L));
    if (!session_active(L)) return luaL_error(L, "No active session.");
    auto* sim = get_sim(L);
    if (lua_type(L, 1) != LUA_TSTRING) return luaL_typerror(L, 1, "string");
    const std::string id = lua_tostring(L, 1);
    const auto* store = LuaState::get_blueprint_store(L);
    const auto* entry = store ? store->find(id) : nullptr;
    if (!entry || entry->type != blueprints::BlueprintType::Unit)
        return luaL_error(L, "Unknown unit kind: %s", id.c_str());
    if (lua_type(L, 2) != LUA_TNUMBER) return luaL_typerror(L, 2, "integer");
    // (a NaN or a number past an int is no army: cast only one within range)
    const lua_Number army_arg = lua_tonumber(L, 2);
    const int army = army_arg > -1e9 && army_arg < 1e9 ? static_cast<int>(army_arg) : -1;
    const auto armies = static_cast<int>(sim->army_count());
    if (army < 0 || army >= armies)
        return luaL_error(L, "Invalid army index, must be >= 0 and < %d but got %d.", armies, army);
    for (const int arg : {4, 3, 5}) { // (Moho's order)
        if (lua_type(L, arg) != LUA_TNUMBER) return luaL_typerror(L, arg, "number");
    }
    const sim::Vector3 at = cursor_world_pos(L);
    if (auto* queue = get_callback_queue(L)) {
        sim::SimCallbackEntry entry_cb;
        entry_cb.func_name = sim::kCreateUnitCallback;
        entry_cb.args["Blueprint"] = entry->id;
        entry_cb.args["Army"] = static_cast<f64>(army);
        entry_cb.args["X"] = static_cast<f64>(at.x + static_cast<f32>(lua_tonumber(L, 3)));
        entry_cb.args["Z"] = static_cast<f64>(at.z + static_cast<f32>(lua_tonumber(L, 4)));
        entry_cb.args["Heading"] = static_cast<f64>(static_cast<f32>(lua_tonumber(L, 5)));
        queue->push(std::move(entry_cb));
    }
    return 0;
}

// --- GetMouseWorldPos global (M136a) ---
// FA calls this as a standalone function: GetMouseWorldPos(), in a session
// (faf-re cfunc_GetMouseWorldPosL reads the session's cursor).
static int l_GetMouseWorldPos(lua_State* L) {
    if (lua_gettop(L) != 0)
        return arg_count_error(L, "vector GetMouseWorldPos()", 0, lua_gettop(L));
    if (!session_active(L)) return luaL_error(L, kNoSessionStarted);
    const sim::Vector3 at = cursor_world_pos(L);
    const f32 wx = at.x;
    const f32 wy = at.y;
    const f32 wz = at.z;

    lua_newtable(L);
    lua_pushstring(L, "x");
    lua_pushnumber(L, static_cast<lua_Number>(wx));
    lua_rawset(L, -3);
    lua_pushstring(L, "y");
    lua_pushnumber(L, static_cast<lua_Number>(wy));
    lua_rawset(L, -3);
    lua_pushstring(L, "z");
    lua_pushnumber(L, static_cast<lua_Number>(wz));
    lua_rawset(L, -3);
    return 1;
}

// --- UserDecal (faf-re script/ScriptedDecal.cpp) ---

static bool read_vector(lua_State* L, int idx, int n, f32* out) {
    if (!lua_istable(L, idx)) {
        return false;
    }
    static const char* const kNames[] = {"x", "y", "z"};
    for (int i = 0; i < n; ++i) {
        lua_rawgeti(L, idx, i + 1);
        if (!lua_isnumber(L, -1)) {
            lua_pop(L, 1);
            lua_pushstring(L, kNames[i]);
            lua_gettable(L, idx);
        }
        const bool ok = lua_isnumber(L, -1) != 0;
        out[i] = static_cast<f32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        if (!ok) {
            return false;
        }
    }
    return true;
}

static ui::UserDecals* user_decal(lua_State* L, u32& id) {
    auto* reg = get_ui_registry(L);
    if (!reg || !lua_istable(L, 1)) {
        return nullptr;
    }
    lua_pushstring(L, "_c_user_decal");
    lua_rawget(L, 1);
    id = lua_isnumber(L, -1) ? static_cast<u32>(lua_tonumber(L, -1)) : 0;
    lua_pop(L, 1);
    return id ? &reg->user_decals() : nullptr;
}

static int l_c_CreateDecal(lua_State* L) {
    auto* reg = get_ui_registry(L);
    if (!reg || !lua_istable(L, 1)) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushstring(L, "_c_user_decal");
    lua_pushnumber(L, static_cast<lua_Number>(reg->user_decals().create()));
    lua_rawset(L, 1);
    lua_pushvalue(L, 1);
    return 1;
}

static int userdecal_SetTexture(lua_State* L) {
    u32 id = 0;
    ui::UserDecals* decals = user_decal(L, id);
    const char* path = luaL_checkstring(L, 2);
    if (decals) {
        decals->set_texture(id, path);
    }
    return 0;
}

static int userdecal_SetScale(lua_State* L) {
    u32 id = 0;
    ui::UserDecals* decals = user_decal(L, id);
    f32 v[3] = {};
    if (decals && read_vector(L, 2, 3, v)) {
        decals->set_scale(id, v[0], v[1], v[2]);
    }
    return 0;
}

static int userdecal_SetPosition(lua_State* L) {
    u32 id = 0;
    ui::UserDecals* decals = user_decal(L, id);
    f32 v[3] = {};
    if (decals && read_vector(L, 2, 3, v)) {
        decals->set_position(id, v[0], v[1], v[2]);
    }
    return 0;
}

static int userdecal_SetPositionByScreen(lua_State* L) {
    u32 id = 0;
    ui::UserDecals* decals = user_decal(L, id);
    f32 v[2] = {};
    if (!decals || !read_vector(L, 2, 2, v)) {
        return 0;
    }
    lua_pushstring(L, "__osc_world_view");
    lua_rawget(L, LUA_REGISTRYINDEX);
    auto* wv = static_cast<ui::WorldView*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    f32 x = 0;
    f32 y = 0;
    f32 z = 0;
    if (wv && wv->camera() && wv->get_mouse_world_pos(v[0], v[1], x, y, z)) {
        decals->set_position(id, x, y, z);
    }
    return 0;
}

static int userdecal_Destroy(lua_State* L) {
    u32 id = 0;
    if (ui::UserDecals* decals = user_decal(L, id)) {
        decals->destroy(id);
        lua_pushstring(L, "_c_user_decal");
        lua_pushnil(L);
        lua_rawset(L, 1);
    }
    return 0;
}

// --- SyncPlayableRect global (M209) ---
/// SyncPlayableRect(rect): the user side of the sim's playable rect (Moho's
/// CWldSession::SyncPlayableRect), which usercamera.lua applies from the
/// campaign's CAMERA_SYNC_PLAYABLE_RECT request. Kept to the map as the
/// sim's is, the camera keeps to it, and the meshes outside it now hide
/// until the next.
static int l_SyncPlayableRect(lua_State* L) {
    if (lua_gettop(L) != 1) return arg_count_error(L, "SyncPlayableRect(rect)", 1, lua_gettop(L));
    luaL_checktype(L, 1, LUA_TTABLE);
    // A Rect's fields, its y the map's z
    const auto field = [L](const char* name) {
        lua_pushstring(L, name);
        lua_gettable(L, 1);
        const auto v = static_cast<i32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        return v;
    };
    const renderer::PlayableRect rect{field("x0"), field("y0"), field("x1"), field("y1")};
    auto* sim = get_sim(L);
    auto* r = get_renderer(L);
    if (!sim || !sim->terrain() || !r) return 0;
    const auto kept =
        renderer::clamp_playable_rect(rect, static_cast<i32>(sim->terrain()->map_width()),
                                      static_cast<i32>(sim->terrain()->map_height()));
    if (!kept) {
        spdlog::warn("Attempting to set an invalid playable rect");
        return 0;
    }
    r->camera().set_playable_rect(static_cast<f32>(kept->x0), static_cast<f32>(kept->z0),
                                  static_cast<f32>(kept->x1), static_cast<f32>(kept->z1));
    r->playable_rect().sync(*kept);
    return 0;
}

/// RenderOverlayEconomy(on): the session's economy overlay flag (Moho's
/// DisplayEconomyOverlay), which the MFD's toggle sets and a NIS clears.
static int l_RenderOverlayEconomy(lua_State* L) {
    if (lua_gettop(L) != 1)
        return arg_count_error(L, "RenderOverlayEconomy(bool)", 1, lua_gettop(L));
    if (auto* r = get_renderer(L)) r->set_economy_overlay(lua_toboolean(L, 1) != 0);
    return 0;
}

/// Moho's cfunc_TeamColorModeL; the string of per-army colours is FAF's
/// (FA-Binary-Patches section/TeamColorMode.cpp).
static int l_TeamColorMode(lua_State* L) {
    if (lua_gettop(L) != 1) {
        return arg_count_error(L, "TeamColorMode(bool)", 1, lua_gettop(L));
    }
    auto* r = get_renderer(L);
    if (!r) {
        return 0;
    }
    if (lua_type(L, 1) == LUA_TSTRING) {
        std::vector<u32> palette;
        const std::string_view text(lua_tostring(L, 1), lua_strlen(L, 1));
        size_t start = 0;
        while (start < text.size()) {
            const size_t end = std::min(text.find(',', start), text.size());
            u32 color = 0;
            const auto digits = text.substr(start, end - start);
            std::from_chars(digits.data(), digits.data() + digits.size(), color, 16);
            palette.push_back(color);
            start = end + 1;
        }
        r->set_team_palette(std::move(palette));
        return 0;
    }
    if (!lua_isboolean(L, 1)) {
        return luaL_typerror(L, 1, "bool");
    }
    r->set_team_color_mode(lua_toboolean(L, 1) != 0);
    return 0;
}

/// The range overlays FA's UI sets up: the session's, or none.
static renderer::RangeOverlays* get_range_overlays(lua_State* L) {
    lua_pushstring(L, "__osc_range_overlays");
    lua_rawget(L, LUA_REGISTRYINDEX);
    auto* overlays = static_cast<renderer::RangeOverlays*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return overlays;
}

/// SetOverlayFilter(name, categories, normalColor, selectColor, rolloverColor,
/// inner0, inner1, outer0, outer1): one range overlay, kept by its name, which
/// picks what it rings (Moho's cfunc_SetOverlayFilterL). The colours are
/// AARRGGBB, the alpha their glow; each pair is a line's thickness zoomed in
/// and zoomed out.
static int l_SetOverlayFilter(lua_State* L) {
    if (lua_gettop(L) != 9) return arg_count_error(L, "SetOverlayFilter()", 9, lua_gettop(L));
    auto* overlays = get_range_overlays(L);
    if (!overlays) return 0;
    renderer::RangeProfile profile;
    profile.name = luaL_checkstring(L, 1);
    const auto number = [L](int i) { return static_cast<f32>(luaL_checknumber(L, i)); };
    profile.inner = {number(6), number(7)};
    profile.outer = {number(8), number(9)};
    profile.categories = sim::compile_category(L, 2);
    profile.rollover = renderer::range_color(check_color(L, 5));
    profile.selected = renderer::range_color(check_color(L, 4));
    profile.normal = renderer::range_color(check_color(L, 3));
    overlays->set_profile(std::move(profile));
    return 0;
}

/// SetOverlayFilters(names): the session's active range overlays, by name
/// (multifunction.lua's), drawn for every unit in view.
static int l_SetOverlayFilters(lua_State* L) {
    std::vector<std::string> names;
    if (lua_istable(L, 1)) {
        const int count = luaL_getn(L, 1);
        for (int i = 1; i <= count; ++i) {
            lua_rawgeti(L, 1, i);
            if (lua_type(L, -1) == LUA_TSTRING) names.emplace_back(lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    }
    if (auto* overlays = get_range_overlays(L)) overlays->set_filters(std::move(names));
    return 0;
}

/// GetCamera(name): RCamManager::GetCamera, nil if no world view made it
static int l_GetCamera(lua_State* L) {
    auto* r = get_renderer(L);
    renderer::Camera* camera = nullptr;
    if (r) {
        camera = lua_type(L, 1) == LUA_TSTRING ? r->cameras().find(lua_tostring(L, 1)) : nullptr;
        if (!camera && lua_type(L, 1) == LUA_TSTRING) {
            lua_pushstring(L, kViewCamerasKey);
            lua_rawget(L, LUA_REGISTRYINDEX);
            if (lua_istable(L, -1)) {
                lua_pushvalue(L, 1);
                lua_rawget(L, -2);
                if (lua_toboolean(L, -1)) {
                    camera = &r->add_camera(lua_tostring(L, 1));
                }
                lua_pop(L, 1);
            }
            lua_pop(L, 1);
        }
        if (!camera) {
            lua_pushnil(L);
            return 1;
        }
    }
    lua_newtable(L);
    if (camera) {
        lua_pushstring(L, "_c_object");
        lua_pushlightuserdata(L, camera);
        lua_rawset(L, -3);
    }

    // Set metatable to moho.camera_methods
    lua_pushstring(L, "moho");
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (lua_istable(L, -1)) {
        lua_pushstring(L, "camera_methods");
        lua_rawget(L, -2);
        if (lua_istable(L, -1)) {
            lua_newtable(L); // mt
            lua_pushstring(L, "__index");
            lua_pushvalue(L, -3);    // camera_methods
            lua_rawset(L, -3);       // mt.__index = camera_methods
            lua_setmetatable(L, -4); // setmetatable(cam_table, mt)
        }
        lua_pop(L, 1); // camera_methods
    }
    lua_pop(L, 1); // moho

    return 1;
}

static renderer::InputHandler* get_input_handler(lua_State* L) {
    lua_pushstring(L, "__osc_input_handler");
    lua_rawget(L, LUA_REGISTRYINDEX);
    auto* ih = static_cast<renderer::InputHandler*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return ih;
}

/// GetHighlightCommand(): the order whose command graph waypoint is under
/// the cursor (FAF's patch, Moho's highlightCommandId), as {commandType, x,
/// y, z, commandId, targetId, blueprintId}; nothing without one. Its place is
/// the waypoint's (a unit target's where the unit is; what Moho's command
/// holds for one isn't known). Only a structure's build has a waypoint and a
/// blueprint: a factory's build and an upgrade draw none.
static int l_GetHighlightCommand(lua_State* L) {
    const auto* input = get_input_handler(L);
    const u32 id = input ? input->hovered_command() : 0;
    if (id == 0) return 0;
    for (const renderer::CommandGraphNode& node : input->command_graph_nodes()) {
        const sim::CommandRecord& order = node.order;
        if (order.command_id != id) continue;
        const auto field = [L](const char* name, lua_Number value) {
            lua_pushstring(L, name);
            lua_pushnumber(L, value);
            lua_rawset(L, -3);
        };
        lua_newtable(L);
        field("commandType", order.moho_command);
        field("x", node.position.x);
        field("y", node.position.y);
        field("z", node.position.z);
        field("commandId", id);
        if (order.target_id != 0) {
            lua_pushstring(L, "targetId");
            sim::push_entity_id(L, order.target_id);
            lua_rawset(L, -3);
        }
        if (!order.blueprint_id.empty()) {
            lua_pushstring(L, "blueprintId");
            lua_pushstring(L, order.blueprint_id.c_str());
            lua_rawset(L, -3);
        }
        return 1;
    }
    return 0;
}

/// GetDepositsAroundPoint(X, Z, Radius, Type) in the UI: the map's deposits
/// about a point, as the UI's tick has them (FAF's patch; see
/// push_deposits_around_point).
static int l_UiGetDepositsAroundPoint(lua_State* L) {
    const sim::WorldSnapshot* world = ui_world(L);
    if (!world) return 0;
    return push_deposits_around_point(L, world->deposits);
}

/// CopyToClipboard(text): puts the text on the desktop's clipboard (FAF's
/// patch); whether it did.
static int l_CopyToClipboard(lua_State* L) {
    if (lua_gettop(L) != 1) return arg_count_error(L, "CopyToClipboard", 1, lua_gettop(L));
    auto* r = get_renderer(L);
    const char* text = lua_tostring(L, 1);
    lua_pushboolean(L, r && text && r->copy_to_clipboard(text) ? 1 : 0);
    return 1;
}

static int l_GetSelectedUnits(lua_State* L) {
    auto* ih = get_input_handler(L);
    auto* sim = get_sim(L);
    if (!ih || !sim) {
        lua_newtable(L);
        return 1;
    }

    const auto& selected = ih->selected();
    lua_newtable(L);
    int result = lua_gettop(L);
    int idx = 1;
    for (u32 eid : selected) {
        auto* entity = sim->entity_registry().find(eid);
        if (entity && entity->is_unit() && !entity->destroyed()) {
            push_unit_for_ui(L, entity);
            lua_rawseti(L, result, idx++);
        }
    }
    return 1;
}

// --- Build templates (Moho's CWldSession::GenerateBuildTemplates & co) ---

/// GenerateBuildTemplateFromSelection(): the selected structures (standing,
/// not mobile) as the active template, in the order they were made, each
/// from the first. With none selected the active one is kept.
static int l_GenerateBuildTemplateFromSelection(lua_State* L) {
    auto* ih = get_input_handler(L);
    auto* sim = get_sim(L);
    if (!ih || !sim) return 0;
    std::vector<u32> ids(ih->selected().begin(), ih->selected().end());
    std::sort(ids.begin(), ids.end());
    std::vector<renderer::TemplateStructure> structures;
    for (const u32 id : ids) {
        auto* e = sim->entity_registry().find(id);
        if (!e || !e->is_unit() || e->destroyed()) continue;
        const auto* u = static_cast<const sim::Unit*>(e);
        if (u->is_mobile() || u->is_dying()) continue;
        renderer::TemplateStructure t;
        t.blueprint_id = u->unit_id();
        std::transform(t.blueprint_id.begin(), t.blueprint_id.end(), t.blueprint_id.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        t.creation_tick = u->creation_tick();
        t.x = u->position().x;
        t.z = u->position().z;
        t.foot_x = u->footprint_size_x();
        t.foot_z = u->footprint_size_z();
        t.skirt_x = u->skirt_size_x();
        t.skirt_z = u->skirt_size_z();
        t.skirt_off_x = u->skirt_offset_x();
        t.skirt_off_z = u->skirt_offset_z();
        structures.push_back(std::move(t));
    }
    if (auto made = renderer::generate_build_template(structures))
        ih->build_template() = std::move(*made);
    return 0;
}

/// GetActiveBuildTemplate(): {spanX, spanZ, {bpId, buildOrder, x, z}, ...},
/// or an empty table with none.
static int l_GetActiveBuildTemplate(lua_State* L) {
    auto* ih = get_input_handler(L);
    if (!ih) return 0;
    const renderer::BuildTemplate& t = ih->build_template();
    lua_newtable(L);
    if (t.entries.empty()) return 1;
    const int result = lua_gettop(L);
    lua_pushnumber(L, t.span_x);
    lua_rawseti(L, result, 1);
    lua_pushnumber(L, t.span_z);
    lua_rawseti(L, result, 2);
    int i = 3;
    for (const auto& e : t.entries) {
        lua_newtable(L);
        lua_pushstring(L, e.blueprint_id.c_str());
        lua_rawseti(L, -2, 1);
        lua_pushnumber(L, e.build_order);
        lua_rawseti(L, -2, 2);
        lua_pushnumber(L, e.x);
        lua_rawseti(L, -2, 3);
        lua_pushnumber(L, e.z);
        lua_rawseti(L, -2, 4);
        lua_rawseti(L, result, i++);
    }
    return 1;
}

/// SetActiveBuildTemplate(template): the template (as GetActiveBuildTemplate
/// gives one) active, if it has a structure; malformed entries are warned of
/// and left out.
static int l_SetActiveBuildTemplate(lua_State* L) {
    if (lua_gettop(L) != 1)
        return arg_count_error(L, "set this as an active build template.", 1, lua_gettop(L));
    auto* ih = get_input_handler(L);
    if (!ih) return 0;
    if (!lua_istable(L, 1) || luaL_getn(L, 1) < 3) {
        spdlog::warn("Build template error! Not a table or insufficient members");
        return 0;
    }
    const auto number_at = [L](int table, int i) {
        lua_rawgeti(L, table, i);
        const auto v = static_cast<f32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        return v;
    };
    renderer::BuildTemplate t;
    t.span_x = number_at(1, 1);
    t.span_z = number_at(1, 2);
    const int count = luaL_getn(L, 1);
    for (int i = 3; i <= count; ++i) {
        lua_rawgeti(L, 1, i);
        const int entry = lua_gettop(L);
        if (!lua_istable(L, entry) || luaL_getn(L, entry) < 4) {
            spdlog::warn("Template info error! Not a table or insufficient members");
            lua_pop(L, 1);
            continue;
        }
        renderer::BuildTemplateEntry e;
        lua_rawgeti(L, entry, 1);
        if (const char* bp = lua_tostring(L, -1)) e.blueprint_id = bp;
        lua_pop(L, 1);
        e.build_order = static_cast<i32>(number_at(entry, 2));
        e.x = number_at(entry, 3);
        e.z = number_at(entry, 4);
        t.entries.push_back(std::move(e));
        lua_pop(L, 1);
    }
    if (!t.entries.empty()) ih->build_template() = std::move(t);
    return 0;
}

/// ClearBuildTemplates(): no active template. (Retail calls it as a build
/// mode ends, so the engine's build ghost goes with it, as before.)
static int l_ClearBuildTemplates(lua_State* L) {
    if (auto* ih = get_input_handler(L)) ih->build_template() = {};
    if (auto* sim = get_sim(L)) sim->clear_build_ghost();
    return 0;
}

static int l_SelectUnits(lua_State* L) {
    auto* ih = get_input_handler(L);
    auto* sim = get_sim(L);
    if (!ih) {
        return 0;
    }

    std::unordered_set<u32> new_sel;
    const int n = lua_istable(L, 1) ? luaL_getn(L, 1) : 0; // Lua 5.0: no lua_objlen
    for (int i = 1; i <= n; i++) {
        lua_rawgeti(L, 1, i);
        if (lua_istable(L, -1)) {
            lua_pushstring(L, "EntityId");
            lua_rawget(L, -2);
            if (lua_isnumber(L, -1)) {
                auto eid = sim::to_entity_id(L, -1);
                if (sim) {
                    auto* entity = sim->entity_registry().find(eid);
                    if (entity && renderer::selectable(*entity)) {
                        new_sel.insert(eid);
                    } else if (entity) {
                        const auto* carrier =
                            sim->entity_registry().find(renderer::carrier_of(*entity));
                        if (carrier && renderer::selectable(*carrier)) {
                            new_sel.insert(carrier->entity_id());
                        }
                    }
                } else {
                    new_sel.insert(eid);
                }
            }
            lua_pop(L, 1); // EntityId value
        }
        lua_pop(L, 1); // unit table
    }
    ih->set_selected(new_sel);
    return 0;
}

/// GetValidAttackingUnits() -> the selected units that can attack: those
/// with a weapon other than a dummy or their death's explosion (the attack
/// order's reticle asks). Moho's also tests them against the hovered
/// target; this doesn't.
static int l_GetValidAttackingUnits(lua_State* L) {
    auto* ih = get_input_handler(L);
    auto* sim = get_sim(L);
    lua_newtable(L);
    if (!ih || !sim) return 1;
    const int result = lua_gettop(L);
    int idx = 1;
    for (u32 eid : ih->selected()) {
        auto* entity = sim->entity_registry().find(eid);
        if (!entity || !entity->is_unit() || entity->destroyed()) continue;
        const auto& weapons = static_cast<const sim::Unit*>(entity)->weapons();
        const bool armed = std::any_of(weapons.begin(), weapons.end(), [](const auto& w) {
            return w && !w->fire_on_death && !w->dummy;
        });
        if (!armed) continue;
        push_unit_for_ui(L, entity);
        lua_rawseti(L, result, idx++);
    }
    return 1;
}

static int l_AddSelectUnits(lua_State* L) {
    auto* ih = get_input_handler(L);
    auto* sim = get_sim(L);
    if (!ih || !lua_istable(L, 1)) return 0;

    auto sel = ih->selected(); // copy
    int n = luaL_getn(L, 1);
    for (int i = 1; i <= n; i++) {
        lua_rawgeti(L, 1, i);
        if (lua_istable(L, -1)) {
            lua_pushstring(L, "EntityId");
            lua_rawget(L, -2);
            if (lua_isnumber(L, -1)) {
                auto eid = sim::to_entity_id(L, -1);
                if (sim) {
                    auto* entity = sim->entity_registry().find(eid);
                    if (entity && renderer::selectable(*entity)) {
                        sel.insert(eid);
                    }
                } else {
                    sel.insert(eid);
                }
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }
    ih->set_selected(sel);
    return 0;
}

// SimCallback({Func="name", Args={...} or a value}, addUnitSelection)
// Serializes the Lua table into a C++ SimCallbackEntry and queues it.
static int l_SimCallback(lua_State* L) {
    auto* queue = get_callback_queue(L);
    if (!queue || !lua_istable(L, 1)) return 0;

    sim::SimCallbackEntry entry;

    // Read Func field
    lua_pushstring(L, "Func");
    lua_rawget(L, 1);
    if (lua_type(L, -1) == LUA_TSTRING) {
        entry.func_name = lua_tostring(L, -1);
    }
    lua_pop(L, 1);

    if (entry.func_name.empty()) return 0; // no function name = skip
    // The engine's own callbacks (a defeat, a post-load...) are the engine's
    // to issue, not a script's.
    if (entry.func_name.rfind(sim::kEngineCallbackPrefix, 0) == 0) {
        spdlog::warn("SimCallback: '{}' is the engine's own; not sent", entry.func_name);
        return 0;
    }

    lua_pushstring(L, "Args");
    lua_rawget(L, 1);
    entry.lua_args = sim::lua_to_bytes(L, -1);
    lua_pop(L, 1);
    if (!entry.lua_args) {
        spdlog::warn("SimCallback: '{}' Args can't be carried", entry.func_name);
        return 0;
    }

    // Check addUnitSelection (arg 2)
    if (lua_toboolean(L, 2)) {
        auto* ih = get_input_handler(L);
        if (ih) {
            const auto& selected = ih->selected();
            entry.unit_ids.reserve(selected.size());
            for (u32 eid : selected) {
                entry.unit_ids.push_back(eid);
            }
        }
    }

    queue->push(std::move(entry));
    return 0;
}

/// GetUnitCommandFromCommandCap("RULEUCC_Move") → "Move": the order a
/// command cap issues, by Moho's table (FAF's engine notes), "None" for a
/// cap with none. Not every name is the cap's: RULEUCC_SiloBuildNuke issues
/// BuildSiloNuke, RULEUCC_Transport TransportUnloadUnits.
static int l_GetUnitCommandFromCommandCap(lua_State* L) {
    static const std::map<std::string_view, const char*> kOrders = {
        {"RULEUCC_Move", "Move"},
        {"RULEUCC_Stop", "Stop"},
        {"RULEUCC_Attack", "Attack"},
        {"RULEUCC_Guard", "Guard"},
        {"RULEUCC_Patrol", "Patrol"},
        {"RULEUCC_Repair", "Repair"},
        {"RULEUCC_Capture", "Capture"},
        {"RULEUCC_Transport", "TransportUnloadUnits"},
        {"RULEUCC_CallTransport", "TransportLoadUnits"},
        {"RULEUCC_Nuke", "Nuke"},
        {"RULEUCC_Tactical", "Tactical"},
        {"RULEUCC_Teleport", "Teleport"},
        {"RULEUCC_Ferry", "Ferry"},
        {"RULEUCC_SiloBuildTactical", "BuildSiloTactical"},
        {"RULEUCC_SiloBuildNuke", "BuildSiloNuke"},
        {"RULEUCC_Sacrifice", "Sacrifice"},
        {"RULEUCC_Pause", "Pause"},
        {"RULEUCC_Overcharge", "OverCharge"},
        {"RULEUCC_Dive", "Dive"},
        {"RULEUCC_Reclaim", "Reclaim"},
        {"RULEUCC_SpecialAction", "SpecialAction"},
    };
    const auto it = kOrders.find(luaL_checkstring(L, 1));
    lua_pushstring(L, it != kOrders.end() ? it->second : "None");
    return 1;
}

/// The selection's unit ids, in id order.
static std::vector<u32> selected_unit_ids(lua_State* L) {
    auto* ih = get_input_handler(L);
    if (!ih) return {};
    std::vector<u32> ids(ih->selected().begin(), ih->selected().end());
    std::sort(ids.begin(), ids.end());
    return ids;
}

/// IssueCommand(command [, data [, clear]]) -- for the selection: the orders
/// panel's Stop, Dive and silo builds, the construction panel's enhancements.
static int l_IssueCommand(lua_State* L) {
    const std::string name = ui_command_name(luaL_checkstring(L, 1));
    const bool clear = lua_isboolean(L, 3) ? lua_toboolean(L, 3) != 0 : true;
    issue_targetless_order(L, selected_unit_ids(L), name, 2, clear);
    return 0;
}

/// IssueDockCommand(clear) -- the orders panel's Dock (M206r), as Moho's
/// cfunc_IssueDockCommandL gives it: the selection's units that can dock go
/// to the focus army's idle air staging platforms. All go to the nearest if
/// it has room for them; otherwise they are shared among those within 100 of
/// its distance, the roomiest first, each taking its share. Measured from the
/// units' centre (from where their queued orders end, when not clearing).
static int l_IssueDockCommand(lua_State* L) {
    const bool clear = lua_toboolean(L, 1) != 0;
    auto* sim = get_sim(L);
    if (!sim) return 0;
    int focus = -1;
    lua_pushstring(L, "__osc_focus_army");
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (lua_isnumber(L, -1)) focus = static_cast<int>(lua_tonumber(L, -1));
    lua_pop(L, 1);
    if (focus < 0) return 0;
    auto& registry = sim->entity_registry();
    const auto live_unit = [&](u32 id) -> const sim::Unit* {
        const sim::Entity* e = registry.find(id);
        return e && !e->destroyed() && e->is_unit() ? static_cast<const sim::Unit*>(e) : nullptr;
    };

    std::vector<u32> docking;
    f32 cx = 0.0f;
    f32 cz = 0.0f;
    for (const u32 id : selected_unit_ids(L)) {
        const sim::Unit* u = live_unit(id);
        if (!u || !u->has_command_cap("RULEUCC_Dock")) continue;
        sim::Vector3 at = u->position();
        if (!clear && !u->command_queue().empty()) {
            const sim::UnitCommand& last = u->command_queue().back();
            const sim::Unit* target = last.target_id != 0 ? live_unit(last.target_id) : nullptr;
            at = target ? target->position() : last.target_pos;
        }
        cx += at.x;
        cz += at.z;
        docking.push_back(id);
    }
    if (docking.empty()) return 0;
    cx /= static_cast<f32>(docking.size());
    cz /= static_cast<f32>(docking.size());

    struct Pad {
        const sim::Unit* unit;
        f32 d2;
        i32 room;
    };
    static const sim::CategoryName kStaging{"AIRSTAGINGPLATFORM"};
    std::vector<Pad> pads;
    bool any_pad = false;
    registry.for_each_unit([&](sim::Entity& e) {
        if (e.destroyed() || !e.is_unit() || e.army() != focus) return;
        const auto& pad = static_cast<const sim::Unit&>(e);
        if (pad.is_being_built() || pad.is_dying() || !pad.has_category(kStaging)) return;
        any_pad = true;
        if (pad.layer() == "Sub" || pad.layer() == "Seabed" || !pad.command_queue().empty()) return;
        const i32 room = pad.storage_slots() != 0
                             ? pad.storage_slots() - static_cast<i32>(pad.stored_ids().size())
                             : pad.docking_slots();
        if (room <= 0) return;
        const f32 dx = pad.position().x - cx;
        const f32 dz = pad.position().z - cz;
        pads.push_back({&pad, dz * dz + dx * dx, room});
    });
    if (pads.empty()) {
        if (auto* queue = get_callback_queue(L)) {
            sim::SimCallbackEntry entry;
            entry.func_name = sim::kProcessInfoCallback;
            entry.args["Action"] =
                std::string(any_pad ? "PlayBusyStagingPlatformsVO" : "PlayNoStagingPlatformsVO");
            entry.args["Value"] = std::string("play");
            entry.unit_ids.push_back(selected_unit_ids(L).front());
            queue->push(std::move(entry));
        }
        return 0;
    }
    std::sort(pads.begin(), pads.end(), [](const Pad& a, const Pad& b) {
        if (a.d2 != b.d2) return a.d2 < b.d2;
        return a.unit->entity_id() < b.unit->entity_id();
    });

    sim::UnitCommand cmd;
    cmd.type = sim::CommandType::Dock;
    const auto dock = [&](const std::vector<u32>& ids, const sim::Unit& pad) {
        cmd.target_id = pad.entity_id();
        cmd.target_pos = pad.position();
        issue_player_order(L, ids, cmd, clear);
    };
    const i32 count = static_cast<i32>(docking.size());
    if (count <= pads.front().room) {
        dock(docking, *pads.front().unit);
        return 0;
    }
    const f32 reach = std::sqrt(pads.front().d2) + 100.0f;
    std::vector<Pad> nearby;
    i32 total = 0;
    for (const Pad& pad : pads) {
        if (reach * reach > pad.d2) {
            nearby.push_back(pad);
            total += pad.room;
        }
    }
    std::sort(nearby.begin(), nearby.end(), [](const Pad& a, const Pad& b) {
        if (a.room != b.room) return a.room > b.room;
        if (a.d2 != b.d2) return a.d2 < b.d2;
        return a.unit->entity_id() < b.unit->entity_id();
    });
    // Each takes its room times max(1, units / total room), rounded, and
    // rounded up on any remainder. (faf-re's decompile walks the units from
    // the start for every platform, which would send the same ones to each;
    // each takes the next ones here, as the shares mean.)
    const f32 ratio = static_cast<f32>(count) / static_cast<f32>(total);
    const f32 scale = ratio > 1.0f ? ratio : 1.0f;
    size_t next = 0;
    for (const Pad& pad : nearby) {
        const f32 share = static_cast<f32>(pad.room) * scale;
        const f32 rounded = std::rint(share);
        i32 quota = static_cast<i32>(rounded);
        if (share > rounded) ++quota;
        std::vector<u32> ids;
        while (next < docking.size() && static_cast<i32>(ids.size()) < quota)
            ids.push_back(docking[next++]);
        if (ids.empty()) break;
        dock(ids, *pad.unit);
    }
    return 0;
}

/// IssueBlueprintCommand(command, blueprint [, count [, clear]]) -- for the
/// selection: the construction panel's factory builds and upgrades.
static int l_IssueBlueprintCommand(lua_State* L) {
    const std::string name = ui_command_name(luaL_checkstring(L, 1));
    sim::UnitCommand cmd;
    cmd.blueprint_id = luaL_checkstring(L, 2);
    const int count = lua_isnumber(L, 3) ? static_cast<int>(lua_tonumber(L, 3)) : 1;
    const bool clear = lua_isboolean(L, 4) && lua_toboolean(L, 4) != 0;
    const auto ids = selected_unit_ids(L);
    if (name == "BuildFactory") {
        cmd.type = sim::CommandType::BuildFactory;
        for (int i = 0; i < std::min(count, 1000); ++i)
            issue_player_order(L, ids, cmd, clear && i == 0);
    } else if (name == "Upgrade") {
        cmd.type = sim::CommandType::Upgrade;
        issue_player_order(L, ids, cmd, clear);
    } else {
        spdlog::warn("IssueBlueprintCommand: unsupported order '{}'", name);
    }
    return 0;
}

/// IsKeyDown(keyCode | keyName): whether a key is held, a modifier by its
/// name
static int l_IsKeyDown(lua_State* L) {
    std::vector<int> keys;
    if (lua_type(L, 1) == LUA_TSTRING) {
        keys = ui::glfw_keys_named(lua_tostring(L, 1));
    } else {
        // FA key codes map to GLFW key codes for most keys
        keys.push_back(static_cast<int>(luaL_checknumber(L, 1)));
    }
    auto* r = get_renderer(L);
    if (auto* reg = get_ui_registry(L); reg && reg->key_down_blocked()) {
        r = nullptr;
    }
    bool down = false;
    for (int key : keys) {
        down = down || (r && r->is_key_pressed(key));
    }
    lua_pushboolean(L, down ? 1 : 0);
    return 1;
}

/// GetRolloverInfo() — returns a rich table describing the hovered or first-selected unit (M142a).
static int l_GetRolloverInfo(lua_State* L) {
    auto* sim = get_sim(L);
    if (!sim) {
        lua_pushnil(L);
        return 1;
    }

    sim::Unit* unit = nullptr;

    auto* input = get_input_handler(L);
    if (input && input->hovered() != 0) {
        auto* entity = sim->entity_registry().find(input->hovered());
        if (entity && entity->is_unit() && !entity->destroyed()) {
            unit = static_cast<sim::Unit*>(entity);
        }
    }
    if (!unit) {
        if (input && !input->selected().empty()) {
            u32 first_id = *input->selected().begin();
            auto* entity = sim->entity_registry().find(first_id);
            if (entity && entity->is_unit() && !entity->destroyed())
                unit = static_cast<sim::Unit*>(entity);
        }
    }

    if (!unit) {
        lua_pushnil(L);
        return 1;
    }

    // Helper lambdas for building the result table
    auto set_str = [&](const char* key, const char* val) {
        lua_pushstring(L, key);
        lua_pushstring(L, val);
        lua_rawset(L, -3);
    };
    auto set_num = [&](const char* key, lua_Number val) {
        lua_pushstring(L, key);
        lua_pushnumber(L, val);
        lua_rawset(L, -3);
    };

    // Moho's rollover of a blip (faf-re BuildUserUnitRolloverInfo): never
    // seen, only its army; seen before, its blueprint without its stats.
    const renderer::Sight sight = input ? input->sight(*unit) : renderer::Sight::Seen;
    if (sight == renderer::Sight::Blip) {
        lua_newtable(L);
        set_str("blueprintId", "unknown");
        set_num("armyIndex", static_cast<lua_Number>(unit->army()));
        return 1;
    }
    if (sight != renderer::Sight::Seen) {
        lua_newtable(L);
        set_str("blueprintId", unit->blueprint_id().c_str());
        set_num("armyIndex", static_cast<lua_Number>(unit->army()));
        for (const char* key :
             {"kills", "energyRequested", "massRequested", "tacticalSiloBuildCount",
              "tacticalSiloStorageCount", "tacticalSiloMaxStorageCount", "nukeSiloBuildCount",
              "nukeSiloStorageCount", "nukeSiloMaxStorageCount"}) {
            set_num(key, 0);
        }
        for (const char* key : {"energyConsumed", "massConsumed", "energyProduced", "massProduced",
                                "fuelRatio", "shieldRatio", "workProgress"}) {
            set_num(key, -1);
        }
        return 1;
    }

    lua_newtable(L); // result table

    set_str("blueprintId", unit->blueprint_id().c_str());
    lua_pushstring(L, "entityId");
    sim::push_entity_id(L, unit->entity_id());
    lua_rawset(L, -3);
    set_num("health", static_cast<lua_Number>(unit->health()));
    set_num("maxHealth", static_cast<lua_Number>(unit->max_health()));
    set_num("kills", static_cast<lua_Number>(unit->get_stat("KILLS", 0)));
    set_num("armyIndex", static_cast<lua_Number>(unit->army()));
    set_num("workProgress", static_cast<lua_Number>(unit->work_progress()));
    set_num("shieldRatio", static_cast<lua_Number>(unit->shield_ratio()));
    set_num("fuelRatio", static_cast<lua_Number>(unit->fuel_ratio()));

    // Moho's rollover reports whole units a second (an int cast of each).
    const auto& econ = unit->economy();
    const auto whole = [](f64 v) { return static_cast<lua_Number>(static_cast<int>(v)); };
    set_num("massProduced", whole(econ.mass_produced()));
    set_num("massConsumed", whole(econ.mass_consumed()));
    set_num("energyProduced", whole(econ.energy_produced()));
    set_num("energyConsumed", whole(econ.energy_consumed()));
    set_num("massRequested", whole(econ.mass_requested()));
    set_num("energyRequested", whole(econ.energy_requested()));

    set_num("tacticalSiloStorageCount", static_cast<lua_Number>(unit->silo_ammo(false)));
    set_num("tacticalSiloMaxStorageCount", static_cast<lua_Number>(unit->silo_max_storage(false)));
    set_num("nukeSiloStorageCount", static_cast<lua_Number>(unit->silo_ammo(true)));
    set_num("nukeSiloMaxStorageCount", static_cast<lua_Number>(unit->silo_max_storage(true)));
    set_num("tacticalSiloBuildCount", static_cast<lua_Number>(unit->silo_build_count(false)));
    set_num("nukeSiloBuildCount", static_cast<lua_Number>(unit->silo_build_count(true)));

    // userUnit: full UI unit table with _c_object + metatable
    lua_pushstring(L, "userUnit");
    push_unit_for_ui(L, unit);
    lua_rawset(L, -3);

    // focus: what the unit builds; "focusUpgrade" while it upgrades (faf-re
    // cfunc_GetRolloverInfoL)
    u32 focus_id = unit->build_target_id();
    if (focus_id != 0) {
        auto* focus_entity = sim->entity_registry().find(focus_id);
        if (focus_entity && focus_entity->is_unit() && !focus_entity->destroyed()) {
            auto* focus_unit = static_cast<sim::Unit*>(focus_entity);
            const bool upgrading = !unit->command_queue().empty() &&
                                   unit->command_queue().front().type == sim::CommandType::Upgrade;
            lua_pushstring(L, upgrading ? "focusUpgrade" : "focus");
            lua_newtable(L); // focus sub-table
            lua_pushstring(L, "blueprintId");
            lua_pushstring(L, focus_unit->blueprint_id().c_str());
            lua_rawset(L, -3);
            lua_pushstring(L, "entityId");
            sim::push_entity_id(L, focus_unit->entity_id());
            lua_rawset(L, -3);
            set_num("health", static_cast<lua_Number>(focus_unit->health()));
            set_num("maxHealth", static_cast<lua_Number>(focus_unit->max_health()));
            lua_rawset(L, -3); // result["focus"] = focus_sub_table
        }
    }

    return 1;
}

std::vector<sim::FormationSlot> plan_ui_formation(lua_State* L,
                                                  const std::vector<sim::FormationMember>& units,
                                                  const map::Terrain* terrain,
                                                  const std::string& script, const sim::Vector3& at,
                                                  f32 facing) {
    return sim::plan_formation(
        L, units,
        [](lua_State* state, const sim::FormationMember& u) {
            push_user_unit(state, u.id, u.army);
        },
        terrain, script, at, facing);
}

void update_world_view_cursor(lua_State* L) {
    lua_pushstring(L, "__osc_world_view");
    lua_rawget(L, LUA_REGISTRYINDEX);
    const auto* view = static_cast<const ui::WorldView*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    if (!view || view->destroyed() || view->lua_table_ref() < 0) {
        return;
    }
    const int top = lua_gettop(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, view->lua_table_ref());
    lua_pushstring(L, "OnUpdateCursor");
    lua_gettable(L, -2);
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, -2);
        if (lua_pcall(L, 1, 0, 0) != 0) {
            static bool reported = false;
            if (!reported) {
                spdlog::warn("OnUpdateCursor: {}", lua_tostring(L, -1));
                reported = true;
            }
        }
    }
    lua_settop(L, top);
}

void register_user_bindings(LuaState& state) {
    lua_State* L = state.raw();
    // Globals of the UI state.
    state.register_function("SetInvertMidMouseButton", l_SetInvertMidMouseButton);
    state.register_function("AddCommandFeedbackBlip", l_AddCommandFeedbackBlip);
    state.register_function("IsDestroyed", ui_is_destroyed);
    // FAF's engine/Library.lua: the engine's statistics tree, which it fills
    // each frame (FAF's performance and logger modules read it; it is
    // empty here), and the folder the executable is in.
    lua_pushstring(L, "__EngineStats");
    lua_newtable(L);
    lua_pushstring(L, "Name");
    lua_pushstring(L, "Root");
    lua_rawset(L, -3);
    lua_pushstring(L, "Children");
    lua_newtable(L);
    lua_rawset(L, -3);
    lua_rawset(L, LUA_GLOBALSINDEX);
    lua_pushstring(L, "LaunchDir");
    lua_pushstring(L, platform::executable_path().parent_path().string().c_str());
    lua_rawset(L, LUA_GLOBALSINDEX);
    state.register_function("InternalCreateWldUIProvider", l_InternalCreateWldUIProvider);
    state.register_function("GetMouseWorldPos", l_GetMouseWorldPos);
    state.register_function("GetMouseScreenPos", l_GetMouseScreenPos);
    state.register_function("CreateUnitAtMouse", l_CreateUnitAtMouse);
    state.register_function("GetAntiAliasingOptions", l_GetAntiAliasingOptions);
    state.register_function("GetHighlightCommand", l_GetHighlightCommand);
    state.register_function("GetDepositsAroundPoint", l_UiGetDepositsAroundPoint);
    state.register_function("CopyToClipboard", l_CopyToClipboard);
    state.register_function("UI_DrawLine", l_UI_DrawLine);
    state.register_function("UI_DrawCircle", l_UI_DrawCircle);
    state.register_function("UI_DrawRect", l_UI_DrawRect);
    state.register_function("UnProject", l_UnProject);
    state.register_function("GetCamera", l_GetCamera);
    state.register_function("SyncPlayableRect", l_SyncPlayableRect);
    state.register_function("RenderOverlayEconomy", l_RenderOverlayEconomy);
    state.register_function("TeamColorMode", l_TeamColorMode);
    state.register_function("SetOverlayFilter", l_SetOverlayFilter);
    state.register_function("SetOverlayFilters", l_SetOverlayFilters);
    state.register_function("GenerateBuildTemplateFromSelection",
                            l_GenerateBuildTemplateFromSelection);
    state.register_function("GetActiveBuildTemplate", l_GetActiveBuildTemplate);
    state.register_function("SetActiveBuildTemplate", l_SetActiveBuildTemplate);
    state.register_function("ClearBuildTemplates", l_ClearBuildTemplates);
    set_ui_wait_hook(L, ui_wait_camera); // WaitFor(camera) (M217g)
    state.register_function("GetSelectedUnits", l_GetSelectedUnits);
    state.register_function("SelectUnits", l_SelectUnits);
    state.register_function("AddSelectUnits", l_AddSelectUnits);
    state.register_function("SimCallback", l_SimCallback);
    state.register_function("GetUnitCommandFromCommandCap", l_GetUnitCommandFromCommandCap);
    state.register_function("IssueCommand", l_IssueCommand);
    state.register_function("IssueDockCommand", l_IssueDockCommand);
    state.register_function("IssueBlueprintCommand", l_IssueBlueprintCommand);
    state.register_function("IsKeyDown", l_IsKeyDown);
    state.register_function("UIZoomTo", l_UIZoomTo);
    state.register_function("UISelectAndZoomTo", l_UISelectAndZoomTo);
    state.register_function("UISelectionByCategory", l_UISelectionByCategory);
    state.register_function("GetValidAttackingUnits", l_GetValidAttackingUnits);
    state.register_function("_c_CreateDecal", l_c_CreateDecal);
    state.register_function("GetRolloverInfo", l_GetRolloverInfo);

    // Methods of the classes register_moho_bindings made: before any UI
    // script builds its classes from them.
    add_class_methods(L, "userDecal_methods",
                      {
                          {"SetTexture", userdecal_SetTexture},
                          {"SetScale", userdecal_SetScale},
                          {"SetPosition", userdecal_SetPosition},
                          {"SetPositionByScreen", userdecal_SetPositionByScreen},
                          {"Destroy", userdecal_Destroy},
                      });
    add_class_methods(L, "ui_map_preview_methods",
                      {
                          {"SetTextureFromMap", mappreview_SetTextureFromMap},
                      });
    add_class_methods(L, "UIWorldView",
                      {
                          {"__init", worldview_init},
                          {"Project", worldview_Project},
                          {"ProjectMultiple", worldview_ProjectMultiple},
                          {"Register", worldview_Register},
                      });
    add_class_methods(L, "camera_methods",
                      {
                          {"SaveSettings", camera_SaveSettings},
                          {"RestoreSettings", camera_RestoreSettings},
                          {"SetZoom", camera_SetZoom},
                          {"GetZoom", camera_GetZoom},
                          {"SetMaxZoomMult", camera_SetMaxZoomMult},
                          {"GetMinZoom", camera_GetMinZoom},
                          {"GetMaxZoom", camera_GetMaxZoom},
                          {"SetTargetZoom", camera_SetTargetZoom},
                          {"GetTargetZoom", camera_GetTargetZoom},
                          {"Reset", camera_Reset},
                          {"MoveTo", camera_MoveTo},
                          {"SnapTo", camera_MoveTo},
                          {"MoveToRegion", camera_MoveToRegion},
                          {"GetFocusPosition", camera_GetFocusPosition},
                          {"GetHeading", camera_GetHeading},
                          {"GetPitch", camera_GetPitch},
                          {"TargetEntities", camera_TargetEntities},
                          {"TrackEntities", camera_TrackEntities},
                          {"NoseCam", camera_NoseCam},
                          {"Spin", camera_Spin},
                          {"SetAccMode", camera_SetAccMode},
                          {"EnableEaseInOut", camera_EnableEaseInOut},
                          {"DisableEaseInOut", camera_DisableEaseInOut},
                          {"HoldRotation", camera_HoldRotation},
                          {"RevertRotation", camera_RevertRotation},
                          {"UseGameClock", camera_UseGameClock},
                          {"UseSystemClock", camera_UseSystemClock},
                      });
}

// ── The console's session commands (M217d) ──────────────────────────────────

namespace {

bool iequal(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) ==
                      std::tolower(static_cast<unsigned char>(y));
           });
}

/// Push `module`'s field `fn` (import(module)[fn]); false, with the stack as
/// it was, if the module or its function is missing.
bool push_module_function(lua_State* L, const char* module, const char* fn) {
    const int top = lua_gettop(L);
    lua_pushstring(L, "import");
    lua_rawget(L, LUA_GLOBALSINDEX);
    lua_pushstring(L, module);
    if (!lua_isfunction(L, -2) || lua_pcall(L, 1, 1, 0) != 0 || !lua_istable(L, -1)) {
        spdlog::warn("Error running '{}:{}': the module didn't load", module, fn);
        lua_settop(L, top);
        return false;
    }
    lua_pushstring(L, fn);
    lua_gettable(L, -2);
    lua_remove(L, -2); // the module
    if (!lua_isfunction(L, -1)) {
        lua_settop(L, top);
        return false;
    }
    return true;
}

/// Where the cursor points in the world (GetMouseWorldPos's answer); false
/// with no world view.
bool cursor_world_pos(lua_State* L, sim::Vector3& out) {
    lua_pushstring(L, "__osc_world_view");
    lua_rawget(L, LUA_REGISTRYINDEX);
    auto* wv = static_cast<ui::WorldView*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    if (!wv || !wv->camera()) return false;
    auto* r = get_renderer(L);
    if (!r) {
        return wv->get_mouse_world_pos(0.0f, 0.0f, out.x, out.y, out.z);
    }
    f64 dx = 0, dy = 0;
    r->ui_mouse_position(dx, dy);
    return renderer::pick_ground_in(r->ui_views(), static_cast<f32>(dx), static_cast<f32>(dy),
                                    wv->terrain(), out.x, out.y, out.z);
}

/// The child named `name` of the stat item on top of the stack (in its
/// Children list, which it makes if need be), pushed; made if missing.
static void push_stat_child(lua_State* L, const char* name) {
    lua_pushstring(L, "Children");
    lua_rawget(L, -2);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushstring(L, "Children");
        lua_pushvalue(L, -2);
        lua_rawset(L, -4);
    }
    const int n = luaL_getn(L, -1);
    for (int i = 1; i <= n; ++i) {
        lua_rawgeti(L, -1, i);
        if (lua_istable(L, -1)) {
            lua_pushstring(L, "Name");
            lua_rawget(L, -2);
            const char* got = lua_tostring(L, -1);
            const bool match = got && std::strcmp(got, name) == 0;
            lua_pop(L, 1);
            if (match) {
                lua_remove(L, -2); // the Children list
                return;
            }
        }
        lua_pop(L, 1);
    }
    lua_newtable(L);
    lua_pushstring(L, "Name");
    lua_pushstring(L, name);
    lua_rawset(L, -3);
    lua_pushvalue(L, -1);
    lua_rawseti(L, -3, n + 1);
    luaL_setn(L, -2, n + 1);
    lua_remove(L, -2);
}

/// Set the leaf stat `name` of the item on top of the stack.
static void set_stat(lua_State* L, const char* name, const char* type, auto push_value) {
    push_stat_child(L, name);
    lua_pushstring(L, "Type");
    lua_pushstring(L, type);
    lua_rawset(L, -3);
    lua_pushstring(L, "Value");
    push_value();
    lua_rawset(L, -3);
    lua_pop(L, 1);
}

/// CON_StartCommandMode: `StartCommandMode mode name` starts the command
/// mode (commandmode.lua's StartCommandMode(mode, {name = name})) if a
/// selected unit has the command cap `name`; the same mode again ends it.
void start_command_mode(lua_State* L, const std::vector<std::string>& args) {
    auto* sim = get_sim(L);
    if (!sim) {
        spdlog::info("No session");
        return;
    }
    if (args.size() < 3) return;
    const std::string& mode = args[1];
    const std::string& name = args[2];

    // The active mode: GetCommandMode() -> {mode, payload}.
    const int top = lua_gettop(L);
    std::string active_mode, active_name;
    if (push_module_function(L, "/lua/ui/game/commandmode.lua", "GetCommandMode")) {
        if (lua_pcall(L, 0, 1, 0) != 0) {
            const char* err = lua_tostring(L, -1);
            spdlog::warn("Error running '/lua/ui/game/commandmode.lua:GetCommandMode': {}",
                         err ? err : "(unknown error)");
        } else if (lua_istable(L, -1)) {
            lua_rawgeti(L, -1, 1);
            if (lua_type(L, -1) == LUA_TSTRING) active_mode = lua_tostring(L, -1);
            lua_pop(L, 1);
            lua_rawgeti(L, -1, 2);
            if (lua_istable(L, -1)) {
                lua_pushstring(L, "name");
                lua_gettable(L, -2);
                if (lua_type(L, -1) == LUA_TSTRING) active_name = lua_tostring(L, -1);
                lua_pop(L, 1);
            }
            lua_pop(L, 1);
        }
        lua_settop(L, top);
    }
    if (iequal(active_mode, mode) && iequal(active_name, name) && !active_mode.empty()) {
        if (push_module_function(L, "/lua/ui/game/commandmode.lua", "EndCommandMode") &&
            lua_pcall(L, 0, 0, 0) != 0) {
            spdlog::warn("Error running '/lua/ui/game/commandmode.lua:EndCommandMode': {}",
                         lua_tostring(L, -1));
        }
        lua_settop(L, top);
        return;
    }

    // A selected unit with the cap.
    auto* ih = get_input_handler(L);
    bool capable = false;
    if (ih) {
        for (u32 id : ih->selected()) {
            auto* e = sim->entity_registry().find(id);
            if (e && e->is_unit() && !e->destroyed() &&
                static_cast<const sim::Unit*>(e)->has_command_cap(name)) {
                capable = true;
                break;
            }
        }
    }
    if (!capable) return;
    if (push_module_function(L, "/lua/ui/game/commandmode.lua", "StartCommandMode")) {
        lua_pushstring(L, mode.c_str());
        lua_newtable(L);
        lua_pushstring(L, "name");
        lua_pushstring(L, name.c_str());
        lua_rawset(L, -3);
        if (lua_pcall(L, 2, 0, 0) != 0)
            spdlog::warn("Error running '/lua/ui/game/commandmode.lua:StartCommandMode': {}",
                         lua_tostring(L, -1));
    }
    lua_settop(L, top);
}

/// How SelectUnitsByCategory picks: the options the console's modifiers and
/// UISelectionByCategory's arguments give.
struct CategorySelection {
    bool add = false;               ///< to the selection, not in its place
    bool in_view = false;           ///< only units in the world camera's view
    bool nearest = false;           ///< only the one nearest the cursor
    bool idle = false;              ///< only units not busy and with no orders
    bool go_to = false;             ///< the camera goes to them
    bool exclude_engineers = false; ///< no ENGINEER or COMMAND unit
};

/// Moho's SelectUnitsByCategory (faf-re CCommandLuaFunctionRegistrations.cpp,
/// 0x008662B0): the focus army's selectable units in the category expression
/// ("A B, C": spaces intersect, commas unite), filtered, as the selection.
void select_units_by_category(lua_State* L, const std::string& expression,
                              const CategorySelection& how) {
    auto* sim = get_sim(L);
    auto* ih = get_input_handler(L);
    if (!sim || !ih) {
        spdlog::info("No session");
        return;
    }
    const bool add = how.add;
    const bool in_view = how.in_view;
    const bool nearest = how.nearest;
    const bool idle = how.idle;
    const bool go_to = how.go_to;
    const bool exclude_engineers = how.exclude_engineers;
    const sim::CategoryExpr category = sim::parse_category_list(expression);
    auto* r = get_renderer(L);
    std::optional<renderer::Frustum> view;
    if (in_view && r) {
        view.emplace(r->camera().view_proj(static_cast<f32>(r->width()) /
                                           static_cast<f32>(std::max(1u, r->height()))));
    }
    sim::Vector3 cursor{};
    const bool have_cursor = nearest && cursor_world_pos(L, cursor);

    std::unordered_set<u32> result;
    if (add) result = ih->selected();
    const sim::Unit* nearest_unit = nullptr;
    f32 nearest_distance = std::numeric_limits<f32>::infinity();
    sim->entity_registry().for_each_unit([&](sim::Entity& e) {
        const auto& u = static_cast<const sim::Unit&>(e);
        if (!renderer::selectable(u) || u.army() != ih->player_army()) return;
        const auto& p = u.position();
        if (view && !view->is_sphere_visible(p.x, p.y, p.z, 0.0f)) return;
        if (idle && (u.busy() || !u.command_queue().empty())) return;
        if (!category.matches(u.categories())) return;
        if (exclude_engineers &&
            (u.categories().count("ENGINEER") != 0 || u.categories().count("COMMAND") != 0))
            return;
        if (nearest) {
            const f32 dx = p.x - cursor.x, dy = p.y - cursor.y, dz = p.z - cursor.z;
            const f32 d = have_cursor ? std::sqrt(dx * dx + dy * dy + dz * dz) : 0.0f;
            if (d < nearest_distance) {
                nearest_distance = d;
                nearest_unit = &u;
            }
        } else if (!u.has_unit_state("BeingUpgraded")) {
            result.insert(u.entity_id());
        }
    });
    if (nearest_unit) result.insert(nearest_unit->entity_id());

    // +goto: the camera goes to what was selected -- the unit, or the box
    // around them all.
    if (go_to && r && !result.empty()) {
        f32 x0 = std::numeric_limits<f32>::infinity(), z0 = x0;
        f32 x1 = -x0, z1 = -x0;
        for (u32 id : result) {
            if (auto* e = sim->entity_registry().find(id)) {
                x0 = std::min(x0, e->position().x);
                z0 = std::min(z0, e->position().z);
                x1 = std::max(x1, e->position().x);
                z1 = std::max(z1, e->position().z);
            }
        }
        if (x0 <= x1) r->camera().set_target((x0 + x1) * 0.5f, (z0 + z1) * 0.5f);
    }
    ih->set_selected(result);
}

/// UI_SelectByCategory [+add] [+nearest] [+idle] [+inview] [+goto]
/// [+excludeengineers] expression: SelectUnitsByCategory.
void select_by_category(lua_State* L, const std::vector<std::string>& args) {
    if (!get_sim(L) || !get_input_handler(L)) {
        spdlog::info("No session");
        return;
    }
    if (args.size() < 2) {
        spdlog::info("UI_SelectByCategory [+add] [+nearest] [+idle] [+goto] categoryExpression");
        return;
    }
    CategorySelection how;
    std::string expression;
    for (size_t i = 1; i < args.size(); ++i) {
        const std::string& token = args[i];
        if (!token.empty() && token[0] == '+') {
            if (iequal(token, "+add")) how.add = true;
            else if (iequal(token, "+nearest")) how.nearest = true;
            else if (iequal(token, "+idle")) how.idle = true;
            else if (iequal(token, "+inview")) how.in_view = true;
            else if (iequal(token, "+goto")) how.go_to = true;
            else if (iequal(token, "+excludeengineers")) how.exclude_engineers = true;
            else spdlog::info("Unknown modifier {}", token);
            continue;
        }
        if (!expression.empty()) expression += ' ';
        expression += token;
    }
    select_units_by_category(L, expression, how);
}

/// UISelectionByCategory(expression, addToCurSel, inViewFrustum,
/// nearestToMouse, mustBeIdle): Moho's (cfunc_UISelectionByCategoryL,
/// 0x00866DB0), SelectUnitsByCategory with no camera move and no engineer
/// exclusion. FAF's key actions, avatars and enhancement queue select so.
int l_UISelectionByCategory(lua_State* L) {
    if (const int n = lua_gettop(L); n != 5)
        return arg_count_error(L,
                               "UISelectionByCategory(expression, addToCurSel, inViewFrustum, "
                               "nearestToMouse, mustBeIdle) - selects units based on a category "
                               "expression",
                               5, n);
    if (!get_sim(L) || !get_input_handler(L)) {
        spdlog::info("No session");
        return 0;
    }
    const char* expression = lua_tostring(L, 1);
    if (!expression) return luaL_typerror(L, 1, "string");
    CategorySelection how;
    how.add = lua_toboolean(L, 2) != 0;
    how.in_view = lua_toboolean(L, 3) != 0;
    how.nearest = lua_toboolean(L, 4) != 0;
    how.idle = lua_toboolean(L, 5) != 0;
    select_units_by_category(L, expression, how);
    return 0;
}

/// CON_IssueCommand: `IssueCommand Stop|Dive|SiloBuildTactical|
/// SiloBuildNuke` to the selection (Stop clears the queues and announces the
/// selection again).
void issue_command(lua_State* L, const std::vector<std::string>& args) {
    auto* sim = get_sim(L);
    auto* ih = get_input_handler(L);
    if (!sim || !ih) {
        spdlog::info("No session");
        return;
    }
    if (args.size() < 2) return;
    const std::vector<u32> ids(ih->selected().begin(), ih->selected().end());
    const std::string& order = args[1];
    if (iequal(order, "Stop")) {
        issue_targetless_order(L, ids, "Stop", 0, true);
        ih->set_selected(ih->selected());
    } else if (iequal(order, "Dive")) {
        issue_targetless_order(L, ids, "Dive", 0, true);
    } else if (iequal(order, "SiloBuildTactical")) {
        issue_targetless_order(L, ids, "BuildSiloTactical", 0, false);
    } else if (iequal(order, "SiloBuildNuke")) {
        issue_targetless_order(L, ids, "BuildSiloNuke", 0, false);
    } else if (iequal(order, "Pause")) {
        spdlog::info("IssueCommand Pause: the engine has no pause order yet");
    }
}

/// A console line in the player's language (Moho's Loc and CON_Printf)
void print_loc(lua_State* L, const char* token) {
    lua_pushstring(L, "__osc_loc_cache");
    lua_rawget(L, LUA_REGISTRYINDEX);
    auto* loc = static_cast<core::Localization*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    spdlog::info("{}", loc ? loc->lookup(token) : std::string(token));
}

/// The selection's units still there, by id
std::vector<sim::Unit*> selected_units(sim::SimState& sim, const renderer::InputHandler& ih) {
    std::vector<u32> ids(ih.selected().begin(), ih.selected().end());
    std::sort(ids.begin(), ids.end());
    std::vector<sim::Unit*> units;
    for (const u32 id : ids) {
        auto* e = sim.entity_registry().find(id);
        if (e && e->is_unit() && !e->destroyed()) units.push_back(static_cast<sim::Unit*>(e));
    }
    return units;
}

/// UI_ExpandCurrentSelection: every unit the player may select whose
/// blueprint one in the selection has, walls and units being upgraded
/// left out (Moho's ExpandCurrentSelection).
void expand_current_selection(lua_State* L, const std::vector<std::string>&) {
    auto* sim = get_sim(L);
    auto* ih = get_input_handler(L);
    if (!sim || !ih) {
        print_loc(L, "<LOC _No_session>");
        return;
    }
    std::unordered_set<std::string> blueprints;
    for (const sim::Unit* u : selected_units(*sim, *ih)) blueprints.insert(u->blueprint_id());
    std::unordered_set<u32> expanded = ih->selected();
    static const sim::CategoryName kWall{"WALL"};
    sim->entity_registry().for_each_unit([&](sim::Entity& e) {
        const auto& u = static_cast<const sim::Unit&>(e);
        if (!renderer::selectable(u) || u.army() != ih->player_army()) return;
        if (u.has_category(kWall) || blueprints.count(u.blueprint_id()) == 0) return;
        if (u.has_unit_state("BeingUpgraded")) return;
        expanded.insert(u.entity_id());
    });
    ih->set_selected(expanded);
}

/// UI_ShowRenameDialog: retail's rename dialog for the one unit selected
void show_rename_dialog(lua_State* L, const std::vector<std::string>&) {
    auto* sim = get_sim(L);
    auto* ih = get_input_handler(L);
    if (!sim || !ih) {
        print_loc(L, "<LOC _No_session>");
        return;
    }
    // The selection's live units: Moho's selection lets its dead go
    const auto units = selected_units(*sim, *ih);
    if (units.empty()) {
        print_loc(L, "<LOC Engine0024>You must have a unit selected to rename.");
        return;
    }
    if (units.size() > 1) {
        print_loc(L, "<LOC Engine0025>You may only name one unit at a time, please limit your "
                     "selection to one unit.");
        return;
    }
    const int top = lua_gettop(L);
    lua_getglobal(L, "import");
    lua_pushstring(L, "/lua/ui/game/rename.lua");
    if (lua_pcall(L, 1, 1, 0) == 0 && lua_istable(L, -1)) {
        lua_pushstring(L, "ShowRenameDialog");
        lua_gettable(L, -2);
        lua_pushstring(L, units.front()->custom_name().c_str());
        if (lua_pcall(L, 1, 0, 0) != 0)
            spdlog::warn("UI_ShowRenameDialog: {}", lua_tostring(L, -1) ? lua_tostring(L, -1) : "");
    } else {
        spdlog::warn("UI_ShowRenameDialog: {}", lua_tostring(L, -1) ? lua_tostring(L, -1) : "");
    }
    lua_settop(L, top);
}

/// RenameUnit [name ...]: name the one unit selected (its words joined by
/// single spaces, trimmed), or with no name say what it is called
void rename_unit(lua_State* L, const std::vector<std::string>& args) {
    auto* sim = get_sim(L);
    auto* ih = get_input_handler(L);
    if (!sim || !ih) {
        print_loc(L, "<LOC _No_session>");
        return;
    }
    const auto units = selected_units(*sim, *ih);
    if (units.empty()) {
        print_loc(L, "<LOC Engine0021>You must have a unit selected to name it.");
        return;
    }
    if (units.size() > 1) {
        print_loc(L, "<LOC Engine0022>Naming a unit requires you only have one unit selected.");
        return;
    }
    sim::Unit* unit = units.front();
    if (args.size() == 1) {
        if (unit->custom_name().empty()) print_loc(L, "<LOC Engine0023>Unit has no custom name");
        else spdlog::info("Unit name: {}", unit->custom_name());
        return;
    }
    std::string name;
    for (size_t i = 1; i < args.size(); ++i) {
        if (!name.empty()) name += ' ';
        name += args[i];
    }
    const size_t first = name.find_first_not_of(" \t\r\n");
    const size_t last = name.find_last_not_of(" \t\r\n");
    name = first == std::string::npos ? std::string() : name.substr(first, last - first + 1);
    // Through the session as a unit's SetCustomName goes (Moho's
    // ProcessInfoPair "CustomName")
    const int top = lua_gettop(L);
    push_unit_for_ui(L, unit);
    lua_pushstring(L, "SetCustomName");
    lua_gettable(L, -2);
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, -2);
        lua_pushstring(L, name.c_str());
        if (lua_pcall(L, 2, 0, 0) != 0)
            spdlog::warn("RenameUnit: {}", lua_tostring(L, -1) ? lua_tostring(L, -1) : "");
    }
    lua_settop(L, top);
}

/// UI_TrackUnit <camera> [camera ...]: each named camera follows the
/// selection, or lets go if it has none or already follows the first of it.
/// A camera no view made is passed over, as Moho passes over one it can't find.
void track_unit(lua_State* L, const std::vector<std::string>& args) {
    auto* sim = get_sim(L);
    auto* ih = get_input_handler(L);
    if (!sim || !ih) {
        print_loc(L, "<LOC _No_session>");
        return;
    }
    auto* r = get_renderer(L);
    if (!r) return;
    for (size_t i = 1; i < args.size(); ++i) {
        renderer::Camera* found = r->cameras().find(args[i]);
        if (!found) {
            continue;
        }
        renderer::Camera& camera = *found;
        const auto units = selected_units(*sim, *ih);
        if (units.empty() || units.front()->entity_id() == camera.target_entity()) {
            camera.target_nothing();
            continue;
        }
        std::vector<u32> ids;
        ids.reserve(units.size());
        for (const sim::Unit* u : units) ids.push_back(u->entity_id());
        camera.target_entities(std::move(ids), true, camera.zoom(), 0.0f);
    }
}

} // namespace

void update_engine_stats(lua_State* L) {
    sim::Vector3 at;
    if (cursor_world_pos(L, at) && !std::isnan(at.x) && !std::isnan(at.z))
        update_cursor_stats(L, at);
}

void update_cursor_stats(lua_State* L, const sim::Vector3& at) {
    const auto* sim = get_sim(L);
    const map::Terrain* terrain = sim ? sim->terrain() : nullptr;
    if (!terrain) return;
    lua_pushstring(L, "__EngineStats");
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    push_stat_child(L, "Camera");
    push_stat_child(L, "Cursor");
    const std::string position = fmt::format("x={:.2f},y={:.2f},z={:.2f}", at.x, at.y, at.z);
    set_stat(L, "Position", "String", [&] { lua_pushstring(L, position.c_str()); });
    set_stat(L, "Elevation", "Float",
             [&] { lua_pushnumber(L, terrain->get_terrain_height(at.x, at.z)); });
    const std::string cell =
        fmt::format("x={},z={}", static_cast<int>(at.x), static_cast<int>(at.z));
    set_stat(L, "OCell", "String", [&] { lua_pushstring(L, cell.c_str()); });
    lua_pop(L, 3); // Cursor, Camera, __EngineStats
}

void register_session_console_commands(ui::Console& console) {
    console.add("StartCommandMode", start_command_mode);
    console.add("UI_SelectByCategory", select_by_category);
    console.add("IssueCommand", issue_command);
    console.add("UI_ExpandCurrentSelection", expand_current_selection);
    console.add("UI_ShowRenameDialog", show_rename_dialog);
    console.add("RenameUnit", rename_unit);
    console.add("UI_TrackUnit", track_unit);
}

} // namespace osc::lua
