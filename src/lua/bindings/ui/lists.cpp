// UI item lists and scrollbars.
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
// ItemList methods (M74)
// ====================================================================

/// item_list:AddItem(text)
static int itemlist_AddItem(lua_State* L) {
    auto* ctrl = check_control(L);
    if (!ctrl) return 0;
    if (lua_type(L, 2) == LUA_TSTRING)
        ctrl->add_item(lua_tostring(L, 2));
    else if (lua_isnumber(L, 2))
        ctrl->add_item(std::to_string(static_cast<int>(lua_tonumber(L, 2))));
    return 0;
}

/// item_list:DeleteItem(index) — 0-based
static int itemlist_DeleteItem(lua_State* L) {
    auto* ctrl = check_control(L);
    if (ctrl && lua_isnumber(L, 2))
        ctrl->delete_item(static_cast<i32>(lua_tonumber(L, 2)));
    return 0;
}

/// item_list:DeleteAllItems()
static int itemlist_DeleteAllItems(lua_State* L) {
    auto* ctrl = check_control(L);
    if (ctrl) ctrl->delete_all_items();
    return 0;
}

/// item_list:ModifyItem(index, string) — 0-based
static int itemlist_ModifyItem(lua_State* L) {
    auto* ctrl = check_control(L);
    if (!ctrl) return 0;
    i32 idx = lua_isnumber(L, 2) ? static_cast<i32>(lua_tonumber(L, 2)) : -1;
    if (lua_type(L, 3) == LUA_TSTRING)
        ctrl->modify_item(idx, lua_tostring(L, 3));
    return 0;
}

/// item_list:GetItem(index) → string — 0-based
static int itemlist_GetItem(lua_State* L) {
    auto* ctrl = check_control(L);
    if (!ctrl) { lua_pushstring(L, ""); return 1; }
    i32 idx = lua_isnumber(L, 2) ? static_cast<i32>(lua_tonumber(L, 2)) : -1;
    lua_pushstring(L, ctrl->get_item(idx).c_str());
    return 1;
}

/// item_list:GetItemCount() → number
static int itemlist_GetItemCount(lua_State* L) {
    auto* ctrl = check_control(L);
    lua_pushnumber(L, ctrl ? ctrl->item_count() : 0);
    return 1;
}

/// item_list:Empty() → bool
static int itemlist_Empty(lua_State* L) {
    auto* ctrl = check_control(L);
    lua_pushboolean(L, ctrl ? (ctrl->item_count() == 0) : 1);
    return 1;
}

/// item_list:GetSelection() → number (0-based, -1 if none)
static int itemlist_GetSelection(lua_State* L) {
    auto* ctrl = check_control(L);
    lua_pushnumber(L, ctrl ? ctrl->selection() : -1);
    return 1;
}

/// item_list:SetSelection(index) — 0-based
static int itemlist_SetSelection(lua_State* L) {
    auto* ctrl = check_control(L);
    if (ctrl && lua_isnumber(L, 2))
        ctrl->set_selection(static_cast<i32>(lua_tonumber(L, 2)));
    return 0;
}

/// item_list:GetRowHeight() → number
static int itemlist_GetRowHeight(lua_State* L) {
    auto* ctrl = check_control(L);
    if (!ctrl) { lua_pushnumber(L, 0); return 1; }
    // Row height ≈ font ascent + descent
    f32 ps = static_cast<f32>(ctrl->font_pointsize());
    lua_pushnumber(L, ps); // approximate row height
    return 1;
}

/// item_list:SetNewFont(family, pointsize)
static int itemlist_SetNewFont(lua_State* L) {
    auto* ctrl = check_control(L);
    if (!ctrl) return 0;
    if (lua_type(L, 2) == LUA_TSTRING)
        ctrl->set_font_family(lua_tostring(L, 2));
    if (lua_isnumber(L, 3))
        ctrl->set_font_pointsize(static_cast<i32>(lua_tonumber(L, 3)));
    update_font_metrics(ctrl);
    return 0;
}

/// item_list:SetNewColors(fg, bg, sel_fg, sel_bg, mo_fg, mo_bg)
static int itemlist_SetNewColors(lua_State* L) {
    auto* ctrl = check_control(L);
    if (!ctrl) return 0;
    if (lua_type(L, 2) == LUA_TSTRING)
        ctrl->set_item_fg_color(parse_color_hex(lua_tostring(L, 2)));
    if (lua_type(L, 3) == LUA_TSTRING)
        ctrl->set_item_bg_color(parse_color_hex(lua_tostring(L, 3)));
    if (lua_type(L, 4) == LUA_TSTRING)
        ctrl->set_item_sel_fg_color(parse_color_hex(lua_tostring(L, 4)));
    if (lua_type(L, 5) == LUA_TSTRING)
        ctrl->set_item_sel_bg_color(parse_color_hex(lua_tostring(L, 5)));
    if (lua_type(L, 6) == LUA_TSTRING)
        ctrl->set_item_mo_fg_color(parse_color_hex(lua_tostring(L, 6)));
    if (lua_type(L, 7) == LUA_TSTRING)
        ctrl->set_item_mo_bg_color(parse_color_hex(lua_tostring(L, 7)));
    return 0;
}

/// item_list:GetStringAdvance(text) → number
static int itemlist_GetStringAdvance(lua_State* L) {
    auto* ctrl = check_control(L);
    if (!ctrl) { lua_pushnumber(L, 0); return 1; }
    if (lua_type(L, 2) != LUA_TSTRING) { lua_pushnumber(L, 0); return 1; }
    const char* s = lua_tostring(L, 2);
    if (!s) { lua_pushnumber(L, 0); return 1; }
    auto& fmp = ui::FontMetricsProvider::instance();
    f32 adv = fmp.string_advance(ctrl->font_family(), ctrl->font_pointsize(),
                                  std::string(s));
    if (adv < 0.0f) {
        f32 ps = static_cast<f32>(ctrl->font_pointsize());
        adv = ps * 0.6f * std::strlen(s);
    }
    lua_pushnumber(L, adv);
    return 1;
}

/// item_list:ScrollToTop()
static int itemlist_ScrollToTop(lua_State* L) {
    auto* ctrl = check_control(L);
    if (ctrl) ctrl->set_scroll_top(0);
    return 0;
}

/// item_list:ScrollToBottom()
static int itemlist_ScrollToBottom(lua_State* L) {
    auto* ctrl = check_control(L);
    if (ctrl && ctrl->item_count() > 0)
        ctrl->set_scroll_top(ctrl->item_count() - 1);
    return 0;
}

/// item_list:ShowItem(index) — scroll to make item visible
static int itemlist_ShowItem(lua_State* L) {
    auto* ctrl = check_control(L);
    if (ctrl && lua_isnumber(L, 2))
        ctrl->set_scroll_top(static_cast<i32>(lua_tonumber(L, 2)));
    return 0;
}

/// item_list:NeedsScrollBar() → bool
static int itemlist_NeedsScrollBar(lua_State* L) {
    // Always false for now (no real viewport calculation)
    lua_pushboolean(L, 0);
    return 1;
}

/// item_list:ShowSelection(bool)
static int itemlist_ShowSelection(lua_State* L) {
    auto* ctrl = check_control(L);
    if (ctrl) ctrl->set_show_selection(lua_toboolean(L, 2) != 0);
    return 0;
}

/// item_list:ShowMouseoverItem(bool)
static int itemlist_ShowMouseoverItem(lua_State* L) {
    auto* ctrl = check_control(L);
    if (ctrl) ctrl->set_show_mouseover(lua_toboolean(L, 2) != 0);
    return 0;
}

static int itemlist_AddItems(lua_State* L) {
    if (!lua_istable(L, 1) || !lua_istable(L, 2)) return 0;
    int n = luaL_getn(L, 2);
    for (int i = 1; i <= n; i++) {
        lua_rawgeti(L, 2, i);
        if (lua_type(L, -1) == LUA_TSTRING) {
            lua_pushstring(L, "AddItem");
            lua_gettable(L, 1);
            lua_pushvalue(L, 1);
            lua_pushvalue(L, -3);
            lua_pcall(L, 2, 0, 0);
        }
        lua_pop(L, 1);
    }
    return 0;
}

static int itemlist_ClearItems(lua_State* L) {
    lua_pushstring(L, "DeleteAllItems");
    lua_gettable(L, 1);
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, 1);
        lua_pcall(L, 1, 0, 0);
    } else {
        lua_pop(L, 1);
    }
    return 0;
}

static int itemlist_SetTitleText(lua_State* L) {
    if (!lua_istable(L, 1)) return 0;
    lua_pushstring(L, "_titleText");
    lua_pushvalue(L, 2);
    lua_rawset(L, 1);
    return 0;
}

// clang-format off
const MethodEntry ui_item_list_methods[] = {
    {"AddItem",             itemlist_AddItem},
    {"DeleteItem",          itemlist_DeleteItem},
    {"DeleteAllItems",      itemlist_DeleteAllItems},
    {"ModifyItem",          itemlist_ModifyItem},
    {"GetItem",             itemlist_GetItem},
    {"GetItemCount",        itemlist_GetItemCount},
    {"Empty",               itemlist_Empty},
    {"GetSelection",        itemlist_GetSelection},
    {"SetSelection",        itemlist_SetSelection},
    {"GetRowHeight",        itemlist_GetRowHeight},
    {"SetNewFont",          itemlist_SetNewFont},
    {"SetNewColors",        itemlist_SetNewColors},
    {"GetStringAdvance",    itemlist_GetStringAdvance},
    {"ScrollToTop",         itemlist_ScrollToTop},
    {"ScrollToBottom",      itemlist_ScrollToBottom},
    {"ShowItem",            itemlist_ShowItem},
    {"NeedsScrollBar",      itemlist_NeedsScrollBar},
    {"ShowSelection",       itemlist_ShowSelection},
    {"ShowMouseoverItem",   itemlist_ShowMouseoverItem},
    {"AddItems",            itemlist_AddItems},
    {"ClearItems",          itemlist_ClearItems},
    {"SetTitleText",        itemlist_SetTitleText},
    // SetAlpha removed — inherited from Control to avoid ClassUI ambiguity
    {nullptr, nullptr},
};
// clang-format on

// ====================================================================
// Scrollbar methods (M74)
// ====================================================================

/// scrollbar:SetScrollable(scrollable)
static int scrollbar_SetScrollable(lua_State* L) {
    auto* ctrl = check_control(L);
    if (!ctrl) return 0;
    // Store a Lua ref to the scrollable control
    if (lua_istable(L, 2)) {
        // Release old ref before storing new one
        if (ctrl->scrollable_ref() >= 0)
            luaL_unref(L, LUA_REGISTRYINDEX, ctrl->scrollable_ref());
        lua_pushvalue(L, 2);
        int ref = luaL_ref(L, LUA_REGISTRYINDEX);
        ctrl->set_scrollable_ref(ref);
    }
    return 0;
}

/// scrollbar:SetNewTextures(background, thumbMiddle, thumbTop, thumbBottom)
static int scrollbar_SetNewTextures(lua_State* L) {
    auto* ctrl = check_control(L);
    if (!ctrl) return 0;
    if (lua_type(L, 2) == LUA_TSTRING)
        ctrl->set_sb_bg_texture(lua_tostring(L, 2));
    if (lua_type(L, 3) == LUA_TSTRING)
        ctrl->set_sb_thumb_mid(lua_tostring(L, 3));
    if (lua_type(L, 4) == LUA_TSTRING)
        ctrl->set_sb_thumb_top(lua_tostring(L, 4));
    if (lua_type(L, 5) == LUA_TSTRING)
        ctrl->set_sb_thumb_bot(lua_tostring(L, 5));
    return 0;
}

/// Helper: call scrollable:ScrollLines/ScrollPages on the scrollable ref
static void call_scrollable_method(lua_State* L, ui::UIControl* ctrl,
                                    const char* method, f32 amount) {
    int ref = ctrl->scrollable_ref();
    if (ref < 0) return;
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return; }
    lua_pushstring(L, method);
    lua_gettable(L, -2);
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, -2); // scrollable self
        lua_pushstring(L, ctrl->scroll_axis().c_str());
        lua_pushnumber(L, amount);
        if (lua_pcall(L, 3, 0, 0) != 0)
            lua_pop(L, 1); // pop error
        lua_pop(L, 1); // pop scrollable table
    } else {
        lua_pop(L, 2); // pop non-function + scrollable
    }
}

/// scrollbar:DoScrollLines(lines)
static int scrollbar_DoScrollLines(lua_State* L) {
    auto* ctrl = check_control(L);
    if (!ctrl) return 0;
    f32 lines = lua_isnumber(L, 2) ? static_cast<f32>(lua_tonumber(L, 2)) : 0.0f;
    call_scrollable_method(L, ctrl, "ScrollLines", lines);
    return 0;
}

/// scrollbar:DoScrollPages(pages)
static int scrollbar_DoScrollPages(lua_State* L) {
    auto* ctrl = check_control(L);
    if (!ctrl) return 0;
    f32 pages = lua_isnumber(L, 2) ? static_cast<f32>(lua_tonumber(L, 2)) : 0.0f;
    call_scrollable_method(L, ctrl, "ScrollPages", pages);
    return 0;
}

// clang-format off
const MethodEntry ui_scrollbar_methods[] = {
    {"SetScrollable",       scrollbar_SetScrollable},
    {"SetNewTextures",      scrollbar_SetNewTextures},
    {"DoScrollLines",       scrollbar_DoScrollLines},
    {"DoScrollPages",       scrollbar_DoScrollPages},
    {nullptr, nullptr},
};
// clang-format on

} // namespace osc::lua
