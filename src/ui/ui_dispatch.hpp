#pragma once

#include "core/types.hpp"

struct GLFWwindow;
struct lua_State;

namespace osc::ui {

class UIControl;
class UIControlRegistry;

/// UI event types matching FA's Lua event table convention.
enum class UIEventType : u8 {
    KEY_DOWN = 0,
    KEY_UP = 1,
    MOUSE_MOTION = 2,
    BUTTON_PRESS = 3,
    BUTTON_RELEASE = 4,
    MOUSE_WHEEL = 5,
    CHAR = 6,
};

/// Buffered UI event from GLFW callbacks.
struct UIEvent {
    UIEventType type;
    i32 key_code = 0;     // GLFW key code or mouse button
    f64 mouse_x = 0;
    f64 mouse_y = 0;
    i32 modifiers = 0;    // GLFW mod bits
    f64 wheel_delta = 0;
    u32 char_code = 0;    // Unicode codepoint for CHAR events
};

/// Manages GLFW input → UI event dispatch.
/// Install callbacks, buffer events, dispatch to Lua HandleEvent.
class UIDispatch {
public:
    /// Install GLFW key/mouse/char callbacks on the window.
    void install_callbacks(GLFWwindow* window);

    /// Dispatch all buffered events to Lua UI controls.
    /// Called once per frame from the main loop.
    void dispatch_events(lua_State* L, UIControlRegistry& registry);

    /// Call OnFrame on all controls with NeedsFrameUpdate.
    void update_controls(lua_State* L, UIControlRegistry& registry, f64 dt);

    // GLFW callback receivers (public so static callbacks can access)
    void on_key(i32 key, i32 action, i32 mods);
    void on_mouse_button(i32 button, i32 action, i32 mods);
    void on_cursor_pos(f64 x, f64 y);
    void on_scroll(f64 y_offset);
    void on_char(u32 codepoint);

    /// The control currently under the mouse (for enter/exit tracking).
    UIControl* hover_control() const { return hover_control_; }

    /// Current mouse position (updated by cursor pos callback).
    f64 mouse_x() const { return mouse_x_; }
    f64 mouse_y() const { return mouse_y_; }

    /// Find the topmost control at (x, y) via front-to-back tree walk.
    UIControl* hit_test(lua_State* L, UIControl* root, f64 x, f64 y);

private:
    /// Fire HandleEvent on a control. Returns true if event was consumed.
    bool fire_handle_event(lua_State* L, UIControl* ctrl, const UIEvent& ev);

    std::vector<UIEvent> pending_events_;
    f64 mouse_x_ = 0;
    f64 mouse_y_ = 0;
    UIControl* hover_control_ = nullptr;
};

} // namespace osc::ui
