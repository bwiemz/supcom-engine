// Shields: moho.shield_methods.
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

static sim::Shield* check_shield(lua_State* L, int idx = 1) {
    auto* e = check_entity(L, idx);
    if (e && e->is_shield())
        return static_cast<sim::Shield*>(e);
    return nullptr;
}

// ====================================================================
// Shield methods — real implementations
// ====================================================================
// Shield-specific methods only — GetHealth, SetHealth, GetMaxHealth, Destroy,
// BeenDestroyed are inherited from entity_methods via the base class chain.
// DO NOT re-declare them here or ClassShield(moho.shield_methods, Entity) will
// error with "field 'X' is ambiguous between the bases" because the flattened
// closures differ from the Entity class's copies.

static int shield_TurnOn(lua_State* L) {
    auto* s = check_shield(L);
    if (s) s->is_on = true;
    return 0;
}
static int shield_TurnOff(lua_State* L) {
    auto* s = check_shield(L);
    if (s) s->is_on = false;
    return 0;
}
static int shield_IsOn(lua_State* L) {
    auto* s = check_shield(L);
    lua_pushboolean(L, (s && s->is_on) ? 1 : 0);
    return 1;
}
static int shield_GetOmni(lua_State* L) {
    // No fog of war / omni detection yet
    lua_pushboolean(L, 0);
    return 1;
}
static int shield_GetType(lua_State* L) {
    auto* s = check_shield(L);
    if (s && !s->shield_type.empty()) {
        lua_pushstring(L, s->shield_type.c_str());
    } else {
        lua_pushstring(L, "");
    }
    return 1;
}
static int shield_SetSize(lua_State* L) {
    auto* s = check_shield(L);
    if (s) {
        s->size = static_cast<f32>(luaL_checknumber(L, 2));
    }
    return 0;
}

// clang-format off
const MethodEntry shield_methods[] = {
    {"GetOmni",                     shield_GetOmni},
    {"TurnOn",                      shield_TurnOn},
    {"TurnOff",                     shield_TurnOff},
    {"IsOn",                        shield_IsOn},
    {"GetType",                     shield_GetType},
    {"SetSize",                     shield_SetSize},
    {nullptr, nullptr},
};
// clang-format on

} // namespace osc::lua
