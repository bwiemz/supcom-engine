// UI world views and meshes, map previews, and the camera class's stubs.
// Split out of moho_bindings.cpp by class (M191 step 2); what the files
// share is declared in lua/moho_bindings_internal.hpp.

#include "lua/moho_bindings.hpp"
#include "lua/moho_bindings_internal.hpp"
#include "lua/lua_stubs.hpp"
#include "core/dmath.hpp"
#include "sim/blueprint_categories.hpp"
#include "lua/category_utils.hpp"
#include "video/video_decoder.hpp"
#include "map/scmap_parser.hpp"
#include "lua/factory_queue.hpp"
#include "lua/order_helpers.hpp"
#include "lua/lua_state.hpp"
#include "lua/sim_bindings.hpp"
#include "sim/army_brain.hpp"
#include "sim/build_placement.hpp"
#include "sim/bone_data.hpp"
#include "sim/collision.hpp"
#include "sim/entity.hpp"
#include "sim/entity_registry.hpp"
#include "sim/ieffect.hpp"
#include "sim/manipulator.hpp"
#include "core/test_status.hpp"
#include "sim/category_expr.hpp"
#include "sim/prop.hpp"
#include "sim/prop_script.hpp"
#include "sim/sim_state.hpp"
#include "sim/collision_beam.hpp"
#include "sim/projectile_script.hpp"
#include "sim/thread_manager.hpp"
#include "map/visibility_grid.hpp"
#include "sim/unit.hpp"
#include "sim/navigator.hpp"
#include "sim/platoon.hpp"
#include "sim/projectile.hpp"
#include "sim/shield.hpp"
#include "sim/unit_command.hpp"
#include "map/pathfinder.hpp"
#include "map/pathfinding_grid.hpp"
#include "sim/weapon.hpp"
#include "blueprints/blueprint_store.hpp"
#include "audio/sound_manager.hpp"
#include "ui/ui_control.hpp"
#include "ui/world_view.hpp"
#include "ui/font_metrics_provider.hpp"
#include "ui/keymap.hpp"
#include "ui/wld_ui_provider.hpp"
#include "sim/sim_callback_queue.hpp"
#include "map/terrain.hpp"
#include "vfs/virtual_file_system.hpp"
#include "core/front_end_data.hpp"
#include "core/game_state.hpp"
#include "core/localization.hpp"
#include "core/preferences.hpp"
#include "lua/beat_system.hpp"
#include "lua/mp_net_state.hpp"
#include "lua/sim_sync.hpp"
#include <algorithm>
#include <cctype>
#include <map>
#include <chrono>
#include <cmath>
#include <cstring>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>
#include <vector>
#include <spdlog/spdlog.h>
#include <lua.h>
#include <lauxlib.h>

#include <utility>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

namespace osc::lua {

ui::WorldView* check_world_view(lua_State* L, int idx) {
    auto* ctrl = check_control(L, idx);
    if (!ctrl) return nullptr;
    return dynamic_cast<ui::WorldView*>(ctrl);
}

// ====================================================================
// M169: MapPreview control
// ====================================================================

static int mappreview_SetTexture(lua_State* L) {
    auto* ctrl = check_control(L);
    if (!ctrl) { lua_pushboolean(L, 0); return 1; }
    const char* path = luaL_checkstring(L, 2);

    auto* vfs = lua::LuaState::get_vfs(L);
    if (!vfs) { lua_pushboolean(L, 0); return 1; }

    // Try to load the DDS file from VFS (synchronous/blocking)
    auto data = vfs->read_file(path);
    if (!data || data->empty()) {
        lua_pushboolean(L, 0);
        return 1;
    }

    // Set the texture path — TextureCache will resolve it on next render
    ctrl->set_texture_path(path);
    ctrl->set_has_solid_color(false);
    lua_pushboolean(L, 1);
    return 1;
}


static int mappreview_ClearTexture(lua_State* L) {
    auto* ctrl = check_control(L);
    if (ctrl) ctrl->set_texture_path("");
    return 0;
}

// clang-format off
const MethodEntry ui_map_preview_methods[] = {
    {"SetTexture", mappreview_SetTexture},
    {"ClearTexture", mappreview_ClearTexture},
    {nullptr, nullptr},
};
// clang-format on

// ====================================================================
// M75: WorldMesh (stub)
// ====================================================================

static int worldmesh_Destroy(lua_State* L) {
    auto* ctrl = check_control(L);
    if (ctrl) ctrl->mark_destroyed();
    return 0;
}

static int worldmesh_SetMesh(lua_State* L) {
    (void)L;
    return 0;
}

static int worldmesh_SetStance(lua_State* L) {
    (void)L;
    return 0;
}

static int worldmesh_SetHidden(lua_State* L) {
    auto* ctrl = check_control(L);
    if (ctrl && lua_isboolean(L, 2))
        ctrl->set_world_mesh_hidden(lua_toboolean(L, 2) != 0);
    return 0;
}

static int worldmesh_IsHidden(lua_State* L) {
    auto* ctrl = check_control(L);
    lua_pushboolean(L, ctrl ? ctrl->world_mesh_hidden() : 1);
    return 1;
}

static int worldmesh_SetColor(lua_State* L) {
    (void)L;
    return 0;
}

static int worldmesh_SetScale(lua_State* L) {
    (void)L;
    return 0;
}

static int worldmesh_SetAuxiliaryParameter(lua_State* L) {
    (void)L;
    return 0;
}

static int worldmesh_SetFractionCompleteParameter(lua_State* L) {
    (void)L;
    return 0;
}

static int worldmesh_SetFractionHealthParameter(lua_State* L) {
    (void)L;
    return 0;
}

static int worldmesh_SetLifetimeParameter(lua_State* L) {
    (void)L;
    return 0;
}

static int worldmesh_GetInterpolatedPosition(lua_State* L) {
    lua_newtable(L);
    lua_pushnumber(L, 0); lua_rawseti(L, -2, 1);
    lua_pushnumber(L, 0); lua_rawseti(L, -2, 2);
    lua_pushnumber(L, 0); lua_rawseti(L, -2, 3);
    return 1;
}

static int worldmesh_GetInterpolatedAlignedBox(lua_State* L) {
    return worldmesh_GetInterpolatedPosition(L);
}

static int worldmesh_GetInterpolatedOrientedBox(lua_State* L) {
    return worldmesh_GetInterpolatedPosition(L);
}

static int worldmesh_GetInterpolatedScroll(lua_State* L) {
    return worldmesh_GetInterpolatedPosition(L);
}

static int worldmesh_GetInterpolatedSphere(lua_State* L) {
    return worldmesh_GetInterpolatedPosition(L);
}

// clang-format off
const MethodEntry ui_world_mesh_methods[] = {
    {"Destroy",                       worldmesh_Destroy},
    {"SetMesh",                       worldmesh_SetMesh},
    {"SetStance",                     worldmesh_SetStance},
    {"SetHidden",                     worldmesh_SetHidden},
    {"IsHidden",                      worldmesh_IsHidden},
    {"SetColor",                      worldmesh_SetColor},
    {"SetScale",                      worldmesh_SetScale},
    {"SetAuxiliaryParameter",         worldmesh_SetAuxiliaryParameter},
    {"SetFractionCompleteParameter",  worldmesh_SetFractionCompleteParameter},
    {"SetFractionHealthParameter",    worldmesh_SetFractionHealthParameter},
    {"SetLifetimeParameter",          worldmesh_SetLifetimeParameter},
    {"GetInterpolatedPosition",       worldmesh_GetInterpolatedPosition},
    {"GetInterpolatedAlignedBox",     worldmesh_GetInterpolatedAlignedBox},
    {"GetInterpolatedOrientedBox",    worldmesh_GetInterpolatedOrientedBox},
    {"GetInterpolatedScroll",         worldmesh_GetInterpolatedScroll},
    {"GetInterpolatedSphere",         worldmesh_GetInterpolatedSphere},
    {nullptr, nullptr},
};
// clang-format on


// --- UIWorldView methods (M76) ---


static int worldview_CameraReset(lua_State* /*L*/) { return 0; }

static int worldview_EnableResourceRendering(lua_State* /*L*/) { return 0; }

static int worldview_GetRightMouseButtonOrder(lua_State* L) {
    lua_pushnil(L);
    return 1;
}

static int worldview_GetScreenPos(lua_State* L) {
    lua_pushnil(L);
    return 1;
}

static int worldview_GetsGlobalCameraCommands(lua_State* L) {
    auto* wv = check_world_view(L);
    if (wv) {
        lua_pushboolean(L, wv->gets_global_camera_commands() ? 1 : 0);
        return 1;
    }
    lua_pushboolean(L, 0);
    return 1;
}

static int worldview_HasHighlightCommand(lua_State* L) {
    lua_pushboolean(L, 0);
    return 1;
}

static int worldview_IsCartographic(lua_State* L) {
    auto* wv = check_world_view(L);
    lua_pushboolean(L, (wv && wv->is_cartographic()) ? 1 : 0);
    return 1;
}

static int worldview_IsInputLocked(lua_State* L) {
    auto* wv = check_world_view(L);
    lua_pushboolean(L, (wv && wv->is_input_locked()) ? 1 : 0);
    return 1;
}

static int worldview_IsResourceRenderingEnabled(lua_State* L) {
    lua_pushboolean(L, 0);
    return 1;
}

static int worldview_LockInput(lua_State* L) {
    auto* wv = check_world_view(L);
    if (wv) wv->set_input_locked(true);
    return 0;
}


static int worldview_SetCartographic(lua_State* L) {
    auto* wv = check_world_view(L);
    if (wv && lua_isboolean(L, 2))
        wv->set_cartographic(lua_toboolean(L, 2) != 0);
    return 0;
}
static int worldview_SetCustomRender(lua_State* /*L*/) { return 0; }

static int worldview_SetHighlightEnabled(lua_State* L) {
    auto* wv = check_world_view(L);
    if (wv && lua_isboolean(L, 2))
        wv->set_highlight_enabled(lua_toboolean(L, 2) != 0);
    return 0;
}

static int worldview_ShowConvertToPatrolCursor(lua_State* L) {
    lua_pushboolean(L, 0);
    return 1;
}

static int worldview_UnlockInput(lua_State* L) {
    auto* wv = check_world_view(L);
    if (wv) wv->set_input_locked(false);
    return 0;
}

static int worldview_ZoomScale(lua_State* L) {
    auto* wv = check_world_view(L);
    if (!wv) return 0;
    // Args: self, x, y, wheelRotation, wheelDelta
    f32 x = static_cast<f32>(luaL_optnumber(L, 2, 0));
    f32 y = static_cast<f32>(luaL_optnumber(L, 3, 0));
    f32 rotation = static_cast<f32>(luaL_optnumber(L, 4, 0));
    f32 delta = static_cast<f32>(luaL_optnumber(L, 5, 0));
    wv->zoom_scale(x, y, rotation, delta);
    return 0;
}


// worldview:SetBuildGhost(bp_id) — show placement preview for blueprint
static int worldview_SetBuildGhost(lua_State* L) {
    auto* sim = get_sim(L);
    if (!sim || !lua_isstring(L, 2)) return 0;

    std::string bp_id = lua_tostring(L, 2);
    f32 size_x = 1.0f, size_z = 1.0f;

    // Read footprint from blueprint table
    lua_pushstring(L, "__blueprints");
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (lua_istable(L, -1)) {
        lua_pushstring(L, bp_id.c_str());
        lua_rawget(L, -2);
        if (lua_istable(L, -1)) {
            int bp = lua_gettop(L);
            lua_pushstring(L, "Footprint");
            lua_rawget(L, bp);
            if (lua_istable(L, -1)) {
                int fp = lua_gettop(L);
                lua_pushstring(L, "SizeX");
                lua_rawget(L, fp);
                if (lua_isnumber(L, -1))
                    size_x = static_cast<f32>(lua_tonumber(L, -1));
                lua_pop(L, 1);
                lua_pushstring(L, "SizeZ");
                lua_rawget(L, fp);
                if (lua_isnumber(L, -1))
                    size_z = static_cast<f32>(lua_tonumber(L, -1));
                lua_pop(L, 1);
            }
            lua_pop(L, 1); // Footprint
        }
        lua_pop(L, 1); // bp table
    }
    lua_pop(L, 1); // __blueprints

    sim->set_build_ghost(bp_id, size_x, size_z);
    return 0;
}

// worldview:ClearBuildGhost() — hide placement preview
static int worldview_ClearBuildGhost(lua_State* L) {
    auto* sim = get_sim(L);
    if (sim) sim->clear_build_ghost();
    return 0;
}

// clang-format off
const MethodEntry ui_worldview_methods[] = {
    {"CameraReset", worldview_CameraReset},
    {"EnableResourceRendering", worldview_EnableResourceRendering},
    {"GetRightMouseButtonOrder", worldview_GetRightMouseButtonOrder},
    {"GetScreenPos", worldview_GetScreenPos},
    {"GetsGlobalCameraCommands", worldview_GetsGlobalCameraCommands},
    {"HasHighlightCommand", worldview_HasHighlightCommand},
    {"IsCartographic", worldview_IsCartographic},
    {"IsInputLocked", worldview_IsInputLocked},
    {"IsResourceRenderingEnabled", worldview_IsResourceRenderingEnabled},
    {"LockInput", worldview_LockInput},
    {"SetCartographic", worldview_SetCartographic},
    {"SetCustomRender", worldview_SetCustomRender},
    {"SetHighlightEnabled", worldview_SetHighlightEnabled},
    {"ShowConvertToPatrolCursor", worldview_ShowConvertToPatrolCursor},
    {"UnlockInput", worldview_UnlockInput},
    {"ZoomScale", worldview_ZoomScale},
    {"SetBuildGhost", worldview_SetBuildGhost},
    {"ClearBuildGhost", worldview_ClearBuildGhost},
    {nullptr, nullptr},
};
// clang-format on

// --- Camera methods (M136a) ---


static int camera_RevertRotation(lua_State* /*L*/) { return 0; }


// Camera behaviours the orbit camera doesn't model yet: accepted and ignored
// (spin, rotation hold, clock source, acceleration mode, easing, entity
// tracking, locking, the playable-rect sync).
static int camera_Ignored(lua_State* /*L*/) { return 0; }

// clang-format off
const MethodEntry camera_methods[] = {
    {"RevertRotation", camera_RevertRotation},
    {"Spin", camera_Ignored},
    {"HoldRotation", camera_Ignored},
    {"UseSystemClock", camera_Ignored},
    {"UseGameClock", camera_Ignored},
    {"SetAccMode", camera_Ignored},
    {"EnableEaseInOut", camera_Ignored},
    {"TrackEntities", camera_Ignored},
    {"TargetEntities", camera_Ignored},
    {"NoseCam", camera_Ignored},
    {"Lock", camera_Ignored},
    {"Unlock", camera_Ignored},
    {"SyncPlayableRect", camera_Ignored},
    {nullptr, nullptr},
};
// clang-format on

} // namespace osc::lua
