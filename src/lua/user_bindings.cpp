// The UI state's bindings that reach the game's view and input: the camera,
// the world view's projection, selection, the mouse and keys, map previews
// and the UI's own orders (M191). They live apart from the Lua library, which
// the sim uses, so that library needs no renderer: osc_lua_user links both.

#include "lua/user_bindings.hpp"

#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "lua/moho_bindings_internal.hpp"
#include "lua/order_helpers.hpp"
#include "map/scmap_parser.hpp"
#include "map/terrain.hpp"
#include "renderer/input_handler.hpp"
#include "renderer/frustum.hpp"
#include "renderer/renderer.hpp"
#include "renderer/terrain_preview.hpp"
#include "sim/entity_registry.hpp"
#include "sim/category_expr.hpp"
#include "sim/sim_callback_queue.hpp"
#include "sim/script_class.hpp"
#include "sim/sim_state.hpp"
#include "sim/thread_manager.hpp"
#include "sim/unit.hpp"
#include "sim/unit_command.hpp"
#include "sim/waitable.hpp"
#include "ui/console.hpp"
#include "ui/ui_control.hpp"
#include "ui/world_view.hpp"
#include "ui/wld_ui_provider.hpp"
#include "vfs/virtual_file_system.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_set>
#include <optional>
#include <cctype>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <memory>
#include <string>
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

/// __init(self, parentControl, cameraName, depth, isMiniMap, trackCamera)
/// Bind a WorldView to the renderer's camera, viewport and the terrain (for
/// raycasts). A main (non-minimap) view becomes the one GetMouseWorldPos and
/// GetCamera use. Done at construction: retail's WorldView class overrides
/// Register in Lua without calling the engine's.
static void bind_world_view(lua_State* L, ui::WorldView* wv) {
    auto* r = get_renderer(L);
    if (r) {
        wv->set_renderer(r);
        wv->register_camera("WorldCamera", &r->camera());
        wv->set_viewport(r->width(), r->height());
    }
    auto* sim = get_sim(L);
    if (sim && sim->terrain()) wv->set_terrain(sim->terrain());
    if (!wv->is_minimap()) {
        lua_pushstring(L, "__osc_world_view");
        lua_pushlightuserdata(L, wv);
        lua_rawset(L, LUA_REGISTRYINDEX);
    }
    spdlog::debug("WorldView bound: camera='{}', viewport={}x{}", wv->camera_name(),
                  wv->viewport_width(), wv->viewport_height());
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
            wv_ptr->set_parent(parent); // set_parent already calls add_child
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

    bind_world_view(L, wv_ptr);
    spdlog::debug("UIWorldView.__init: WorldView control #{}, camera='{}'", id, cam_name);
    return 0;
}

static int worldview_Project(lua_State* L) {
    auto* wv = check_world_view(L);
    if (!wv || !wv->camera()) {
        // Fallback: return {x=0, y=0}
        lua_newtable(L);
        lua_pushstring(L, "x");
        lua_pushnumber(L, 0);
        lua_rawset(L, -3);
        lua_pushstring(L, "y");
        lua_pushnumber(L, 0);
        lua_rawset(L, -3);
        return 1;
    }

    // Update viewport from renderer if available
    auto* r = get_renderer(L);
    if (r) wv->set_viewport(r->width(), r->height());

    // Args: self, Vector {x,y,z}
    f32 wx = 0, wy = 0, wz = 0;
    if (lua_istable(L, 2)) {
        lua_pushstring(L, "x");
        lua_rawget(L, 2);
        if (lua_isnumber(L, -1)) wx = static_cast<f32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        lua_pushstring(L, "y");
        lua_rawget(L, 2);
        if (lua_isnumber(L, -1)) wy = static_cast<f32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        lua_pushstring(L, "z");
        lua_rawget(L, 2);
        if (lua_isnumber(L, -1)) wz = static_cast<f32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
    }

    f32 sx = 0, sy = 0;
    wv->project(wx, wy, wz, sx, sy);

    lua_newtable(L);
    lua_pushstring(L, "x");
    lua_pushnumber(L, static_cast<lua_Number>(sx));
    lua_rawset(L, -3);
    lua_pushstring(L, "y");
    lua_pushnumber(L, static_cast<lua_Number>(sy));
    lua_rawset(L, -3);
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
    if (wv) bind_world_view(L, wv);
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

/// camera:SaveSettings() -> {Focus, Zoom, Pitch, Heading} (Moho's: the
/// focus, the target zoom, the pitch and heading)
static int camera_SaveSettings(lua_State* L) {
    auto* r = get_renderer(L);
    lua_newtable(L);
    if (!r) return 1;
    const auto& cam = r->camera();
    lua_pushstring(L, "Focus");
    push_vec3(L, cam.focus_x(), cam.focus_y(), cam.focus_z());
    lua_rawset(L, -3);
    lua_pushstring(L, "Zoom");
    lua_pushnumber(L, cam.zoom());
    lua_rawset(L, -3);
    lua_pushstring(L, "Pitch");
    lua_pushnumber(L, cam.pitch());
    lua_rawset(L, -3);
    lua_pushstring(L, "Heading");
    lua_pushnumber(L, cam.heading());
    lua_rawset(L, -3);
    return 1;
}

/// camera:RestoreSettings(settings): TargetManual to them at once, then the
/// rotation reverts (the pitch glides back to the zoom's)
static int camera_RestoreSettings(lua_State* L) {
    if (!lua_istable(L, 2)) return 0;
    auto* r = get_renderer(L);
    if (!r) return 0;
    auto& cam = r->camera();
    f32 focus[3] = {cam.focus_x(), cam.focus_y(), cam.focus_z()};
    lua_pushstring(L, "Focus");
    lua_rawget(L, 2);
    (void)read_vec3(L, lua_gettop(L), focus);
    lua_pop(L, 1);
    cam.target_manual(focus[0], focus[1], focus[2], field_number(L, 2, "Heading", cam.heading()),
                      field_number(L, 2, "Pitch", cam.pitch()),
                      field_number(L, 2, "Zoom", cam.zoom()));
    cam.revert_rotation();
    return 0;
}

/// camera:SetZoom(zoom, seconds): TargetManual at the target, heading and
/// pitch, over the seconds
static int camera_SetZoom(lua_State* L) {
    auto* r = get_renderer(L);
    if (!r) return 0;
    auto& cam = r->camera();
    cam.target_manual(cam.target_x(), cam.target_y(), cam.target_z(), cam.heading(), cam.pitch(),
                      static_cast<f32>(luaL_checknumber(L, 2)),
                      static_cast<f32>(lua_tonumber(L, 3)));
    return 0;
}

/// camera:GetZoom(): the target zoom (CameraGetTargetZoom)
static int camera_GetZoom(lua_State* L) {
    auto* r = get_renderer(L);
    lua_pushnumber(L, r ? r->camera().zoom() : kHeadlessMaxZoom);
    return 1;
}

/// camera:SetMaxZoomMult(mult) -- scales the far zoom limit.
static int camera_SetMaxZoomMult(lua_State* L) {
    auto* r = get_renderer(L);
    if (r) r->camera().set_max_zoom_mult(static_cast<f32>(luaL_checknumber(L, 2)));
    return 0;
}

/// camera:GetMinZoom(): cam_NearZoom
static int camera_GetMinZoom(lua_State* L) {
    lua_pushnumber(L, renderer::Camera::kNearZoom);
    return 1;
}

static int camera_GetMaxZoom(lua_State* L) {
    auto* r = get_renderer(L);
    lua_pushnumber(L, r ? r->camera().max_zoom() : kHeadlessMaxZoom);
    return 1;
}

/// camera:SetTargetZoom(zoom): the zoom the view glides toward (mNearZoom)
static int camera_SetTargetZoom(lua_State* L) {
    auto* r = get_renderer(L);
    if (r) r->camera().set_requested_zoom(static_cast<f32>(luaL_checknumber(L, 2)));
    return 0;
}

/// camera:GetTargetZoom(): the zoom the view glides toward
static int camera_GetTargetZoom(lua_State* L) {
    auto* r = get_renderer(L);
    lua_pushnumber(L, r ? r->camera().requested_zoom() : kHeadlessMaxZoom);
    return 1;
}

static int camera_Reset(lua_State* L) {
    auto* r = get_renderer(L);
    if (r) r->camera().reset();
    return 0;
}

/// camera:MoveTo(position, orientationHPR, zoom, seconds) and
/// camera:SnapTo(position, orientationHPR, zoom): TargetManual, heading and
/// pitch from the orientation, over the seconds (SnapTo's none)
static int camera_MoveTo(lua_State* L) {
    auto* r = get_renderer(L);
    if (!r) return 0;
    auto& cam = r->camera();
    f32 pos[3] = {cam.target_x(), cam.focus_y(), cam.target_z()};
    (void)read_vec3(L, 2, pos);
    f32 hpr[3] = {cam.heading(), cam.pitch(), 0.0f};
    (void)read_vec3(L, 3, hpr);
    const f32 zoom = lua_isnumber(L, 4) ? static_cast<f32>(lua_tonumber(L, 4)) : cam.zoom();
    cam.target_manual(pos[0], pos[1], pos[2], hpr[0], hpr[1], zoom,
                      static_cast<f32>(lua_tonumber(L, 5)));
    return 0;
}

/// camera:MoveToRegion(rect, [seconds]): TargetBox over the rect ({x0, y0,
/// x1, y1} or a Rect's fields, y being map z), its corners on their cells'
/// middles at the ground's height there (cfunc_CameraImplMoveToRegionL)
static int camera_MoveToRegion(lua_State* L) {
    auto* r = get_renderer(L);
    if (!r || !lua_istable(L, 2)) return 0;
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
    r->camera().target_box({x0, height(x0, z0), z0}, {x1, height(x1, z1), z1},
                           static_cast<f32>(lua_tonumber(L, 3)));
    return 0;
}

/// A table of entity ids (strings, as retail passes them, or numbers), or a
/// single one.
static std::vector<u32> read_entity_ids(lua_State* L, int idx) {
    std::vector<u32> ids;
    const auto one = [&](int at) {
        if (lua_type(L, at) == LUA_TSTRING || lua_type(L, at) == LUA_TNUMBER)
            ids.push_back(static_cast<u32>(std::strtoul(lua_tostring(L, at), nullptr, 10)));
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
    auto* r = get_renderer(L);
    lua_pushstring(L, "_c_object");
    lua_rawget(L, 1);
    const bool camera = r && lua_touserdata(L, -1) == r;
    lua_pop(L, 1);
    lua_pushstring(L, "__osc_ui_thread_manager");
    lua_rawget(L, LUA_REGISTRYINDEX);
    auto* threads = static_cast<sim::ThreadManager*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    if (!camera || !threads || r->camera().signaled()) return 0;
    // The thread manager keeps the waitable only while the thread waits;
    // the camera's waiter keeps it alive until the wake
    auto wait = std::make_shared<CameraWait>();
    r->camera().on_signal([wait, threads] {
        wait->done = true;
        threads->wake(*wait, 0); // only the thread that waited, if it still lives
    });
    lua_pushlightuserdata(L, static_cast<sim::Waitable*>(wait.get()));
    return lua_yield(L, 1);
}

/// camera:TargetEntities(ids, zoom, seconds): to the first, not followed
static int camera_TargetEntities(lua_State* L) {
    if (auto* r = get_renderer(L)) {
        r->camera().target_entities(read_entity_ids(L, 2), false,
                                    static_cast<f32>(lua_tonumber(L, 3)),
                                    static_cast<f32>(lua_tonumber(L, 4)));
    }
    return 0;
}

/// camera:TrackEntities(ids, zoom, seconds): to the first, followed
static int camera_TrackEntities(lua_State* L) {
    if (auto* r = get_renderer(L)) {
        r->camera().target_entities(read_entity_ids(L, 2), true,
                                    static_cast<f32>(lua_tonumber(L, 3)),
                                    static_cast<f32>(lua_tonumber(L, 4)));
    }
    return 0;
}

/// camera:NoseCam(id, pitchAdjust, zoom, seconds, transition)
static int camera_NoseCam(lua_State* L) {
    if (auto* r = get_renderer(L)) {
        r->camera().target_nose_cam(read_entity_ids(L, 2), static_cast<f32>(lua_tonumber(L, 3)),
                                    static_cast<f32>(lua_tonumber(L, 4)),
                                    static_cast<f32>(lua_tonumber(L, 5)),
                                    static_cast<f32>(lua_tonumber(L, 6)));
    }
    return 0;
}

/// camera:Spin(headingRate, [zoomRate]): revolutions and zoom a second
static int camera_Spin(lua_State* L) {
    if (auto* r = get_renderer(L)) {
        r->camera().spin_rates(static_cast<f32>(lua_tonumber(L, 2)),
                               static_cast<f32>(lua_tonumber(L, 3)));
    }
    return 0;
}

/// camera:SetAccMode(name): Linear, FastInSlowOut or SlowInOut
static int camera_SetAccMode(lua_State* L) {
    auto* r = get_renderer(L);
    if (r && lua_type(L, 2) == LUA_TSTRING) (void)r->camera().set_acc_mode(lua_tostring(L, 2));
    return 0;
}

static int camera_EnableEaseInOut(lua_State* L) {
    if (auto* r = get_renderer(L)) r->camera().set_ease_in_out(true);
    return 0;
}

static int camera_DisableEaseInOut(lua_State* L) {
    if (auto* r = get_renderer(L)) r->camera().set_ease_in_out(false);
    return 0;
}

static int camera_HoldRotation(lua_State* L) {
    if (auto* r = get_renderer(L)) r->camera().hold_rotation();
    return 0;
}

static int camera_RevertRotation(lua_State* L) {
    if (auto* r = get_renderer(L)) r->camera().revert_rotation();
    return 0;
}

static int camera_UseGameClock(lua_State* L) {
    if (auto* r = get_renderer(L)) r->camera().use_game_clock(true);
    return 0;
}

static int camera_UseSystemClock(lua_State* L) {
    if (auto* r = get_renderer(L)) r->camera().use_game_clock(false);
    return 0;
}

/// camera:GetFocusPosition() -> the point the camera looks at (its focus,
/// on the ground or the water's surface)
static int camera_GetFocusPosition(lua_State* L) {
    auto* r = get_renderer(L);
    if (!r) {
        push_vec3(L, 512.0f, 0.0f, 512.0f);
        return 1;
    }
    const auto& cam = r->camera();
    push_vec3(L, cam.focus_x(), cam.focus_y(), cam.focus_z());
    return 1;
}

static int camera_GetHeading(lua_State* L) {
    auto* r = get_renderer(L);
    lua_pushnumber(L, r ? r->camera().heading() : renderer::Camera::kPi);
    return 1;
}

static int camera_GetPitch(lua_State* L) {
    auto* r = get_renderer(L);
    lua_pushnumber(L, r ? r->camera().pitch()
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
    if (auto* r = get_renderer(L); r && wv) wv->set_viewport(r->width(), r->height());
    sim::Vector3 at{};
    if (!wv || !wv->camera() || !wv->get_mouse_world_pos(sx, sy, at.x, at.y, at.z)) {
        const f32 nan = std::numeric_limits<f32>::quiet_NaN();
        at = {nan, nan, nan};
    }
    push_vector3(L, at);
    return 1;
}

// --- GetMouseWorldPos global (M136a) ---
// FA calls this as a standalone function: GetMouseWorldPos()
static int l_GetMouseWorldPos(lua_State* L) {
    // Get WorldView from registry
    lua_pushstring(L, "__osc_world_view");
    lua_rawget(L, LUA_REGISTRYINDEX);
    auto* wv = static_cast<ui::WorldView*>(lua_touserdata(L, -1));
    lua_pop(L, 1);

    if (!wv || !wv->camera()) {
        // Return zero vector
        lua_newtable(L);
        lua_pushstring(L, "x");
        lua_pushnumber(L, 0);
        lua_rawset(L, -3);
        lua_pushstring(L, "y");
        lua_pushnumber(L, 0);
        lua_rawset(L, -3);
        lua_pushstring(L, "z");
        lua_pushnumber(L, 0);
        lua_rawset(L, -3);
        return 1;
    }

    // Get mouse position from renderer
    auto* r = get_renderer(L);
    f32 mx = 0, my = 0;
    if (r) {
        f64 dx = 0, dy = 0;
        r->mouse_position(dx, dy);
        mx = static_cast<f32>(dx);
        my = static_cast<f32>(dy);
        wv->set_viewport(r->width(), r->height());
    }

    f32 wx = 0, wy = 0, wz = 0;
    wv->get_mouse_world_pos(mx, my, wx, wy, wz);

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

// --- SyncPlayableRect global (M209) ---
/// SyncPlayableRect(rect): the user side of the sim's playable rect (Moho's
/// CWldSession::SyncPlayableRect), which usercamera.lua applies from the
/// campaign's CAMERA_SYNC_PLAYABLE_RECT request. Kept to the map as the
/// sim's is, the camera keeps to it, and the meshes outside it now hide
/// until the next.
static int l_SyncPlayableRect(lua_State* L) {
    if (lua_gettop(L) != 1)
        return luaL_error(L, "SyncPlayableRect(rect)\n  expected 1 args, but got %d",
                          lua_gettop(L));
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
        return luaL_error(L, "RenderOverlayEconomy(bool)\n  expected 1 args, but got %d",
                          lua_gettop(L));
    if (auto* r = get_renderer(L)) r->set_economy_overlay(lua_toboolean(L, 1) != 0);
    return 0;
}

// --- GetCamera global (M136a) ---
// FA calls GetCamera(cameraName) and gets back a camera object with methods.
static int l_GetCamera(lua_State* L) {
    // Create a table with camera_methods metatable
    lua_newtable(L);

    // Store renderer pointer as _c_object for camera methods to use
    auto* r = get_renderer(L);
    if (r) {
        lua_pushstring(L, "_c_object");
        lua_pushlightuserdata(L, r);
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

static int l_SelectUnits(lua_State* L) {
    auto* ih = get_input_handler(L);
    auto* sim = get_sim(L);
    if (!ih || !lua_istable(L, 1)) return 0;

    std::unordered_set<u32> new_sel;
    int n = luaL_getn(L, 1); // Lua 5.0: no lua_objlen
    for (int i = 1; i <= n; i++) {
        lua_rawgeti(L, 1, i);
        if (lua_istable(L, -1)) {
            lua_pushstring(L, "EntityId");
            lua_rawget(L, -2);
            if (lua_isnumber(L, -1)) {
                auto eid = static_cast<u32>(lua_tonumber(L, -1));
                if (sim) {
                    auto* entity = sim->entity_registry().find(eid);
                    if (entity && entity->is_unit() && !entity->destroyed()) {
                        new_sel.insert(eid);
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
                auto eid = static_cast<u32>(lua_tonumber(L, -1));
                if (sim) {
                    auto* entity = sim->entity_registry().find(eid);
                    if (entity && entity->is_unit() && !entity->destroyed()) {
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

    // Read Args field (table of key→value)
    lua_pushstring(L, "Args");
    lua_rawget(L, 1);
    if (lua_istable(L, -1)) {
        int args_tbl = lua_gettop(L);
        lua_pushnil(L); // first key
        while (lua_next(L, args_tbl) != 0) {
            // key at -2, value at -1
            if (lua_type(L, -2) == LUA_TSTRING) {
                const char* key = lua_tostring(L, -2);
                std::string key_str(key);
                int vtype = lua_type(L, -1);
                if (vtype == LUA_TSTRING) {
                    entry.args[key_str] = std::string(lua_tostring(L, -1));
                } else if (vtype == LUA_TNUMBER) {
                    entry.args[key_str] = static_cast<f64>(lua_tonumber(L, -1));
                } else if (vtype == LUA_TBOOLEAN) {
                    entry.args[key_str] = lua_toboolean(L, -1) != 0;
                }
                // Other types (tables, functions, etc.) are silently skipped
            }
            lua_pop(L, 1); // pop value, keep key for next iteration
        }
    } else if (lua_type(L, -1) == LUA_TSTRING) {
        entry.value = std::string(lua_tostring(L, -1));
    } else if (lua_type(L, -1) == LUA_TNUMBER) {
        entry.value = static_cast<f64>(lua_tonumber(L, -1));
    } else if (lua_type(L, -1) == LUA_TBOOLEAN) {
        entry.value = lua_toboolean(L, -1) != 0;
    }
    lua_pop(L, 1); // pop Args (a table, a value, or nil)

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
    registry.for_each_unit([&](sim::Entity& e) {
        if (e.destroyed() || !e.is_unit() || e.army() != focus) return;
        const auto& pad = static_cast<const sim::Unit&>(e);
        if (pad.is_being_built() || pad.is_dying() || !pad.has_category(kStaging)) return;
        if (pad.layer() == "Sub" || pad.layer() == "Seabed" || !pad.command_queue().empty()) return;
        const i32 room = pad.storage_slots() != 0
                             ? pad.storage_slots() - static_cast<i32>(pad.stored_ids().size())
                             : pad.docking_slots();
        if (room <= 0) return;
        const f32 dx = pad.position().x - cx;
        const f32 dz = pad.position().z - cz;
        pads.push_back({&pad, dz * dz + dx * dx, room});
    });
    if (pads.empty()) return 0;
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

/// IsKeyDown(keyCode) — checks if a GLFW key is currently pressed.
static int l_IsKeyDown(lua_State* L) {
    auto* r = get_renderer(L);
    if (!r) {
        lua_pushboolean(L, 0);
        return 1;
    }

    // FA key codes map to GLFW key codes for most keys
    int key_code = static_cast<int>(luaL_checknumber(L, 1));
    lua_pushboolean(L, r->is_key_pressed(key_code) ? 1 : 0);
    return 1;
}

/// GetRolloverInfo() — returns a rich table describing the hovered or first-selected unit (M142a).
/// Checks __osc_hover_entity_id registry key first; falls back to first selected unit.
static int l_GetRolloverInfo(lua_State* L) {
    auto* sim = get_sim(L);
    if (!sim) {
        lua_pushnil(L);
        return 1;
    }

    sim::Unit* unit = nullptr;

    // 1. Try hover entity from WorldView HitTest
    lua_pushstring(L, "__osc_hover_entity_id");
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (lua_isnumber(L, -1)) {
        u32 hover_id = static_cast<u32>(lua_tonumber(L, -1));
        if (hover_id != 0) {
            auto* entity = sim->entity_registry().find(hover_id);
            if (entity && entity->is_unit() && !entity->destroyed())
                unit = static_cast<sim::Unit*>(entity);
        }
    }
    lua_pop(L, 1);

    // 2. Fall back to first selected unit
    if (!unit) {
        auto* input = get_input_handler(L);
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

    // Fuel: -1 for units without any, which hides the unit view's fuel line
    // (it multiplies Physics.FuelUseTime by the ratio). Fuel use is not
    // simulated yet, so fuelled units report a full tank.
    lua_Number fuel_ratio = -1;
    if (push_entity_blueprint(L, unit)) {
        lua_pushstring(L, "Physics");
        lua_rawget(L, -2);
        if (lua_istable(L, -1)) {
            lua_pushstring(L, "FuelUseTime");
            lua_rawget(L, -2);
            if (lua_isnumber(L, -1) && lua_tonumber(L, -1) > 0) fuel_ratio = 1;
            lua_pop(L, 1);
        }
        lua_pop(L, 2); // Physics + blueprint
    }

    lua_newtable(L); // result table

    set_str("blueprintId", unit->blueprint_id().c_str());
    lua_pushstring(L, "entityId");
    sim::push_entity_id(L, unit->entity_id());
    lua_rawset(L, -3);
    set_num("health", static_cast<lua_Number>(unit->health()));
    set_num("maxHealth", static_cast<lua_Number>(unit->max_health()));
    set_num("kills", 0);
    set_num("armyIndex", static_cast<lua_Number>(unit->army()));
    set_num("workProgress", static_cast<lua_Number>(unit->work_progress()));
    set_num("shieldRatio", static_cast<lua_Number>(unit->shield_ratio()));
    set_num("fuelRatio", fuel_ratio);

    const auto& econ = unit->economy();
    set_num("massProduced", static_cast<lua_Number>(econ.production_mass));
    set_num("massConsumed", static_cast<lua_Number>(econ.consumption_mass));
    set_num("energyProduced", static_cast<lua_Number>(econ.production_energy));
    set_num("energyConsumed", static_cast<lua_Number>(econ.consumption_energy));
    set_num("massRequested", static_cast<lua_Number>(econ.consumption_mass));
    set_num("energyRequested", static_cast<lua_Number>(econ.consumption_energy));

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

    // focus: if unit is building something, include a sub-table for the target
    u32 focus_id = unit->build_target_id();
    if (focus_id != 0) {
        auto* focus_entity = sim->entity_registry().find(focus_id);
        if (focus_entity && focus_entity->is_unit() && !focus_entity->destroyed()) {
            auto* focus_unit = static_cast<sim::Unit*>(focus_entity);
            lua_pushstring(L, "focus");
            lua_newtable(L); // focus sub-table
            lua_pushstring(L, "blueprintId");
            lua_pushstring(L, focus_unit->blueprint_id().c_str());
            lua_rawset(L, -3);
            lua_pushstring(L, "entityId");
            sim::push_entity_id(L, focus_unit->entity_id());
            lua_rawset(L, -3);
            lua_rawset(L, -3); // result["focus"] = focus_sub_table
        }
    }

    return 1;
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
    state.register_function("InternalCreateWldUIProvider", l_InternalCreateWldUIProvider);
    state.register_function("GetMouseWorldPos", l_GetMouseWorldPos);
    state.register_function("UnProject", l_UnProject);
    state.register_function("GetCamera", l_GetCamera);
    state.register_function("SyncPlayableRect", l_SyncPlayableRect);
    state.register_function("RenderOverlayEconomy", l_RenderOverlayEconomy);
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
    state.register_function("GetValidAttackingUnits", l_GetValidAttackingUnits);
    state.register_function("GetRolloverInfo", l_GetRolloverInfo);

    // Methods of the classes register_moho_bindings made: before any UI
    // script builds its classes from them.
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
    f32 mx = 0, my = 0;
    if (auto* r = get_renderer(L)) {
        f64 dx = 0, dy = 0;
        r->mouse_position(dx, dy);
        mx = static_cast<f32>(dx);
        my = static_cast<f32>(dy);
        wv->set_viewport(r->width(), r->height());
    }
    return wv->get_mouse_world_pos(mx, my, out.x, out.y, out.z);
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
        if (lua_pcall(L, 0, 1, 0) == 0 && lua_istable(L, -1)) {
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

/// UI_SelectByCategory [+add] [+nearest] [+idle] [+inview] [+goto]
/// [+excludeengineers] expression (Moho's SelectUnitsByCategory): the focus
/// army's selectable units in the category expression ("A B, C": spaces
/// intersect, commas unite), filtered, as the selection.
void select_by_category(lua_State* L, const std::vector<std::string>& args) {
    auto* sim = get_sim(L);
    auto* ih = get_input_handler(L);
    if (!sim || !ih) {
        spdlog::info("No session");
        return;
    }
    if (args.size() < 2) {
        spdlog::info("UI_SelectByCategory [+add] [+nearest] [+idle] [+goto] categoryExpression");
        return;
    }
    bool add = false, in_view = false, nearest = false, idle = false, go_to = false,
         exclude_engineers = false;
    std::string expression;
    for (size_t i = 1; i < args.size(); ++i) {
        const std::string& token = args[i];
        if (!token.empty() && token[0] == '+') {
            if (iequal(token, "+add")) add = true;
            else if (iequal(token, "+nearest")) nearest = true;
            else if (iequal(token, "+idle")) idle = true;
            else if (iequal(token, "+inview")) in_view = true;
            else if (iequal(token, "+goto")) go_to = true;
            else if (iequal(token, "+excludeengineers")) exclude_engineers = true;
            else spdlog::info("Unknown modifier {}", token);
            continue;
        }
        if (!expression.empty()) expression += ' ';
        expression += token;
    }
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
        if (u.destroyed() || u.unselectable() || u.army() != ih->player_army()) return;
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

} // namespace

void register_session_console_commands(ui::Console& console) {
    console.add("StartCommandMode", start_command_mode);
    console.add("UI_SelectByCategory", select_by_category);
    console.add("IssueCommand", issue_command);
}

} // namespace osc::lua
