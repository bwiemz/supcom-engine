// The UI state's bindings that reach the game's view and input: the camera,
// the world view's projection, selection, the mouse and keys, map previews
// and the UI's own orders (M191). They live apart from the Lua library, which
// the sim uses, so that library needs no renderer: osc_lua_user links both.

#include "lua/user_bindings.hpp"

#include "lua/lua_state.hpp"
#include "lua/moho_bindings_internal.hpp"
#include "lua/order_helpers.hpp"
#include "map/scmap_parser.hpp"
#include "map/terrain.hpp"
#include "renderer/input_handler.hpp"
#include "renderer/renderer.hpp"
#include "renderer/terrain_preview.hpp"
#include "sim/entity_registry.hpp"
#include "sim/sim_callback_queue.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/unit_command.hpp"
#include "ui/ui_control.hpp"
#include "ui/world_view.hpp"
#include "ui/wld_ui_provider.hpp"
#include "vfs/virtual_file_system.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <initializer_list>
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
constexpr f32 kHeadlessMaxZoom = 1536.0f; // 1024 map x 1.5

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

    // Control.OnInit: MAUI's default layout, and Depth = parent's + 1.
    lua_pushstring(L, "OnInit");
    lua_gettable(L, 1);
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, 1);
        if (lua_pcall(L, 1, 0, 0) != 0) {
            spdlog::warn("UIWorldView.__init: OnInit error: {}", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    } else {
        lua_pop(L, 1);
    }

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

/// camera:SaveSettings() -> table with camera state
static int camera_SaveSettings(lua_State* L) {
    auto* r = get_renderer(L);
    if (!r) {
        lua_newtable(L);
        return 1;
    }
    auto& cam = r->camera();
    lua_newtable(L);
    lua_pushstring(L, "target_x");
    lua_pushnumber(L, cam.target_x());
    lua_rawset(L, -3);
    lua_pushstring(L, "target_z");
    lua_pushnumber(L, cam.target_z());
    lua_rawset(L, -3);
    lua_pushstring(L, "distance");
    lua_pushnumber(L, cam.distance());
    lua_rawset(L, -3);
    lua_pushstring(L, "yaw");
    lua_pushnumber(L, cam.yaw());
    lua_rawset(L, -3);
    lua_pushstring(L, "pitch");
    lua_pushnumber(L, cam.pitch());
    lua_rawset(L, -3);
    return 1;
}

/// camera:RestoreSettings(settings) — self at index 1, settings at index 2
static int camera_RestoreSettings(lua_State* L) {
    if (!lua_istable(L, 2)) return 0;
    auto* r = get_renderer(L);
    if (!r) return 0;
    auto& cam = r->camera();

    lua_pushstring(L, "target_x");
    lua_rawget(L, 2);
    f32 tx = static_cast<f32>(lua_tonumber(L, -1));
    lua_pop(L, 1);
    lua_pushstring(L, "target_z");
    lua_rawget(L, 2);
    f32 tz = static_cast<f32>(lua_tonumber(L, -1));
    lua_pop(L, 1);
    lua_pushstring(L, "distance");
    lua_rawget(L, 2);
    f32 d = static_cast<f32>(lua_tonumber(L, -1));
    lua_pop(L, 1);
    lua_pushstring(L, "yaw");
    lua_rawget(L, 2);
    f32 yaw = static_cast<f32>(lua_tonumber(L, -1));
    lua_pop(L, 1);
    lua_pushstring(L, "pitch");
    lua_rawget(L, 2);
    f32 pitch = static_cast<f32>(lua_tonumber(L, -1));
    lua_pop(L, 1);

    cam.set_target(tx, tz);
    cam.set_distance(d);
    cam.set_yaw(yaw);
    cam.set_pitch(pitch);
    return 0;
}

static int camera_SetZoom(lua_State* L) {
    auto* r = get_renderer(L);
    if (!r) return 0;
    r->camera().set_zoom(static_cast<f32>(luaL_checknumber(L, 2)));
    return 0;
}

static int camera_GetZoom(lua_State* L) {
    auto* r = get_renderer(L);
    if (!r) {
        lua_pushnumber(L, 300);
        return 1;
    }
    lua_pushnumber(L, static_cast<lua_Number>(r->camera().distance()));
    return 1;
}

/// camera:SetMaxZoomMult(mult) -- scales the far zoom limit.
static int camera_SetMaxZoomMult(lua_State* L) {
    auto* r = get_renderer(L);
    if (r) r->camera().set_max_zoom_mult(static_cast<f32>(luaL_checknumber(L, 2)));
    return 0;
}

static int camera_GetMinZoom(lua_State* L) {
    auto* r = get_renderer(L);
    lua_pushnumber(L, r ? r->camera().min_zoom() : renderer::Camera::MIN_ZOOM);
    return 1;
}

static int camera_GetMaxZoom(lua_State* L) {
    auto* r = get_renderer(L);
    lua_pushnumber(L, r ? r->camera().max_zoom() : kHeadlessMaxZoom);
    return 1;
}

/// Zoom changes are immediate (no easing yet), so the target zoom is the
/// current one.
static int camera_SetTargetZoom(lua_State* L) {
    return camera_SetZoom(L);
}

static int camera_GetTargetZoom(lua_State* L) {
    return camera_GetZoom(L);
}

static int camera_Reset(lua_State* L) {
    auto* r = get_renderer(L);
    if (r) r->camera().reset();
    return 0;
}

/// camera:MoveTo(position, orientationHPR, zoom, seconds) and
/// camera:SnapTo(position, orientationHPR, zoom): placed at once (no
/// animation yet). Orientation is heading, pitch, roll in radians.
static int camera_MoveTo(lua_State* L) {
    auto* r = get_renderer(L);
    if (!r) return 0;
    auto& cam = r->camera();
    f32 x = 0, z = 0;
    if (read_xz(L, 2, x, z)) cam.set_target(x, z);
    if (lua_istable(L, 3)) {
        lua_rawgeti(L, 3, 1);
        lua_rawgeti(L, 3, 2);
        if (lua_isnumber(L, -2)) cam.set_yaw(static_cast<f32>(lua_tonumber(L, -2)));
        if (lua_isnumber(L, -1)) cam.set_pitch(static_cast<f32>(lua_tonumber(L, -1)));
        lua_pop(L, 2);
    }
    if (lua_isnumber(L, 4)) cam.set_zoom(static_cast<f32>(lua_tonumber(L, 4)));
    return 0;
}

/// camera:MoveToRegion(rect, seconds): centre on the rect ({x0, y0, x1, y1}
/// or a Rect with x0/y0/x1/y1 fields, y being map z) and zoom to fit it.
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
    auto& cam = r->camera();
    cam.set_target((v[0] + v[2]) * 0.5f, (v[1] + v[3]) * 0.5f);
    cam.set_zoom(std::max(std::abs(v[2] - v[0]), std::abs(v[3] - v[1])));
    return 0;
}

/// camera:GetFocusPosition() -> {x, y, z} of the point the camera looks at.
static int camera_GetFocusPosition(lua_State* L) {
    auto* r = get_renderer(L);
    f32 x = r ? r->camera().target_x() : 512.0f;
    f32 z = r ? r->camera().target_z() : 512.0f;
    f32 y = 0.0f;
    if (auto* sim = get_sim(L); sim && sim->terrain()) y = sim->terrain()->get_terrain_height(x, z);
    lua_newtable(L);
    lua_pushnumber(L, x);
    lua_rawseti(L, -2, 1);
    lua_pushnumber(L, y);
    lua_rawseti(L, -2, 2);
    lua_pushnumber(L, z);
    lua_rawseti(L, -2, 3);
    return 1;
}

static int camera_GetHeading(lua_State* L) {
    auto* r = get_renderer(L);
    lua_pushnumber(L, r ? r->camera().yaw() : 0.0f);
    return 1;
}

static int camera_GetPitch(lua_State* L) {
    auto* r = get_renderer(L);
    lua_pushnumber(L, r ? r->camera().pitch() : 0.87f);
    return 1;
}

/// UIZoomTo(units, duration) — animate camera to center on units.
/// units is a Lua array of unit objects with _c_object lightuserdata.
static int l_UIZoomTo(lua_State* L) {
    if (!lua_istable(L, 1)) return 0;
    auto* r = get_renderer(L);
    if (!r) return 0;

    f32 sum_x = 0, sum_z = 0;
    int count = 0;
    int n = luaL_getn(L, 1);
    for (int i = 1; i <= n; ++i) {
        lua_rawgeti(L, 1, i);
        if (auto* e = check_entity(L, lua_gettop(L)); e && !e->destroyed()) {
            auto pos = e->position();
            sum_x += pos.x;
            sum_z += pos.z;
            ++count;
        }
        lua_pop(L, 1); // array element
    }

    if (count > 0) {
        r->camera().set_target(sum_x / count, sum_z / count);
    }
    return 0;
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

// SimCallback({Func="name", Args={...}}, addUnitSelection)
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
    }
    lua_pop(L, 1); // pop Args table (or nil)

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
    static const sim::CategoryName kCarrier{"CARRIER"};
    std::vector<Pad> pads;
    registry.for_each_unit([&](sim::Entity& e) {
        if (e.destroyed() || !e.is_unit() || e.army() != focus) return;
        const auto& pad = static_cast<const sim::Unit&>(e);
        if (pad.is_being_built() || pad.is_dying() || !pad.has_category(kStaging)) return;
        // Landing on a carrier isn't modelled yet: carriers are left out.
        if (pad.has_category(kCarrier)) return;
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
    set_num("entityId", static_cast<lua_Number>(unit->entity_id()));
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
            lua_pushnumber(L, static_cast<lua_Number>(focus_unit->entity_id()));
            lua_rawset(L, -3);
            lua_rawset(L, -3); // result["focus"] = focus_sub_table
        }
    }

    return 1;
}

void register_user_bindings(LuaState& state) {
    lua_State* L = state.raw();
    // Globals of the UI state.
    state.register_function("InternalCreateWldUIProvider", l_InternalCreateWldUIProvider);
    state.register_function("GetMouseWorldPos", l_GetMouseWorldPos);
    state.register_function("GetCamera", l_GetCamera);
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
                      });
}

} // namespace osc::lua
