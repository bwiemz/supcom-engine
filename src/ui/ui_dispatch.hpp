#pragma once

#include "core/types.hpp"

#include <unordered_set>

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
    MOUSE_ENTER = 7, // synthetic: hover entered a control
    MOUSE_EXIT = 8,  // synthetic: hover left a control
    /// A press that makes a double-click, in its place (wx's ButtonDClick,
    /// which Moho raises as MET_ButtonDClick; Windows sends it so).
    BUTTON_DCLICK = 9,
};

/// A press or a double-click's press.
inline bool is_press(UIEventType t) {
    return t == UIEventType::BUTTON_PRESS || t == UIEventType::BUTTON_DCLICK;
}

/// Buffered UI event from GLFW callbacks.
struct UIEvent {
    UIEventType type;
    i32 key_code = 0;     // GLFW key code or mouse button
    f64 mouse_x = 0;
    f64 mouse_y = 0;
    i32 modifiers = 0;    // GLFW mod bits
    f64 wheel_delta = 0;
    u32 char_code = 0;    // Unicode codepoint for CHAR events
    bool is_repeat = false; // GLFW_REPEAT (held key) vs fresh press
    /// A mouse event's buttons held once it happened (kMouseLeft...):
    /// a press holds its button, a release no longer does. A key event
    /// has none.
    u8 buttons = 0;
};

constexpr u8 kMouseLeft = 1;
constexpr u8 kMouseMiddle = 2;
constexpr u8 kMouseRight = 4;

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
    /// A press of the same button within kDoubleClickSeconds and
    /// kDoubleClickPixels of the last, not itself one, is a double-click
    /// (Windows' GetDoubleClickTime and SM_CXDOUBLECLK defaults). `now` is the
    /// time in seconds (glfwGetTime when negative).
    void on_mouse_button(i32 button, i32 action, i32 mods, f64 now = -1.0);
    static constexpr f64 kDoubleClickSeconds = 0.5;
    static constexpr f64 kDoubleClickPixels = 2.0;
    void on_cursor_pos(f64 x, f64 y);
    void on_scroll(f64 y_offset);
    void on_char(u32 codepoint);

    /// The control currently under the mouse (for enter/exit tracking).
    UIControl* hover_control() const { return hover_control_; }
    /// Let go of the controls it remembers (their registry is being
    /// replaced with a new UI state).
    void forget_controls() {
        hover_control_ = nullptr;
        thumb_drag_ = nullptr;
        mouseover_list_ = nullptr;
        mouseover_row_ = -1;
    }

    /// Current mouse position (updated by cursor pos callback).
    f64 mouse_x() const { return mouse_x_; }
    f64 mouse_y() const { return mouse_y_; }

    /// Whether the UI, not the world, has the mouse at (x, y): a control
    /// other than the root frame or a world view is under it. Under an input
    /// capture (a modal dialog, retail's opening lock) the mouse is the
    /// capture's unless a world view in it is under the point, as Moho hit-
    /// tests under the capture and never past it.
    bool ui_has_mouse(lua_State* L, UIControlRegistry& registry, f64 x, f64 y);

    /// Find the topmost control at (x, y) via front-to-back tree walk.
    /// If \p skip is non-null, controls in that set are treated as invisible.
    UIControl* hit_test(lua_State* L, UIControl* root, f64 x, f64 y,
                        const std::unordered_set<UIControl*>* skip = nullptr);

private:
    /// Fire HandleEvent on a control. Returns true if event was consumed.
    bool fire_handle_event(lua_State* L, UIControl* ctrl, const UIEvent& ev);
    /// A key or character for a focused Edit taking input: Moho's CMauiEdit
    /// edits its text and calls its On* methods. False: not the Edit's.
    bool edit_event(lua_State* L, UIControlRegistry& registry, UIControl* edit, const UIEvent& ev);
    /// Call the control's method `name` (found through its class) with
    /// `arg`, then `event`, if given; false if it has none.
    bool run_script(lua_State* L, UIControl* ctrl, const char* name, const f64* arg = nullptr,
                    const UIEvent* event = nullptr);
    /// A key going down with no control focused and no capture: the key
    /// map's action, run through the console (CUIKeyHandler::OnKeyDown).
    void handle_key(lua_State* L, const UIEvent& ev);
    /// UI_ActivateChat: chat.lua's ActivateChat(modifiers), in a game.
    void activate_chat(lua_State* L, const UIEvent& ev);
    /// A Movie control's frame (Moho's CMauiMovie::Frame): OnFrame, then
    /// OnStopped, its movie's clock and frame, or OnFinished at its end.
    void movie_frame(lua_State* L, UIControl* ctrl, f64 dt);
    /// A press on a scrollbar (Moho's CMauiScrollbar): its thumb taken to
    /// drag, or a page toward the press on its track
    void press_scrollbar(lua_State* L, UIControl* bar, const UIEvent& ev);
    /// The dragged thumb follows the mouse until its button is let go
    void drag_thumb(lua_State* L, const UIEvent& ev);
    /// The mouse over an ItemList's row (Moho's CMauiItemList): its
    /// OnMouseoverItem(row) when the row changes, -1 once off its rows
    void hover_item_list(lua_State* L, UIControl* target, const UIEvent& ev);

    std::vector<UIEvent> pending_events_;
    f64 mouse_x_ = 0;
    f64 mouse_y_ = 0;
    u8 buttons_down_ = 0; ///< kMouseLeft... held now
    /// The last press, which the next may make a double-click
    f64 last_press_time_ = -1.0;
    f64 last_press_x_ = 0;
    f64 last_press_y_ = 0;
    i32 last_press_button_ = -1;
    bool last_press_double_ = false;
    UIControl* hover_control_ = nullptr;
    UIControl* thumb_drag_ = nullptr; ///< the scrollbar whose thumb is dragged
    f32 thumb_grab_ = 0;              ///< where along its thumb it was taken
    UIControl* mouseover_list_ = nullptr; ///< the ItemList told of a row under the mouse
    i32 mouseover_row_ = -1;              ///< that row
};

} // namespace osc::ui
