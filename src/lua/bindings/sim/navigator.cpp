// A unit's navigator: moho.navigator_methods.
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

// ====================================================================
// Navigator methods
// ====================================================================

static sim::Navigator* check_navigator(lua_State* L, int idx = 1) {
    if (!lua_istable(L, idx)) return nullptr;
    lua_pushstring(L, "_c_object");
    lua_rawget(L, idx);
    auto* nav = lua_isuserdata(L, -1)
                    ? static_cast<sim::Navigator*>(lua_touserdata(L, -1))
                    : nullptr;
    lua_pop(L, 1);
    return nav;
}

static sim::Unit* check_nav_unit(lua_State* L, int idx = 1) {
    if (!lua_istable(L, idx)) return nullptr;
    lua_pushstring(L, "_c_unit");
    lua_rawget(L, idx);
    auto* unit = lua_isuserdata(L, -1)
                     ? static_cast<sim::Unit*>(lua_touserdata(L, -1))
                     : nullptr;
    lua_pop(L, 1);
    return unit;
}

// navigator:SetGoal(position) — position is {x, y, z} table
static int nav_SetGoal(lua_State* L) {
    auto* unit = check_nav_unit(L);
    if (!unit || unit->destroyed()) return 0;
    auto* nav = check_navigator(L);
    if (!nav) return 0;
    if (!lua_istable(L, 2)) return 0;
    sim::Vector3 pos;
    lua_pushnumber(L, 1);
    lua_gettable(L, 2);
    pos.x = static_cast<f32>(lua_tonumber(L, -1));
    lua_pop(L, 1);
    lua_pushnumber(L, 2);
    lua_gettable(L, 2);
    pos.y = static_cast<f32>(lua_tonumber(L, -1));
    lua_pop(L, 1);
    lua_pushnumber(L, 3);
    lua_gettable(L, 2);
    pos.z = static_cast<f32>(lua_tonumber(L, -1));
    lua_pop(L, 1);
    nav->set_goal(pos);
    return 0;
}

// navigator:AbortMove()
static int nav_AbortMove(lua_State* L) {
    auto* unit = check_nav_unit(L);
    if (!unit || unit->destroyed()) return 0;
    auto* nav = check_navigator(L);
    if (nav) nav->abort_move();
    return 0;
}

// navigator:GetGoal() — returns position table or nil
static int nav_GetGoal(lua_State* L) {
    auto* unit = check_nav_unit(L);
    if (!unit || unit->destroyed()) { lua_pushnil(L); return 1; }
    auto* nav = check_navigator(L);
    if (!nav || !nav->busy()) { // a goal awaiting its path is still the goal
        lua_pushnil(L);
        return 1;
    }
    push_vector3(L, nav->goal());
    return 1;
}

// navigator:GetCurrentTargetSpeed() — returns speed if moving, 0 otherwise
static int nav_GetCurrentTargetSpeed(lua_State* L) {
    auto* unit = check_nav_unit(L);
    if (!unit || unit->destroyed()) { lua_pushnumber(L, 0); return 1; }
    auto* nav = check_navigator(L);
    if (nav && nav->is_moving()) {
        lua_pushnumber(L, unit->max_speed());
    } else {
        lua_pushnumber(L, 0);
    }
    return 1;
}

static int nav_SetSpeedThroughGoal(lua_State* L) {
    auto* nav = check_navigator(L);
    if (nav) nav->set_speed_through_goal(lua_toboolean(L, 2) != 0);
    return 0;
}

// navigator
// clang-format off
const MethodEntry navigator_methods[] = {
    {"GetCurrentTargetSpeed",   nav_GetCurrentTargetSpeed},
    {"SetSpeedThroughGoal",     nav_SetSpeedThroughGoal},
    {"SetGoal",                 nav_SetGoal},
    {"GetGoal",                 nav_GetGoal},
    {"AbortMove",               nav_AbortMove},
    {nullptr, nullptr},
};
// clang-format on

} // namespace osc::lua
