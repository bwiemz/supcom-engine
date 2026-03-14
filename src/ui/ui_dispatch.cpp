#include "ui/ui_dispatch.hpp"
#include "ui/ui_control.hpp"
#include "ui/keymap.hpp"

#include <GLFW/glfw3.h>
#include <spdlog/spdlog.h>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

namespace osc::ui {

// File-static pointer for safe GLFW callback routing without conflicting
// with Renderer's glfwSetWindowUserPointer.
static UIDispatch* s_dispatch = nullptr;

void UIDispatch::install_callbacks(GLFWwindow* window) {
    s_dispatch = this;
    glfwSetKeyCallback(window, [](GLFWwindow*, int key, int /*scancode*/,
                                   int action, int mods) {
        if (s_dispatch) s_dispatch->on_key(key, action, mods);
    });
    glfwSetMouseButtonCallback(window, [](GLFWwindow*, int button,
                                           int action, int mods) {
        if (s_dispatch) s_dispatch->on_mouse_button(button, action, mods);
    });
    glfwSetCursorPosCallback(window, [](GLFWwindow*, double x, double y) {
        if (s_dispatch) s_dispatch->on_cursor_pos(x, y);
    });
    glfwSetCharCallback(window, [](GLFWwindow*, unsigned int cp) {
        if (s_dispatch) s_dispatch->on_char(cp);
    });
    spdlog::debug("UIDispatch: installed GLFW callbacks via static pointer");
}

void UIDispatch::on_key(i32 key, i32 action, i32 mods) {
    UIEvent e;
    e.type = (action == GLFW_RELEASE) ? UIEventType::KEY_UP
                                       : UIEventType::KEY_DOWN;
    e.key_code = key;
    e.mouse_x = mouse_x_;
    e.mouse_y = mouse_y_;
    e.modifiers = mods;
    e.is_repeat = (action == GLFW_REPEAT);
    pending_events_.push_back(e);
}

void UIDispatch::on_mouse_button(i32 button, i32 action, i32 mods) {
    UIEvent e;
    e.type = (action == GLFW_RELEASE) ? UIEventType::BUTTON_RELEASE
                                       : UIEventType::BUTTON_PRESS;
    e.key_code = button;
    e.mouse_x = mouse_x_;
    e.mouse_y = mouse_y_;
    e.modifiers = mods;
    pending_events_.push_back(e);
}

void UIDispatch::on_cursor_pos(f64 x, f64 y) {
    mouse_x_ = x;
    mouse_y_ = y;
    UIEvent e;
    e.type = UIEventType::MOUSE_MOTION;
    e.mouse_x = x;
    e.mouse_y = y;
    pending_events_.push_back(e);
}

void UIDispatch::on_scroll(f64 y_offset) {
    UIEvent e;
    e.type = UIEventType::MOUSE_WHEEL;
    e.mouse_x = mouse_x_;
    e.mouse_y = mouse_y_;
    e.wheel_delta = y_offset;
    pending_events_.push_back(e);
}

void UIDispatch::on_char(u32 codepoint) {
    UIEvent e;
    e.type = UIEventType::CHAR;
    e.char_code = codepoint;
    e.mouse_x = mouse_x_;
    e.mouse_y = mouse_y_;
    pending_events_.push_back(e);
}

/// Push a Lua event table for the given UIEvent.
/// FA convention: {Type='ButtonPress', KeyCode=N, MouseX=N, MouseY=N, Modifiers={...}}
static void push_event_table(lua_State* L, const UIEvent& ev) {
    lua_newtable(L);

    // Type string
    const char* type_str = "Unknown";
    switch (ev.type) {
    case UIEventType::KEY_DOWN:       type_str = "KeyDown"; break;
    case UIEventType::KEY_UP:         type_str = "KeyUp"; break;
    case UIEventType::MOUSE_MOTION:   type_str = "MouseMotion"; break;
    case UIEventType::BUTTON_PRESS:   type_str = "ButtonPress"; break;
    case UIEventType::BUTTON_RELEASE: type_str = "ButtonRelease"; break;
    case UIEventType::MOUSE_WHEEL:    type_str = "WheelRotation"; break;
    case UIEventType::CHAR:           type_str = "Char"; break;
    }
    lua_pushstring(L, "Type");
    lua_pushstring(L, type_str);
    lua_rawset(L, -3);

    lua_pushstring(L, "KeyCode");
    lua_pushnumber(L, ev.key_code);
    lua_rawset(L, -3);

    lua_pushstring(L, "MouseX");
    lua_pushnumber(L, ev.mouse_x);
    lua_rawset(L, -3);

    lua_pushstring(L, "MouseY");
    lua_pushnumber(L, ev.mouse_y);
    lua_rawset(L, -3);

    if (ev.type == UIEventType::MOUSE_WHEEL) {
        lua_pushstring(L, "WheelRotation");
        lua_pushnumber(L, ev.wheel_delta);
        lua_rawset(L, -3);
    }

    if (ev.type == UIEventType::CHAR) {
        lua_pushstring(L, "CharCode");
        lua_pushnumber(L, ev.char_code);
        lua_rawset(L, -3);
    }

    // Modifiers table
    lua_pushstring(L, "Modifiers");
    lua_newtable(L);
    if (ev.modifiers & GLFW_MOD_SHIFT) {
        lua_pushstring(L, "Shift");
        lua_pushboolean(L, 1);
        lua_rawset(L, -3);
    }
    if (ev.modifiers & GLFW_MOD_CONTROL) {
        lua_pushstring(L, "Ctrl");
        lua_pushboolean(L, 1);
        lua_rawset(L, -3);
    }
    if (ev.modifiers & GLFW_MOD_ALT) {
        lua_pushstring(L, "Alt");
        lua_pushboolean(L, 1);
        lua_rawset(L, -3);
    }
    lua_rawset(L, -3);
}

/// Read a LazyVar float from a control's Lua table.
static f32 read_lazyvar_dispatch(lua_State* L, int tbl_idx, const char* field) {
    lua_pushstring(L, field);
    lua_rawget(L, tbl_idx);
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return 0.0f; }
    if (lua_pcall(L, 0, 1, 0) != 0) { lua_pop(L, 1); return 0.0f; }
    f32 val = static_cast<f32>(lua_tonumber(L, -1));
    lua_pop(L, 1);
    return val;
}

UIControl* UIDispatch::hit_test(lua_State* L, UIControl* root, f64 x, f64 y) {
    if (!root || root->hidden() || root->destroyed()) return nullptr;
    if (root->lua_table_ref() < 0) return nullptr;

    // Walk children front-to-back (last child is topmost)
    auto& children = root->children();
    for (i32 i = static_cast<i32>(children.size()) - 1; i >= 0; --i) {
        auto* hit = hit_test(L, children[i], x, y);
        if (hit) return hit;
    }

    // Check this control's bounds
    if (root->hit_test_disabled()) return nullptr;

    lua_rawgeti(L, LUA_REGISTRYINDEX, root->lua_table_ref());
    int tbl = lua_gettop(L);
    f32 left = read_lazyvar_dispatch(L, tbl, "Left");
    f32 top = read_lazyvar_dispatch(L, tbl, "Top");
    f32 w = read_lazyvar_dispatch(L, tbl, "Width");
    f32 h = read_lazyvar_dispatch(L, tbl, "Height");
    lua_pop(L, 1);

    f32 fx = static_cast<f32>(x);
    f32 fy = static_cast<f32>(y);
    if (fx >= left && fx < left + w && fy >= top && fy < top + h)
        return root;
    return nullptr;
}

bool UIDispatch::fire_handle_event(lua_State* L, UIControl* ctrl,
                                    const UIEvent& ev) {
    if (!ctrl || ctrl->lua_table_ref() < 0) return false;

    lua_rawgeti(L, LUA_REGISTRYINDEX, ctrl->lua_table_ref());
    lua_pushstring(L, "HandleEvent");
    lua_rawget(L, -2);
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 2);
        return false;
    }
    lua_pushvalue(L, -2); // self
    push_event_table(L, ev);
    if (lua_pcall(L, 2, 1, 0) != 0) {
        spdlog::warn("HandleEvent error: {}", lua_tostring(L, -1));
        lua_pop(L, 2);
        return false;
    }
    bool consumed = lua_toboolean(L, -1) != 0;
    lua_pop(L, 2); // return value + control table
    return consumed;
}

void UIDispatch::dispatch_events(lua_State* L, UIControlRegistry& registry) {
    if (pending_events_.empty()) return;

    // Get root frame for hit testing
    UIControl* root = nullptr;
    lua_pushstring(L, "__osc_root_frame");
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (lua_istable(L, -1)) {
        lua_pushstring(L, "_c_object");
        lua_rawget(L, -2);
        if (lua_islightuserdata(L, -1))
            root = static_cast<UIControl*>(lua_touserdata(L, -1));
        lua_pop(L, 1);
    }
    lua_pop(L, 1);

    for (const auto& ev : pending_events_) {
        // Clear stale hover pointer if the control was destroyed (e.g. by Lua)
        if (hover_control_ && hover_control_->destroyed())
            hover_control_ = nullptr;

        // Check for active dragger — intercepts mouse events
        lua_pushstring(L, "__osc_active_dragger");
        lua_rawget(L, LUA_REGISTRYINDEX);
        bool has_dragger = lua_istable(L, -1);
        int dragger_idx = lua_gettop(L);

        if (has_dragger) {
            bool handled = false;
            if (ev.type == UIEventType::MOUSE_MOTION) {
                lua_pushstring(L, "OnMove");
                lua_rawget(L, dragger_idx);
                if (lua_isfunction(L, -1)) {
                    lua_pushvalue(L, dragger_idx);
                    lua_pushnumber(L, ev.mouse_x);
                    lua_pushnumber(L, ev.mouse_y);
                    if (lua_pcall(L, 3, 0, 0) != 0) lua_pop(L, 1);
                } else {
                    lua_pop(L, 1);
                }
                handled = true;
            } else if (ev.type == UIEventType::BUTTON_RELEASE) {
                lua_pushstring(L, "OnRelease");
                lua_rawget(L, dragger_idx);
                if (lua_isfunction(L, -1)) {
                    lua_pushvalue(L, dragger_idx);
                    lua_pushnumber(L, ev.mouse_x);
                    lua_pushnumber(L, ev.mouse_y);
                    if (lua_pcall(L, 3, 0, 0) != 0) lua_pop(L, 1);
                } else {
                    lua_pop(L, 1);
                }
                // Clear active dragger
                lua_pushstring(L, "__osc_active_dragger");
                lua_pushnil(L);
                lua_rawset(L, LUA_REGISTRYINDEX);
                handled = true;
            } else if (ev.type == UIEventType::KEY_DOWN && ev.key_code == 256) {
                // ESC = GLFW_KEY_ESCAPE = 256
                lua_pushstring(L, "OnCancel");
                lua_rawget(L, dragger_idx);
                if (lua_isfunction(L, -1)) {
                    lua_pushvalue(L, dragger_idx);
                    if (lua_pcall(L, 1, 0, 0) != 0) lua_pop(L, 1);
                } else {
                    lua_pop(L, 1);
                }
                lua_pushstring(L, "__osc_active_dragger");
                lua_pushnil(L);
                lua_rawset(L, LUA_REGISTRYINDEX);
                handled = true;
            }
            lua_pop(L, 1); // pop dragger table
            if (handled) continue;
        }
        if (!has_dragger) lua_pop(L, 1); // pop nil

        // Keyboard events go to keyboard focus control first
        if (ev.type == UIEventType::KEY_DOWN ||
            ev.type == UIEventType::KEY_UP ||
            ev.type == UIEventType::CHAR) {
            auto* focus = registry.keyboard_focus();
            bool consumed = false;
            if (focus) consumed = fire_handle_event(L, focus, ev);

            // If fresh KEY_DOWN not consumed by UI, try key map registry (hotkeys)
            // Skip repeats (held keys) — hotkeys should only fire on initial press
            if (!consumed && ev.type == UIEventType::KEY_DOWN && !ev.is_repeat) {
                lua_pushstring(L, "__osc_keymap_registry");
                lua_rawget(L, LUA_REGISTRYINDEX);
                auto* km = static_cast<KeyMapRegistry*>(lua_touserdata(L, -1));
                lua_pop(L, 1);
                if (km) {
                    std::string key_name = KeyMapRegistry::glfw_to_key_name(
                        ev.key_code, ev.modifiers);
                    if (!key_name.empty()) {
                        km->dispatch(L, key_name);
                    }
                }
            }
            continue;
        }

        // Mouse events: hit-test the control tree
        UIControl* target = root ? hit_test(L, root, ev.mouse_x, ev.mouse_y)
                                 : nullptr;

        // Mouse enter/exit tracking
        if (ev.type == UIEventType::MOUSE_MOTION && target != hover_control_) {
            if (hover_control_ && !hover_control_->destroyed() &&
                hover_control_->lua_table_ref() >= 0) {
                lua_rawgeti(L, LUA_REGISTRYINDEX, hover_control_->lua_table_ref());
                lua_pushstring(L, "OnMouseExit");
                lua_rawget(L, -2);
                if (lua_isfunction(L, -1)) {
                    lua_pushvalue(L, -2);
                    if (lua_pcall(L, 1, 0, 0) != 0) lua_pop(L, 1);
                } else {
                    lua_pop(L, 1);
                }
                lua_pop(L, 1);
            }
            // Re-validate target after OnMouseExit callback (may have destroyed it)
            if (target && target->destroyed()) target = nullptr;
            if (target && target->lua_table_ref() >= 0) {
                lua_rawgeti(L, LUA_REGISTRYINDEX, target->lua_table_ref());
                lua_pushstring(L, "OnMouseEnter");
                lua_rawget(L, -2);
                if (lua_isfunction(L, -1)) {
                    lua_pushvalue(L, -2);
                    if (lua_pcall(L, 1, 0, 0) != 0) lua_pop(L, 1);
                } else {
                    lua_pop(L, 1);
                }
                lua_pop(L, 1);
            }
            hover_control_ = target;
        }

        // Dispatch to hit target, then walk up ancestors
        UIControl* c = target;
        while (c) {
            if (fire_handle_event(L, c, ev)) break;
            c = c->parent();
        }
    }

    pending_events_.clear();
}

void UIDispatch::update_controls(lua_State* L, UIControlRegistry& registry,
                                  f64 dt) {
    for (auto& ctrl_ptr : registry.all()) {
        if (!ctrl_ptr || ctrl_ptr->destroyed()) continue;
        if (!ctrl_ptr->needs_frame_update()) continue;
        if (ctrl_ptr->lua_table_ref() < 0) continue;

        lua_rawgeti(L, LUA_REGISTRYINDEX, ctrl_ptr->lua_table_ref());
        lua_pushstring(L, "OnFrame");
        lua_rawget(L, -2);
        if (lua_isfunction(L, -1)) {
            lua_pushvalue(L, -2); // self
            lua_pushnumber(L, dt);
            if (lua_pcall(L, 2, 0, 0) != 0) {
                spdlog::warn("OnFrame error for control #{}: {}",
                             ctrl_ptr->control_id(), lua_tostring(L, -1));
                lua_pop(L, 1);
            }
        } else {
            lua_pop(L, 1);
        }
        lua_pop(L, 1); // control table
    }
}

} // namespace osc::ui
