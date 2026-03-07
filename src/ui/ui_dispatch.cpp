#include "ui/ui_dispatch.hpp"
#include "ui/ui_control.hpp"

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

void UIDispatch::dispatch_events(lua_State* L, UIControlRegistry& registry) {
    if (pending_events_.empty()) return;

    for (const auto& ev : pending_events_) {
        // Keyboard events go to the control with keyboard focus
        if (ev.type == UIEventType::KEY_DOWN ||
            ev.type == UIEventType::KEY_UP ||
            ev.type == UIEventType::CHAR) {
            auto* focus = registry.keyboard_focus();
            if (focus && focus->lua_table_ref() >= 0) {
                lua_rawgeti(L, LUA_REGISTRYINDEX, focus->lua_table_ref());
                lua_pushstring(L, "HandleEvent");
                lua_rawget(L, -2);
                if (lua_isfunction(L, -1)) {
                    lua_pushvalue(L, -2); // self
                    push_event_table(L, ev);
                    if (lua_pcall(L, 2, 1, 0) != 0) {
                        spdlog::warn("HandleEvent error: {}",
                                     lua_tostring(L, -1));
                        lua_pop(L, 1);
                    } else {
                        lua_pop(L, 1); // return value
                    }
                } else {
                    lua_pop(L, 1);
                }
                lua_pop(L, 1); // control table
            }
            continue;
        }

        // Mouse events: for now just dispatch to keyboard focus control
        // (full hit-test walk deferred to when we have visual controls)
        auto* focus = registry.keyboard_focus();
        if (focus && focus->lua_table_ref() >= 0) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, focus->lua_table_ref());
            lua_pushstring(L, "HandleEvent");
            lua_rawget(L, -2);
            if (lua_isfunction(L, -1)) {
                lua_pushvalue(L, -2);
                push_event_table(L, ev);
                if (lua_pcall(L, 2, 1, 0) != 0) {
                    spdlog::warn("HandleEvent error: {}",
                                 lua_tostring(L, -1));
                    lua_pop(L, 1);
                } else {
                    lua_pop(L, 1);
                }
            } else {
                lua_pop(L, 1);
            }
            lua_pop(L, 1);
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
