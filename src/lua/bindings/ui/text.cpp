// UI text and edit controls.
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

/// text:SetNewFont(family, pointsize)
static int text_SetNewFont(lua_State* L) {
    auto* ctrl = check_control(L);
    if (!ctrl) return 0;
    if (lua_type(L, 2) == LUA_TSTRING) {
        ctrl->set_font_family(lua_tostring(L, 2));
    }
    if (lua_isnumber(L, 3)) {
        ctrl->set_font_pointsize(static_cast<i32>(lua_tonumber(L, 3)));
    }
    update_font_metrics(ctrl);
    update_text_advance(ctrl);
    // Update LazyVars on self table
    push_font_lazyvars(L, 1, ctrl);
    return 0;
}

/// text:SetNewColor(color) — color is hex string like "ffFFFFFF"
static int text_SetNewColor(lua_State* L) {
    auto* ctrl = check_control(L);
    if (!ctrl) return 0;
    if (lua_type(L, 2) == LUA_TSTRING) {
        ctrl->set_text_color(parse_color_hex(lua_tostring(L, 2)));
    }
    return 0;
}

/// text:SetText(text)
static int text_SetText(lua_State* L) {
    auto* ctrl = check_control(L);
    if (!ctrl) return 0;
    if (lua_type(L, 2) == LUA_TSTRING) {
        ctrl->set_text_content(lua_tostring(L, 2));
    } else if (lua_isnumber(L, 2)) {
        // SetText can accept a number — convert to string
        ctrl->set_text_content(std::to_string(static_cast<int>(lua_tonumber(L, 2))));
    } else {
        ctrl->set_text_content("");
    }
    update_text_advance(ctrl);
    // Update TextAdvance LazyVar
    push_font_lazyvars(L, 1, ctrl);
    return 0;
}

/// text:GetText() → string
static int text_GetText(lua_State* L) {
    auto* ctrl = check_control(L);
    if (!ctrl) { lua_pushstring(L, ""); return 1; }
    lua_pushstring(L, ctrl->text_content().c_str());
    return 1;
}

/// text:SetDropShadow(bool)
static int text_SetDropShadow(lua_State* L) {
    auto* ctrl = check_control(L);
    if (ctrl) ctrl->set_drop_shadow(lua_toboolean(L, 2) != 0);
    return 0;
}

/// text:SetNewClipToWidth(bool)
static int text_SetNewClipToWidth(lua_State* L) {
    auto* ctrl = check_control(L);
    if (ctrl) ctrl->set_clip_to_width(lua_toboolean(L, 2) != 0);
    return 0;
}

/// text:SetCenteredVertically(bool)
static int text_SetCenteredVertically(lua_State* L) {
    auto* ctrl = check_control(L);
    if (ctrl) ctrl->set_centered_vertically(lua_toboolean(L, 2) != 0);
    return 0;
}

/// text:SetCenteredHorizontally(bool)
static int text_SetCenteredHorizontally(lua_State* L) {
    auto* ctrl = check_control(L);
    if (ctrl) ctrl->set_centered_horizontally(lua_toboolean(L, 2) != 0);
    return 0;
}

/// text:GetStringAdvance(text) → number
/// Returns the pixel width of the given string in the current font.
static int text_GetStringAdvance(lua_State* L) {
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

// clang-format off
const MethodEntry ui_text_methods[] = {
    {"SetNewFont",              text_SetNewFont},
    {"SetNewColor",             text_SetNewColor},
    {"SetText",                 text_SetText},
    {"GetText",                 text_GetText},
    {"SetDropShadow",           text_SetDropShadow},
    {"SetNewClipToWidth",       text_SetNewClipToWidth},
    {"SetCenteredVertically",   text_SetCenteredVertically},
    {"SetCenteredHorizontally", text_SetCenteredHorizontally},
    {"GetStringAdvance",        text_GetStringAdvance},
    {nullptr, nullptr},
};
// clang-format on

// ====================================================================
// Edit methods (M74)
// ====================================================================

/// edit:SetText(text)
static int edit_SetText(lua_State* L) {
    auto* ctrl = check_control(L);
    if (!ctrl) return 0;
    if (lua_type(L, 2) == LUA_TSTRING)
        ctrl->set_text_content(lua_tostring(L, 2));
    return 0;
}

/// edit:GetText() → string
static int edit_GetText(lua_State* L) {
    auto* ctrl = check_control(L);
    if (!ctrl) { lua_pushstring(L, ""); return 1; }
    lua_pushstring(L, ctrl->text_content().c_str());
    return 1;
}

/// edit:ClearText()
static int edit_ClearText(lua_State* L) {
    auto* ctrl = check_control(L);
    if (ctrl) ctrl->set_text_content("");
    return 0;
}

/// edit:SetNewFont(family, pointsize)
static int edit_SetNewFont(lua_State* L) {
    auto* ctrl = check_control(L);
    if (!ctrl) return 0;
    if (lua_type(L, 2) == LUA_TSTRING)
        ctrl->set_font_family(lua_tostring(L, 2));
    if (lua_isnumber(L, 3))
        ctrl->set_font_pointsize(static_cast<i32>(lua_tonumber(L, 3)));
    update_font_metrics(ctrl);
    return 0;
}

/// edit:SetNewForegroundColor(color)
static int edit_SetNewForegroundColor(lua_State* L) {
    auto* ctrl = check_control(L);
    if (!ctrl) return 0;
    if (lua_type(L, 2) == LUA_TSTRING)
        ctrl->set_foreground_color(parse_color_hex(lua_tostring(L, 2)));
    return 0;
}

/// edit:GetForegroundColor() → string
static int edit_GetForegroundColor(lua_State* L) {
    auto* ctrl = check_control(L);
    if (!ctrl) { lua_pushstring(L, "ffffffff"); return 1; }
    char buf[9];
    snprintf(buf, sizeof(buf), "%08x", ctrl->foreground_color());
    lua_pushstring(L, buf);
    return 1;
}

/// edit:SetNewBackgroundColor(color)
static int edit_SetNewBackgroundColor(lua_State* L) {
    auto* ctrl = check_control(L);
    if (!ctrl) return 0;
    if (lua_type(L, 2) == LUA_TSTRING)
        ctrl->set_background_color(parse_color_hex(lua_tostring(L, 2)));
    return 0;
}

/// edit:GetBackgroundColor() → string
static int edit_GetBackgroundColor(lua_State* L) {
    auto* ctrl = check_control(L);
    if (!ctrl) { lua_pushstring(L, "ff000000"); return 1; }
    char buf[9];
    snprintf(buf, sizeof(buf), "%08x", ctrl->background_color());
    lua_pushstring(L, buf);
    return 1;
}

/// edit:ShowBackground(bool)
static int edit_ShowBackground(lua_State* L) {
    auto* ctrl = check_control(L);
    if (ctrl) ctrl->set_bg_visible(lua_toboolean(L, 2) != 0);
    return 0;
}

/// edit:IsBackgroundVisible() → bool
static int edit_IsBackgroundVisible(lua_State* L) {
    auto* ctrl = check_control(L);
    lua_pushboolean(L, ctrl ? ctrl->bg_visible() : 0);
    return 1;
}

/// edit:SetNewCaretColor(color)
static int edit_SetNewCaretColor(lua_State* L) {
    auto* ctrl = check_control(L);
    if (!ctrl) return 0;
    if (lua_type(L, 2) == LUA_TSTRING)
        ctrl->set_caret_color(parse_color_hex(lua_tostring(L, 2)));
    return 0;
}

/// edit:GetCaretColor() → string
static int edit_GetCaretColor(lua_State* L) {
    auto* ctrl = check_control(L);
    if (!ctrl) { lua_pushstring(L, "ffffffff"); return 1; }
    char buf[9];
    snprintf(buf, sizeof(buf), "%08x", ctrl->caret_color());
    lua_pushstring(L, buf);
    return 1;
}

/// edit:ShowCaret(bool)
static int edit_ShowCaret(lua_State* L) {
    auto* ctrl = check_control(L);
    if (ctrl) ctrl->set_caret_visible(lua_toboolean(L, 2) != 0);
    return 0;
}

/// edit:IsCaretVisible() → bool
static int edit_IsCaretVisible(lua_State* L) {
    auto* ctrl = check_control(L);
    lua_pushboolean(L, ctrl ? ctrl->caret_visible() : 0);
    return 1;
}

/// edit:SetCaretPosition(pos)
static int edit_SetCaretPosition(lua_State* L) {
    auto* ctrl = check_control(L);
    if (ctrl && lua_isnumber(L, 2))
        ctrl->set_caret_position(static_cast<i32>(lua_tonumber(L, 2)));
    return 0;
}

/// edit:GetCaretPosition() → number
static int edit_GetCaretPosition(lua_State* L) {
    auto* ctrl = check_control(L);
    lua_pushnumber(L, ctrl ? ctrl->caret_position() : 0);
    return 1;
}

/// edit:SetCaretCycle(seconds, minAlpha, maxAlpha)
static int edit_SetCaretCycle(lua_State* L) {
    auto* ctrl = check_control(L);
    if (!ctrl) return 0;
    f32 secs = lua_isnumber(L, 2) ? static_cast<f32>(lua_tonumber(L, 2)) : 1.0f;
    f32 min_a = lua_isnumber(L, 3) ? static_cast<f32>(lua_tonumber(L, 3)) : 0.0f;
    f32 max_a = lua_isnumber(L, 4) ? static_cast<f32>(lua_tonumber(L, 4)) : 1.0f;
    ctrl->set_caret_cycle(secs, min_a, max_a);
    return 0;
}

/// edit:SetNewHighlightForegroundColor(color)
static int edit_SetNewHighlightForegroundColor(lua_State* L) {
    auto* ctrl = check_control(L);
    if (!ctrl) return 0;
    if (lua_type(L, 2) == LUA_TSTRING)
        ctrl->set_highlight_fg_color(parse_color_hex(lua_tostring(L, 2)));
    return 0;
}

/// edit:GetHighlightForegroundColor() → string
static int edit_GetHighlightForegroundColor(lua_State* L) {
    auto* ctrl = check_control(L);
    if (!ctrl) { lua_pushstring(L, "ffffffff"); return 1; }
    char buf[9];
    snprintf(buf, sizeof(buf), "%08x", ctrl->highlight_fg_color());
    lua_pushstring(L, buf);
    return 1;
}

/// edit:SetNewHighlightBackgroundColor(color)
static int edit_SetNewHighlightBackgroundColor(lua_State* L) {
    auto* ctrl = check_control(L);
    if (!ctrl) return 0;
    if (lua_type(L, 2) == LUA_TSTRING)
        ctrl->set_highlight_bg_color(parse_color_hex(lua_tostring(L, 2)));
    return 0;
}

/// edit:GetHighlightBackgroundColor() → string
static int edit_GetHighlightBackgroundColor(lua_State* L) {
    auto* ctrl = check_control(L);
    if (!ctrl) { lua_pushstring(L, "ff0000ff"); return 1; }
    char buf[9];
    snprintf(buf, sizeof(buf), "%08x", ctrl->highlight_bg_color());
    lua_pushstring(L, buf);
    return 1;
}

/// edit:SetMaxChars(size)
static int edit_SetMaxChars(lua_State* L) {
    auto* ctrl = check_control(L);
    if (ctrl && lua_isnumber(L, 2))
        ctrl->set_max_chars(static_cast<i32>(lua_tonumber(L, 2)));
    return 0;
}

/// edit:GetMaxChars() → number
static int edit_GetMaxChars(lua_State* L) {
    auto* ctrl = check_control(L);
    lua_pushnumber(L, ctrl ? ctrl->max_chars() : 0);
    return 1;
}

/// edit:SetDropShadow(bool)
static int edit_SetDropShadow(lua_State* L) {
    auto* ctrl = check_control(L);
    if (ctrl) ctrl->set_drop_shadow(lua_toboolean(L, 2) != 0);
    return 0;
}

/// edit:EnableInput()
static int edit_EnableInput(lua_State* L) {
    auto* ctrl = check_control(L);
    if (ctrl) ctrl->set_input_enabled(true);
    return 0;
}

/// edit:DisableInput()
static int edit_DisableInput(lua_State* L) {
    auto* ctrl = check_control(L);
    if (ctrl) ctrl->set_input_enabled(false);
    return 0;
}

/// edit:IsEnabled() → bool
static int edit_IsEnabled(lua_State* L) {
    auto* ctrl = check_control(L);
    lua_pushboolean(L, ctrl ? ctrl->input_enabled() : 0);
    return 1;
}

/// edit:GetFontHeight() → number
static int edit_GetFontHeight(lua_State* L) {
    auto* ctrl = check_control(L);
    if (!ctrl) { lua_pushnumber(L, 0); return 1; }
    f32 ps = static_cast<f32>(ctrl->font_pointsize());
    lua_pushnumber(L, ps); // font height ≈ point size
    return 1;
}

/// edit:GetStringAdvance(text) → number
static int edit_GetStringAdvance(lua_State* L) {
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

/// edit:AcquireFocus()
static int edit_AcquireFocus(lua_State* L) {
    auto* ctrl = check_control(L);
    auto* reg = get_ui_registry(L);
    if (ctrl && reg) {
        reg->set_keyboard_focus(ctrl);
        ctrl->set_keyboard_focus(true);
    }
    return 0;
}

/// edit:AbandonFocus()
static int edit_AbandonFocus(lua_State* L) {
    auto* ctrl = check_control(L);
    auto* reg = get_ui_registry(L);
    if (ctrl && reg) {
        if (reg->keyboard_focus() == ctrl)
            reg->set_keyboard_focus(nullptr);
        ctrl->set_keyboard_focus(false);
    }
    return 0;
}

// clang-format off
const MethodEntry ui_edit_methods[] = {
    {"SetText",                         edit_SetText},
    {"GetText",                         edit_GetText},
    {"ClearText",                       edit_ClearText},
    {"SetNewFont",                      edit_SetNewFont},
    {"SetNewForegroundColor",           edit_SetNewForegroundColor},
    {"GetForegroundColor",              edit_GetForegroundColor},
    {"SetNewBackgroundColor",           edit_SetNewBackgroundColor},
    {"GetBackgroundColor",              edit_GetBackgroundColor},
    {"ShowBackground",                  edit_ShowBackground},
    {"IsBackgroundVisible",             edit_IsBackgroundVisible},
    {"SetNewCaretColor",                edit_SetNewCaretColor},
    {"GetCaretColor",                   edit_GetCaretColor},
    {"ShowCaret",                       edit_ShowCaret},
    {"IsCaretVisible",                  edit_IsCaretVisible},
    {"SetCaretPosition",                edit_SetCaretPosition},
    {"GetCaretPosition",                edit_GetCaretPosition},
    {"SetCaretCycle",                   edit_SetCaretCycle},
    {"SetNewHighlightForegroundColor",  edit_SetNewHighlightForegroundColor},
    {"GetHighlightForegroundColor",     edit_GetHighlightForegroundColor},
    {"SetNewHighlightBackgroundColor",  edit_SetNewHighlightBackgroundColor},
    {"GetHighlightBackgroundColor",     edit_GetHighlightBackgroundColor},
    {"SetMaxChars",                     edit_SetMaxChars},
    {"GetMaxChars",                     edit_GetMaxChars},
    {"SetDropShadow",                   edit_SetDropShadow},
    {"EnableInput",                     edit_EnableInput},
    {"DisableInput",                    edit_DisableInput},
    {"IsEnabled",                       edit_IsEnabled},
    {"GetFontHeight",                   edit_GetFontHeight},
    {"GetStringAdvance",                edit_GetStringAdvance},
    {"AcquireFocus",                    edit_AcquireFocus},
    {"AbandonFocus",                    edit_AbandonFocus},
    {nullptr, nullptr},
};
// clang-format on

} // namespace osc::lua
