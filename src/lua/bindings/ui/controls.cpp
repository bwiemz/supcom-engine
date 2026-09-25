// UI controls: control, frame, bitmap, border, dragger, cursor, movie and
// histogram.
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

/// Destroy `ctrl` as Moho's Control:Destroy does: its children first, then
/// itself. Each runs its OnDestroy (a class method, so looked up through
/// the class), loses its _c_object, leaves the tree and is marked destroyed.
static void destroy_control_tree(lua_State* L, ui::UIControlRegistry* reg,
                                 ui::UIControl* ctrl) {
    // Scripts destroy things from OnDestroy (themselves, their parent);
    // marking the teardown first makes those calls no-ops rather than a
    // second OnDestroy and a registry ref freed twice.
    ctrl->set_destroying();
    const std::vector<ui::UIControl*> children = ctrl->children();
    for (auto* child : children)
        if (child && !child->destroyed() && !child->destroying())
            destroy_control_tree(L, reg, child);

    const int ref = ctrl->lua_table_ref();
    if (ref >= 0) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
        const int tbl = lua_gettop(L);
        lua_pushstring(L, "OnDestroy");
        lua_gettable(L, tbl);
        if (lua_isfunction(L, -1)) {
            lua_pushvalue(L, tbl);
            if (lua_pcall(L, 1, 0, 0) != 0) {
                spdlog::warn("OnDestroy error: {}", lua_tostring(L, -1));
                lua_pop(L, 1);
            }
        } else {
            lua_pop(L, 1);
        }
        // Null out _c_object to prevent use-after-destroy
        lua_pushstring(L, "_c_object");
        lua_pushlightuserdata(L, nullptr);
        lua_rawset(L, tbl);
        lua_pop(L, 1);
    }

    // The main world view is published for GetMouseWorldPos / GetCamera.
    lua_pushstring(L, "__osc_world_view");
    lua_rawget(L, LUA_REGISTRYINDEX);
    const bool was_world_view = lua_touserdata(L, -1) == ctrl;
    lua_pop(L, 1);
    if (was_world_view) {
        lua_pushstring(L, "__osc_world_view");
        lua_pushnil(L);
        lua_rawset(L, LUA_REGISTRYINDEX);
    }

    ctrl->set_parent(nullptr);
    ctrl->clear_children();
    if (ref >= 0) {
        ctrl->set_lua_table_ref(LUA_NOREF);
        luaL_unref(L, LUA_REGISTRYINDEX, ref);
    }
    reg->destroy(ctrl->control_id());
}

/// The root frame (GetFrame(0)), or null.
static ui::UIControl* root_frame(lua_State* L) {
    lua_pushstring(L, "__osc_root_frame");
    lua_rawget(L, LUA_REGISTRYINDEX);
    ui::UIControl* root = nullptr;
    if (lua_istable(L, -1)) {
        lua_pushstring(L, "_c_object");
        lua_rawget(L, -2);
        root = static_cast<ui::UIControl*>(lua_touserdata(L, -1));
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    return root;
}

static int control_Destroy(lua_State* L) {
    auto* ctrl = check_control(L);
    if (!ctrl || ctrl->destroyed() || ctrl->destroying()) return 0;
    auto* reg = get_ui_registry(L);
    if (!reg) return 0;
    // The root frame outlives every game: retail's Load and replay dialogs
    // destroy the control they were opened over as they leave for the next
    // game, and in a game that is GetFrame(0). What it holds goes.
    if (ctrl == root_frame(L)) {
        const std::vector<ui::UIControl*> children = ctrl->children();
        for (auto* child : children)
            if (child && !child->destroyed() && !child->destroying())
                destroy_control_tree(L, reg, child);
        return 0;
    }
    destroy_control_tree(L, reg, ctrl);
    return 0;
}

static int control_GetParent(lua_State* L) {
    auto* ctrl = check_control(L);
    if (!ctrl || !ctrl->parent()) {
        lua_pushnil(L);
        return 1;
    }
    auto* parent = ctrl->parent();
    if (parent->lua_table_ref() >= 0) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, parent->lua_table_ref());
    } else {
        lua_pushnil(L);
    }
    return 1;
}

static int control_SetParent(lua_State* L) {
    auto* ctrl = check_control(L);
    if (!ctrl) return 0;
    auto* new_parent = check_control(L, 2);
    ctrl->set_parent(new_parent);
    return 0;
}

static int control_ClearChildren(lua_State* L) {
    auto* ctrl = check_control(L);
    if (ctrl) ctrl->clear_children();
    return 0;
}

static int control_Show(lua_State* L) {
    auto* ctrl = check_control(L);
    if (ctrl) ctrl->set_hidden(false);
    return 0;
}

static int control_Hide(lua_State* L) {
    auto* ctrl = check_control(L);
    if (ctrl) ctrl->set_hidden(true);
    return 0;
}

static int control_SetHidden(lua_State* L) {
    auto* ctrl = check_control(L);
    if (!ctrl) return 0;
    bool h = lua_toboolean(L, 2) != 0;

    // FA calls OnHide(hidden) before applying the state change.
    // If OnHide returns true, the operation is suppressed.
    if (lua_istable(L, 1)) {
        lua_pushstring(L, "OnHide");
        lua_gettable(L, 1);
        if (lua_isfunction(L, -1)) {
            lua_pushvalue(L, 1); // self
            lua_pushboolean(L, h);
            if (lua_pcall(L, 2, 1, 0) == 0) {
                if (lua_toboolean(L, -1)) {
                    lua_pop(L, 1); // pop return value
                    return 0; // suppressed
                }
                lua_pop(L, 1); // pop return value
            } else {
                lua_pop(L, 1); // pop error
            }
        } else {
            lua_pop(L, 1); // pop nil
        }
    }

    ctrl->set_hidden(h);
    return 0;
}

static int control_IsHidden(lua_State* L) {
    auto* ctrl = check_control(L);
    lua_pushboolean(L, ctrl && ctrl->hidden());
    return 1;
}

static int control_DisableHitTest(lua_State* L) {
    auto* ctrl = check_control(L);
    if (ctrl) ctrl->set_hit_test_disabled(true);
    return 0;
}

static int control_EnableHitTest(lua_State* L) {
    auto* ctrl = check_control(L);
    if (ctrl) ctrl->set_hit_test_disabled(false);
    return 0;
}

static int control_IsHitTestDisabled(lua_State* L) {
    auto* ctrl = check_control(L);
    lua_pushboolean(L, ctrl && ctrl->hit_test_disabled());
    return 1;
}

static void set_alpha_recursive(ui::UIControl* ctrl, f32 a) {
    ctrl->set_alpha(a);
    for (auto* child : ctrl->children())
        set_alpha_recursive(child, a);
}

static int control_SetAlpha(lua_State* L) {
    auto* ctrl = check_control(L);
    if (!ctrl) return 0;
    f32 a = static_cast<f32>(luaL_checknumber(L, 2));
    bool apply_children = lua_toboolean(L, 3) != 0;
    ctrl->set_alpha(a);
    if (apply_children) {
        for (auto* child : ctrl->children())
            set_alpha_recursive(child, a);
    }
    return 0;
}

static int control_GetAlpha(lua_State* L) {
    auto* ctrl = check_control(L);
    lua_pushnumber(L, ctrl ? ctrl->alpha() : 1.0);
    return 1;
}

static int control_SetRenderPass(lua_State* L) {
    auto* ctrl = check_control(L);
    if (ctrl) ctrl->set_render_pass(static_cast<i32>(luaL_checknumber(L, 2)));
    return 0;
}

static int control_GetRenderPass(lua_State* L) {
    auto* ctrl = check_control(L);
    lua_pushnumber(L, ctrl ? ctrl->render_pass() : 0);
    return 1;
}

static int control_AcquireKeyboardFocus(lua_State* L) {
    auto* ctrl = check_control(L);
    auto* reg = get_ui_registry(L);
    if (!ctrl || !reg) return 0;

    bool blocks = lua_toboolean(L, 2) != 0;
    auto* prev = reg->keyboard_focus();
    if (prev && prev != ctrl) {
        prev->set_keyboard_focus(false);
        // Call OnLoseKeyboardFocus on previous
        if (prev->lua_table_ref() >= 0) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, prev->lua_table_ref());
            lua_pushstring(L, "OnLoseKeyboardFocus");
            lua_rawget(L, -2);
            if (lua_isfunction(L, -1)) {
                lua_pushvalue(L, -2);
                lua_pcall(L, 1, 0, 0);
            } else {
                lua_pop(L, 1);
            }
            lua_pop(L, 1);
        }
    }

    ctrl->set_keyboard_focus(true);
    ctrl->set_blocks_key_down(blocks);
    reg->set_keyboard_focus(ctrl);
    return 0;
}

static int control_AbandonKeyboardFocus(lua_State* L) {
    auto* ctrl = check_control(L);
    auto* reg = get_ui_registry(L);
    if (!ctrl || !reg) return 0;

    if (reg->keyboard_focus() == ctrl) {
        ctrl->set_keyboard_focus(false);
        reg->set_keyboard_focus(nullptr);
    }
    return 0;
}

static int control_NeedsFrameUpdate(lua_State* L) {
    auto* ctrl = check_control(L);
    lua_pushboolean(L, ctrl && ctrl->needs_frame_update());
    return 1;
}

static int control_SetNeedsFrameUpdate(lua_State* L) {
    auto* ctrl = check_control(L);
    if (ctrl) ctrl->set_needs_frame_update(lua_toboolean(L, 2) != 0);
    return 0;
}

static int control_SetName(lua_State* L) {
    auto* ctrl = check_control(L);
    if (!ctrl) return 0;
    const char* n = luaL_checkstring(L, 2);
    ctrl->set_name(n ? n : "");
    return 0;
}

static int control_GetName(lua_State* L) {
    auto* ctrl = check_control(L);
    if (ctrl) {
        lua_pushstring(L, ctrl->name().c_str());
    } else {
        lua_pushstring(L, "");
    }
    return 1;
}

static int control_Dump(lua_State* L) {
    auto* ctrl = check_control(L);
    if (ctrl) {
        spdlog::info("UIControl#{} '{}' hidden={} alpha={:.2f} hit_test_disabled={} "
                     "needs_update={} children={}",
                     ctrl->control_id(), ctrl->name(), ctrl->hidden(),
                     ctrl->alpha(), ctrl->hit_test_disabled(),
                     ctrl->needs_frame_update(),
                     ctrl->children().size());
    }
    return 0;
}

static int control_GetRootFrame(lua_State* L) {
    auto* ctrl = check_control(L);
    if (!ctrl) { lua_pushnil(L); return 1; }
    // Walk up parent chain to root
    auto* root = ctrl;
    while (root->parent()) root = root->parent();
    if (root->lua_table_ref() >= 0) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, root->lua_table_ref());
    } else {
        lua_pushnil(L);
    }
    return 1;
}

static int control_HitTest(lua_State* L) {
    auto* ctrl = check_control(L);
    if (!ctrl) { lua_pushboolean(L, 0); return 1; }
    // Stub: always return false for now
    lua_pushboolean(L, 0);
    return 1;
}

static int control_Disable(lua_State* L) {
    if (!lua_istable(L, 1)) return 0;
    lua_pushstring(L, "_isDisabled");
    lua_pushboolean(L, 1);
    lua_rawset(L, 1);
    return 0;
}

static int control_Enable(lua_State* L) {
    if (!lua_istable(L, 1)) return 0;
    lua_pushstring(L, "_isDisabled");
    lua_pushboolean(L, 0);
    lua_rawset(L, 1);
    return 0;
}

static int control_IsDisabled(lua_State* L) {
    if (!lua_istable(L, 1)) { lua_pushboolean(L, 0); return 1; }
    lua_pushstring(L, "_isDisabled");
    lua_rawget(L, 1);
    lua_pushboolean(L, lua_toboolean(L, -1));
    lua_replace(L, -2);
    return 1;
}

// clang-format off
const MethodEntry ui_control_methods[] = {
    {"Destroy",                 control_Destroy},
    {"GetParent",               control_GetParent},
    {"SetParent",               control_SetParent},
    {"ClearChildren",           control_ClearChildren},
    {"Show",                    control_Show},
    {"Hide",                    control_Hide},
    {"SetHidden",               control_SetHidden},
    {"IsHidden",                control_IsHidden},
    {"DisableHitTest",          control_DisableHitTest},
    {"EnableHitTest",           control_EnableHitTest},
    {"IsHitTestDisabled",       control_IsHitTestDisabled},
    {"SetAlpha",                control_SetAlpha},
    {"GetAlpha",                control_GetAlpha},
    {"SetRenderPass",           control_SetRenderPass},
    {"GetRenderPass",           control_GetRenderPass},
    {"AcquireKeyboardFocus",    control_AcquireKeyboardFocus},
    {"AbandonKeyboardFocus",    control_AbandonKeyboardFocus},
    {"NeedsFrameUpdate",        control_NeedsFrameUpdate},
    {"SetNeedsFrameUpdate",     control_SetNeedsFrameUpdate},
    {"SetName",                 control_SetName},
    {"GetName",                 control_GetName},
    {"Dump",                    control_Dump},
    {"GetRootFrame",            control_GetRootFrame},
    {"HitTest",                 control_HitTest},
    {"Disable",                 control_Disable},
    {"Enable",                  control_Enable},
    {"IsDisabled",              control_IsDisabled},
    {nullptr, nullptr},
};
// clang-format on

static int frame_GetTopmostDepth(lua_State* L) {
    // Walk all children recursively and find max Depth LazyVar value
    // For now return a fixed value; Depth is managed in Lua LazyVars
    lua_pushnumber(L, 0);
    return 1;
}

static int frame_GetTargetHead(lua_State* L) {
    lua_pushnumber(L, 0); // single monitor
    return 1;
}

static int frame_SetTargetHead(lua_State* L) {
    return 0; // no-op, single monitor
}

// clang-format off
const MethodEntry ui_frame_methods[] = {
    {"GetTopmostDepth",         frame_GetTopmostDepth},
    {"GetTargetHead",           frame_GetTargetHead},
    {"SetTargetHead",           frame_SetTargetHead},
    {nullptr, nullptr},
};
// clang-format on

static int bitmap_SetNewTexture(lua_State* L) {
    auto* ctrl = check_control(L);
    if (!ctrl) return 0;
    i32 border = 1;
    if (lua_type(L, 3) == LUA_TNUMBER) border = static_cast<i32>(lua_tonumber(L, 3));
    ctrl->set_texture_border(border);

    if (lua_type(L, 2) == LUA_TTABLE) {
        // Array of filenames
        std::vector<std::string> textures;
        int n = luaL_getn(L, 2);
        for (int i = 1; i <= n; i++) {
            lua_rawgeti(L, 2, i);
            if (lua_type(L, -1) == LUA_TSTRING)
                textures.emplace_back(lua_tostring(L, -1));
            lua_pop(L, 1);
        }
        ctrl->set_textures(std::move(textures));
        if (!ctrl->textures().empty()) {
            ctrl->set_texture_path(ctrl->textures()[0]);
            auto [w, h] = read_dds_dimensions(L, ctrl->texture_path());
            ctrl->set_bitmap_width(w);
            ctrl->set_bitmap_height(h);
        }
        ctrl->set_has_solid_color(false);
    } else if (lua_type(L, 2) == LUA_TSTRING) {
        std::string path = lua_tostring(L, 2);
        ctrl->set_texture_path(path);
        ctrl->set_textures({path});
        auto [w, h] = read_dds_dimensions(L, path);
        ctrl->set_bitmap_width(w);
        ctrl->set_bitmap_height(h);
        ctrl->set_has_solid_color(false);
    }

    // Push bitmap dimensions to Lua-side Width/Height LazyVars so the
    // UI layout engine can size the control based on the texture.
    // FA's engine does this internally; we must do it explicitly.
    if (ctrl->bitmap_width() > 0 && ctrl->bitmap_height() > 0) {
        // Get the control's Lua table (arg 1 = self)
        if (lua_istable(L, 1)) {
            lua_pushstring(L, "Width");
            lua_rawget(L, 1);
            if (lua_istable(L, -1)) {
                lua_pushstring(L, "Set");
                lua_gettable(L, -2);
                if (lua_isfunction(L, -1)) {
                    lua_pushvalue(L, -2); // self (Width LazyVar)
                    lua_pushnumber(L, ctrl->bitmap_width());
                    lua_pcall(L, 2, 0, 0);
                } else {
                    lua_pop(L, 1);
                }
            }
            lua_pop(L, 1); // Width LazyVar

            lua_pushstring(L, "Height");
            lua_rawget(L, 1);
            if (lua_istable(L, -1)) {
                lua_pushstring(L, "Set");
                lua_gettable(L, -2);
                if (lua_isfunction(L, -1)) {
                    lua_pushvalue(L, -2);
                    lua_pushnumber(L, ctrl->bitmap_height());
                    lua_pcall(L, 2, 0, 0);
                } else {
                    lua_pop(L, 1);
                }
            }
            lua_pop(L, 1); // Height LazyVar
        }
    }

    return 0;
}

static int bitmap_InternalSetSolidColor(lua_State* L) {
    auto* ctrl = check_control(L);
    if (!ctrl) return 0;
    if (lua_type(L, 2) == LUA_TSTRING) {
        const char* hex = lua_tostring(L, 2);
        u32 color = static_cast<u32>(strtoul(hex, nullptr, 16));
        ctrl->set_solid_color(color);
        ctrl->set_has_solid_color(true);
    }
    return 0;
}

static int bitmap_SetColorMask(lua_State* L) {
    auto* ctrl = check_control(L);
    if (!ctrl) return 0;
    if (lua_type(L, 2) == LUA_TSTRING) {
        const char* hex = lua_tostring(L, 2);
        u32 color = static_cast<u32>(strtoul(hex, nullptr, 16));
        ctrl->set_color_mask(color);
    }
    return 0;
}

static int bitmap_SetUV(lua_State* L) {
    auto* ctrl = check_control(L);
    if (!ctrl) return 0;
    f32 u0 = static_cast<f32>(lua_tonumber(L, 2));
    f32 v0 = static_cast<f32>(lua_tonumber(L, 3));
    f32 u1 = static_cast<f32>(lua_tonumber(L, 4));
    f32 v1 = static_cast<f32>(lua_tonumber(L, 5));
    ctrl->set_uv(u0, v0, u1, v1);
    return 0;
}

static int bitmap_SetTiled(lua_State* L) {
    auto* ctrl = check_control(L);
    if (ctrl) ctrl->set_tiled(lua_toboolean(L, 2) != 0);
    return 0;
}

static int bitmap_UseAlphaHitTest(lua_State* L) {
    auto* ctrl = check_control(L);
    if (ctrl) ctrl->set_alpha_hit_test(lua_toboolean(L, 2) != 0);
    return 0;
}

static int bitmap_BitmapWidth(lua_State* L) {
    auto* ctrl = check_control(L);
    lua_pushnumber(L, ctrl ? ctrl->bitmap_width() : 0);
    return 1;
}

static int bitmap_BitmapHeight(lua_State* L) {
    auto* ctrl = check_control(L);
    lua_pushnumber(L, ctrl ? ctrl->bitmap_height() : 0);
    return 1;
}

static int bitmap_SetFrame(lua_State* L) {
    auto* ctrl = check_control(L);
    if (ctrl) {
        i32 f = static_cast<i32>(lua_tonumber(L, 2));
        i32 n = ctrl->num_frames();
        if (n > 0) f = std::max(0, std::min(f, n - 1));
        ctrl->set_current_frame(f);
    }
    return 0;
}

static int bitmap_GetFrame(lua_State* L) {
    auto* ctrl = check_control(L);
    lua_pushnumber(L, ctrl ? ctrl->current_frame() : 0);
    return 1;
}

static int bitmap_GetNumFrames(lua_State* L) {
    auto* ctrl = check_control(L);
    lua_pushnumber(L, ctrl ? ctrl->num_frames() : 0);
    return 1;
}

static int bitmap_SetFrameRate(lua_State* L) {
    auto* ctrl = check_control(L);
    if (ctrl) ctrl->set_frame_rate(static_cast<f32>(lua_tonumber(L, 2)));
    return 0;
}

static int bitmap_SetFramePattern(lua_State* L) {
    auto* ctrl = check_control(L);
    if (!ctrl || !lua_istable(L, 2)) return 0;
    std::vector<i32> pattern;
    int n = luaL_getn(L, 2);
    for (int i = 1; i <= n; i++) {
        lua_rawgeti(L, 2, i);
        pattern.push_back(static_cast<i32>(lua_tonumber(L, -1)));
        lua_pop(L, 1);
    }
    ctrl->set_frame_pattern(std::move(pattern));
    return 0;
}

static int bitmap_SetForwardPattern(lua_State* L) {
    auto* ctrl = check_control(L);
    if (!ctrl || ctrl->num_frames() <= 0) return 0;
    i32 n = ctrl->num_frames();
    std::vector<i32> pat(n);
    for (i32 i = 0; i < n; i++) pat[i] = i;
    ctrl->set_frame_pattern(std::move(pat));
    return 0;
}

static int bitmap_SetBackwardPattern(lua_State* L) {
    auto* ctrl = check_control(L);
    if (!ctrl || ctrl->num_frames() <= 0) return 0;
    i32 n = ctrl->num_frames();
    std::vector<i32> pat(n);
    for (i32 i = 0; i < n; i++) pat[i] = n - 1 - i;
    ctrl->set_frame_pattern(std::move(pat));
    return 0;
}

static int bitmap_SetPingPongPattern(lua_State* L) {
    auto* ctrl = check_control(L);
    if (!ctrl || ctrl->num_frames() <= 0) return 0;
    i32 n = ctrl->num_frames();
    std::vector<i32> pat;
    pat.reserve(2 * static_cast<size_t>(n));
    for (i32 i = 0; i < n; i++) pat.push_back(i);
    for (i32 i = n - 2; i >= 0; i--) pat.push_back(i);
    ctrl->set_frame_pattern(std::move(pat));
    return 0;
}

static int bitmap_SetLoopPingPongPattern(lua_State* L) {
    // Same as PingPong but sets looping
    bitmap_SetPingPongPattern(L);
    auto* ctrl = check_control(L);
    if (ctrl) ctrl->set_anim_looping(true);
    return 0;
}

static int bitmap_Play(lua_State* L) {
    auto* ctrl = check_control(L);
    if (ctrl) ctrl->set_anim_playing(true);
    return 0;
}

static int bitmap_Stop(lua_State* L) {
    auto* ctrl = check_control(L);
    if (ctrl) {
        ctrl->set_anim_playing(false);
        // Call OnAnimationStopped callback
        lua_pushstring(L, "OnAnimationStopped");
        lua_rawget(L, 1);
        if (lua_isfunction(L, -1)) {
            lua_pushvalue(L, 1);
            if (lua_pcall(L, 1, 0, 0) != 0) {
                spdlog::warn("OnAnimationStopped error: {}", lua_tostring(L, -1));
                lua_pop(L, 1);
            }
        } else {
            lua_pop(L, 1);
        }
    }
    return 0;
}

static int bitmap_Loop(lua_State* L) {
    auto* ctrl = check_control(L);
    if (ctrl) ctrl->set_anim_looping(lua_toboolean(L, 2) != 0);
    return 0;
}

static int bitmap_ShareTextures(lua_State* L) {
    auto* ctrl = check_control(L);
    auto* other = check_control(L, 2);
    if (ctrl && other) {
        ctrl->set_textures(std::vector<std::string>(other->textures()));
        ctrl->set_texture_path(other->texture_path());
        ctrl->set_bitmap_width(other->bitmap_width());
        ctrl->set_bitmap_height(other->bitmap_height());
        ctrl->set_texture_border(other->texture_border());
    }
    return 0;
}

// clang-format off
const MethodEntry ui_bitmap_methods[] = {
    {"SetNewTexture",           bitmap_SetNewTexture},
    {"InternalSetSolidColor",   bitmap_InternalSetSolidColor},
    {"SetColorMask",            bitmap_SetColorMask},
    {"SetUV",                   bitmap_SetUV},
    {"SetTiled",                bitmap_SetTiled},
    {"UseAlphaHitTest",         bitmap_UseAlphaHitTest},
    {"BitmapWidth",             bitmap_BitmapWidth},
    {"BitmapHeight",            bitmap_BitmapHeight},
    {"SetFrame",                bitmap_SetFrame},
    {"GetFrame",                bitmap_GetFrame},
    {"GetNumFrames",            bitmap_GetNumFrames},
    {"SetFrameRate",            bitmap_SetFrameRate},
    {"SetFramePattern",         bitmap_SetFramePattern},
    {"SetForwardPattern",       bitmap_SetForwardPattern},
    {"SetBackwardPattern",      bitmap_SetBackwardPattern},
    {"SetPingPongPattern",      bitmap_SetPingPongPattern},
    {"SetLoopPingPongPattern",  bitmap_SetLoopPingPongPattern},
    {"Play",                    bitmap_Play},
    {"Stop",                    bitmap_Stop},
    {"Loop",                    bitmap_Loop},
    {"ShareTextures",           bitmap_ShareTextures},
    {nullptr, nullptr},
};
// clang-format on

// ====================================================================
// M75: Border
// ====================================================================

static int border_SetNewTextures(lua_State* L) {
    auto* ctrl = check_control(L);
    if (!ctrl) return 0;
    if (lua_type(L, 2) == LUA_TSTRING)
        ctrl->set_border_tex_vert(lua_tostring(L, 2));
    if (lua_type(L, 3) == LUA_TSTRING)
        ctrl->set_border_tex_horiz(lua_tostring(L, 3));
    if (lua_type(L, 4) == LUA_TSTRING)
        ctrl->set_border_tex_ul(lua_tostring(L, 4));
    if (lua_type(L, 5) == LUA_TSTRING)
        ctrl->set_border_tex_ur(lua_tostring(L, 5));
    if (lua_type(L, 6) == LUA_TSTRING)
        ctrl->set_border_tex_ll(lua_tostring(L, 6));
    if (lua_type(L, 7) == LUA_TSTRING)
        ctrl->set_border_tex_lr(lua_tostring(L, 7));

    // Set BorderWidth/BorderHeight from UL corner texture dimensions
    if (!ctrl->border_tex_ul().empty()) {
        auto [w, h] = read_dds_dimensions(L, ctrl->border_tex_ul());
        if (w > 0 && h > 0 && lua_istable(L, 1)) {
            // Set BorderWidth LazyVar via lua_gettable (finds Set on metatable)
            lua_pushstring(L, "BorderWidth");
            lua_gettable(L, 1);
            if (lua_istable(L, -1)) {
                lua_pushstring(L, "Set");
                lua_gettable(L, -2);
                if (lua_isfunction(L, -1)) {
                    lua_pushvalue(L, -2); // LazyVar table as self
                    lua_pushnumber(L, static_cast<f64>(w));
                    lua_pcall(L, 2, 0, 0);
                } else {
                    lua_pop(L, 1);
                }
            }
            lua_pop(L, 1);
            // Set BorderHeight LazyVar
            lua_pushstring(L, "BorderHeight");
            lua_gettable(L, 1);
            if (lua_istable(L, -1)) {
                lua_pushstring(L, "Set");
                lua_gettable(L, -2);
                if (lua_isfunction(L, -1)) {
                    lua_pushvalue(L, -2);
                    lua_pushnumber(L, static_cast<f64>(h));
                    lua_pcall(L, 2, 0, 0);
                } else {
                    lua_pop(L, 1);
                }
            }
            lua_pop(L, 1);
        }
    }
    return 0;
}

static int border_SetSolidColor(lua_State* L) {
    auto* ctrl = check_control(L);
    if (!ctrl) return 0;
    if (lua_type(L, 2) == LUA_TSTRING) {
        u32 color = parse_color_hex(lua_tostring(L, 2));
        ctrl->set_border_solid_color(color);
        ctrl->set_has_border_solid_color(true);
    }
    return 0;
}

// clang-format off
const MethodEntry ui_border_methods[] = {
    {"SetNewTextures", border_SetNewTextures},
    {"SetSolidColor",  border_SetSolidColor},
    {nullptr, nullptr},
};
// clang-format on

// ====================================================================
// M75: Dragger
// ====================================================================

static int dragger_Destroy(lua_State* L) {
    auto* ctrl = check_control(L);
    if (!ctrl) return 0;
    ctrl->mark_destroyed();
    return 0;
}

// clang-format off
const MethodEntry ui_dragger_methods[] = {
    {"Destroy", dragger_Destroy},
    {nullptr, nullptr},
};
// clang-format on

// ====================================================================
// M75: Cursor
// ====================================================================

static int cursor_SetNewTexture(lua_State* L) {
    auto* ctrl = check_control(L);
    if (!ctrl) return 0;
    if (lua_type(L, 2) == LUA_TSTRING) {
        std::string path = lua_tostring(L, 2);
        ctrl->set_cursor_texture(path);
        auto [w, h] = read_dds_dimensions(L, path);
        ctrl->set_bitmap_width(w);
        ctrl->set_bitmap_height(h);
    }
    f32 hx = lua_isnumber(L, 3) ? static_cast<f32>(lua_tonumber(L, 3)) : 0.0f;
    f32 hy = lua_isnumber(L, 4) ? static_cast<f32>(lua_tonumber(L, 4)) : 0.0f;
    ctrl->set_cursor_hotspot(hx, hy);
    return 0;
}

static int cursor_SetDefaultTexture(lua_State* L) {
    auto* ctrl = check_control(L);
    if (!ctrl) return 0;
    if (lua_type(L, 2) == LUA_TSTRING)
        ctrl->set_cursor_default_texture(lua_tostring(L, 2));
    f32 hx = lua_isnumber(L, 3) ? static_cast<f32>(lua_tonumber(L, 3)) : 0.0f;
    f32 hy = lua_isnumber(L, 4) ? static_cast<f32>(lua_tonumber(L, 4)) : 0.0f;
    ctrl->set_cursor_default_hotspot(hx, hy);
    return 0;
}

static int cursor_ResetToDefault(lua_State* L) {
    auto* ctrl = check_control(L);
    if (!ctrl) return 0;
    ctrl->set_cursor_texture(ctrl->cursor_default_texture());
    ctrl->set_cursor_hotspot(ctrl->cursor_default_hotspot_x(),
                              ctrl->cursor_default_hotspot_y());
    return 0;
}

static int cursor_Show(lua_State* L) {
    auto* ctrl = check_control(L);
    if (!ctrl) return 0;
    ctrl->set_cursor_visible(true);
    return 0;
}

static int cursor_Hide(lua_State* L) {
    auto* ctrl = check_control(L);
    if (!ctrl) return 0;
    ctrl->set_cursor_visible(false);
    return 0;
}

// clang-format off
const MethodEntry ui_cursor_methods[] = {
    {"SetNewTexture",     cursor_SetNewTexture},
    {"SetDefaultTexture", cursor_SetDefaultTexture},
    {"ResetToDefault",    cursor_ResetToDefault},
    {"Show",              cursor_Show},
    {"Hide",              cursor_Hide},
    {nullptr, nullptr},
};
// clang-format on

// ====================================================================
// M75: Movie (stub — video playback not implemented)
// ====================================================================

static int movie_InternalSet(lua_State* L) {
    auto* ctrl = check_control(L);
    if (!ctrl) return 0;
    const char* path = lua_type(L, 2) == LUA_TSTRING ? lua_tostring(L, 2) : nullptr;
    if (!path) { lua_pushboolean(L, 0); return 1; }
    ctrl->set_movie_filename(path);

    // Get VFS via LuaState helper
    auto* vfs = lua::LuaState::get_vfs(L);
    if (vfs) {
        // Try .mpg version first (pre-converted), then original path
        std::string mpg_path(path);
        auto dot = mpg_path.rfind('.');
        if (dot != std::string::npos)
            mpg_path = mpg_path.substr(0, dot) + ".mpg";

        auto data = vfs->read_file(mpg_path);
        if (!data) data = vfs->read_file(path);

        if (data) {
            auto dec = std::make_unique<osc::video::VideoDecoder>();
            if (dec->open_file(reinterpret_cast<const osc::u8*>(data->data()), data->size())) {
                spdlog::info("MovieControl: opened '{}' ({}x{})",
                             path, dec->width(), dec->height());
                ctrl->set_video_decoder(std::move(dec));
                ctrl->set_movie_loaded(true);
            } else {
                spdlog::warn("MovieControl: failed to decode '{}'", path);
            }
        } else {
            spdlog::warn("MovieControl: file not found '{}'", path);
        }
    }
    lua_pushboolean(L, ctrl->movie_loaded() ? 1 : 0);
    return 1;
}

static int movie_IsLoaded(lua_State* L) {
    auto* ctrl = check_control(L);
    lua_pushboolean(L, ctrl ? ctrl->movie_loaded() : 0);
    return 1;
}

static int movie_Play(lua_State* L) {
    auto* ctrl = check_control(L);
    if (ctrl) {
        ctrl->set_movie_playing(true);
        if (ctrl->video_decoder() && ctrl->video_decoder()->is_open()) {
            ctrl->video_decoder()->decode_next_frame();
            ctrl->set_video_needs_upload(true);
        }
    }
    return 0;
}

static int movie_Stop(lua_State* L) {
    auto* ctrl = check_control(L);
    if (ctrl) ctrl->set_movie_playing(false);
    return 0;
}

static int movie_Loop(lua_State* L) {
    auto* ctrl = check_control(L);
    if (ctrl) {
        bool loop = lua_toboolean(L, 2) != 0;
        ctrl->set_movie_looping(loop);
        if (ctrl->video_decoder())
            ctrl->video_decoder()->set_loop(loop);
    }
    return 0;
}

static int movie_GetFrameRate(lua_State* L) {
    auto* ctrl = check_control(L);
    if (ctrl && ctrl->video_decoder() && ctrl->video_decoder()->is_open())
        lua_pushnumber(L, ctrl->video_decoder()->framerate());
    else
        lua_pushnumber(L, 30.0);
    return 1;
}

static int movie_GetNumFrames(lua_State* L) {
    lua_pushnumber(L, 0); // stub: no frames
    return 1;
}

// clang-format off
const MethodEntry ui_movie_methods[] = {
    {"InternalSet",  movie_InternalSet},
    {"IsLoaded",     movie_IsLoaded},
    {"Play",         movie_Play},
    {"Stop",         movie_Stop},
    {"Loop",         movie_Loop},
    {"GetFrameRate", movie_GetFrameRate},
    {"GetNumFrames", movie_GetNumFrames},
    {nullptr, nullptr},
};
// clang-format on

// ====================================================================
// M75: Histogram (deprecated stub)
// ====================================================================

static int histogram_SetData(lua_State* L) {
    (void)L;
    return 0; // no-op
}

static int histogram_SetXIncrement(lua_State* L) {
    (void)L;
    return 0;
}

static int histogram_SetYIncrement(lua_State* L) {
    (void)L;
    return 0;
}

// clang-format off
const MethodEntry ui_histogram_methods[] = {
    {"SetData",       histogram_SetData},
    {"SetXIncrement", histogram_SetXIncrement},
    {"SetYIncrement", histogram_SetYIncrement},
    {nullptr, nullptr},
};
// clang-format on

} // namespace osc::lua
