#include "ui/ui_dispatch.hpp"
#include "core/cursor.hpp"
#include "ui/ui_control.hpp"
#include "core/game_state.hpp"
#include "ui/console.hpp"
#include "ui/edit_text.hpp"

#include <functional>
#include "ui/key_codes.hpp"
#include "ui/keymap.hpp"
#include "ui/lazyvar.hpp"
#include "ui/scroll.hpp"
#include "ui/ui_layout.hpp"
#include "ui/world_view.hpp"
#include "core/test_status.hpp"

#include <GLFW/glfw3.h>
#include <spdlog/spdlog.h>

#include <utility>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

namespace {

/// A UI script callback raised an error. In test modes that fails the run,
/// as a dying script thread does (see ThreadManager); in play it is logged.
void report_ui_callback_error(const std::string& what) {
    spdlog::warn("{}", what);
    if (osc::test_status::count_lua_failures())
        osc::test_status::record_failure("Lua UI callback error: " + what);
}

} // namespace


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
    // The cursor in framebuffer pixels, the UI's units (M217h: they differ
    // from the window's on a scaled display)
    glfwSetCursorPosCallback(window, [](GLFWwindow* w, double x, double y) {
        if (!s_dispatch) return;
        int ww = 0;
        int wh = 0;
        int fw = 0;
        int fh = 0;
        glfwGetWindowSize(w, &ww, &wh);
        glfwGetFramebufferSize(w, &fw, &fh);
        const auto p = core::to_framebuffer(x, y, ww, wh, fw, fh);
        s_dispatch->on_cursor_pos(p[0], p[1]);
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
    u8 bit = 0;
    switch (button) {
    case GLFW_MOUSE_BUTTON_LEFT: bit = kMouseLeft; break;
    case GLFW_MOUSE_BUTTON_MIDDLE: bit = kMouseMiddle; break;
    case GLFW_MOUSE_BUTTON_RIGHT: bit = kMouseRight; break;
    default: break;
    }
    if (action == GLFW_RELEASE) buttons_down_ &= static_cast<u8>(~bit);
    else buttons_down_ |= bit;

    UIEvent e;
    e.type = (action == GLFW_RELEASE) ? UIEventType::BUTTON_RELEASE
                                       : UIEventType::BUTTON_PRESS;
    e.key_code = button;
    e.mouse_x = mouse_x_;
    e.mouse_y = mouse_y_;
    e.modifiers = mods;
    e.buttons = buttons_down_;
    pending_events_.push_back(e);
}

void UIDispatch::on_cursor_pos(f64 x, f64 y) {
    mouse_x_ = x;
    mouse_y_ = y;
    UIEvent e;
    e.type = UIEventType::MOUSE_MOTION;
    e.mouse_x = x;
    e.mouse_y = y;
    e.buttons = buttons_down_;
    pending_events_.push_back(e);
}

void UIDispatch::on_scroll(f64 y_offset) {
    UIEvent e;
    e.type = UIEventType::MOUSE_WHEEL;
    e.mouse_x = mouse_x_;
    e.mouse_y = mouse_y_;
    e.wheel_delta = y_offset;
    e.buttons = buttons_down_;
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

/// A key event's, as Moho's are.
static bool is_key_event(const UIEvent& ev) {
    return ev.type == UIEventType::KEY_DOWN || ev.type == UIEventType::KEY_UP;
}

/// The event's KeyCode, as Moho gives it (see key_codes.hpp): a key's wx
/// code, a button's wx number, a character's code, else 0.
static i32 moho_event_key_code(const UIEvent& ev) {
    if (is_key_event(ev)) return moho_key_code(ev.key_code);
    if (ev.type == UIEventType::BUTTON_PRESS || ev.type == UIEventType::BUTTON_RELEASE)
        return moho_mouse_button(ev.key_code);
    if (ev.type == UIEventType::CHAR) return static_cast<i32>(ev.char_code);
    return 0;
}

/// Push a Lua event table for the given UIEvent, as Moho builds it:
/// {Type='ButtonPress', KeyCode=N, RawKeyCode=N, MouseX=N, MouseY=N,
/// Modifiers={Shift, Ctrl, Alt, Left, Middle, Right}}.
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
    case UIEventType::MOUSE_ENTER:    type_str = "MouseEnter"; break;
    case UIEventType::MOUSE_EXIT:     type_str = "MouseExit"; break;
    }
    lua_pushstring(L, "Type");
    lua_pushstring(L, type_str);
    lua_rawset(L, -3);

    lua_pushstring(L, "KeyCode");
    lua_pushnumber(L, moho_event_key_code(ev));
    lua_rawset(L, -3);

    lua_pushstring(L, "RawKeyCode");
    lua_pushnumber(L, is_key_event(ev) ? windows_key_code(ev.key_code) : 0);
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
    // The buttons held (a mouse event's only: Moho builds those modifiers
    // from the mouse's state).
    for (const auto& [bit, name] :
         {std::pair{kMouseLeft, "Left"}, std::pair{kMouseMiddle, "Middle"},
          std::pair{kMouseRight, "Right"}}) {
        if (!(ev.buttons & bit)) continue;
        lua_pushstring(L, name);
        lua_pushboolean(L, 1);
        lua_rawset(L, -3);
    }
    lua_rawset(L, -3);
}

namespace {

/// Walk the visible tree under `ctrl`, keeping the deepest hit-testable
/// control whose rect contains (x, y). Parents are visited before their
/// children and siblings in the order they were made, and only a deeper
/// control replaces the one kept, so the first one wins a tie, as in
/// Moho's CMauiControl::GetTopmostControl (a depth-first walk, children
/// appended as they are made, `>`). Retail's score screen relies on it:
/// its page group fills the panel at the Continue button's depth, made
/// after it.
void collect_hit(lua_State* L, UIControl* ctrl, f32 x, f32 y,
                 const std::unordered_set<UIControl*>* skip, UIControl*& best,
                 f32& best_depth) {
    if (!ctrl || ctrl->hidden() || ctrl->destroyed()) return;
    if (ctrl->lua_table_ref() < 0) return;

    if (!ctrl->hit_test_disabled() && !(skip && skip->count(ctrl))) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, ctrl->lua_table_ref());
        const int tbl = lua_gettop(L);
        const f32 left = read_lazyvar(L, tbl, "Left");
        const f32 top = read_lazyvar(L, tbl, "Top");
        const auto rect =
            control_rect(left, top, read_lazyvar(L, tbl, "Right"), read_lazyvar(L, tbl, "Bottom"),
                         read_lazyvar(L, tbl, "Width"), read_lazyvar(L, tbl, "Height"));
        const bool inside = x >= rect.x && x < rect.x + rect.w &&
                            y >= rect.y && y < rect.y + rect.h;
        const f32 depth = inside ? read_lazyvar(L, tbl, "Depth") : 0.0f;
        lua_pop(L, 1);
        if (inside && (!best || depth > best_depth)) {
            best = ctrl;
            best_depth = depth;
        }
    }
    for (auto* child : ctrl->children())
        collect_hit(L, child, x, y, skip, best, best_depth);
}

} // namespace

// Moho hit-tests by depth: of the visible, hit-testable controls under the
// point, the deepest wins -- not the last in the tree. Retail relies on it
// (a panel created early with a raised Depth sits above later siblings).
UIControl* UIDispatch::hit_test(lua_State* L, UIControl* root, f64 x, f64 y,
                                const std::unordered_set<UIControl*>* skip) {
    UIControl* best = nullptr;
    f32 best_depth = 0.0f;
    collect_hit(L, root, static_cast<f32>(x), static_cast<f32>(y), skip, best,
                best_depth);
    return best;
}

bool UIDispatch::ui_has_mouse(lua_State* L, UIControlRegistry& registry, f64 x, f64 y) {
    if (auto* capture = registry.input_capture()) {
        auto* hit = hit_test(L, capture, x, y);
        return !dynamic_cast<WorldView*>(hit);
    }
    lua_pushstring(L, "__osc_root_frame");
    lua_rawget(L, LUA_REGISTRYINDEX);
    UIControl* root = nullptr;
    if (lua_istable(L, -1)) {
        lua_pushstring(L, "_c_object");
        lua_rawget(L, -2);
        root = static_cast<UIControl*>(lua_touserdata(L, -1));
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    if (!root) return false;
    auto* hit = hit_test(L, root, x, y);
    return hit && hit != root && !dynamic_cast<WorldView*>(hit);
}

namespace {

/// The row of an ItemList under a mouse event; -1 off its rows
i32 item_list_row(lua_State* L, const UIControl& list, const UIEvent& ev) {
    i32 row = -1;
    lua_rawgeti(L, LUA_REGISTRYINDEX, list.lua_table_ref());
    const int tbl = lua_gettop(L);
    if (lua_istable(L, tbl)) {
        const auto rect =
            control_rect(read_lazyvar(L, tbl, "Left"), read_lazyvar(L, tbl, "Top"),
                         read_lazyvar(L, tbl, "Right"), read_lazyvar(L, tbl, "Bottom"),
                         read_lazyvar(L, tbl, "Width"), read_lazyvar(L, tbl, "Height"));
        if (ev.mouse_x >= rect.x && ev.mouse_x < rect.x + rect.w) {
            row = item_list_row_at(list, rect.h, static_cast<f32>(ev.mouse_y - rect.y));
        }
    }
    lua_settop(L, tbl - 1);
    return row;
}

} // namespace

bool UIDispatch::fire_handle_event(lua_State* L, UIControl* ctrl,
                                    const UIEvent& ev) {
    if (!ctrl || ctrl->lua_table_ref() < 0) return false;

    lua_rawgeti(L, LUA_REGISTRYINDEX, ctrl->lua_table_ref());
    lua_pushstring(L, "HandleEvent");
    lua_gettable(L, -2);  // gettable for metatable lookup (HandleEvent is on class)
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 2);
        return false;
    }
    lua_pushvalue(L, -2); // self
    push_event_table(L, ev);
    if (lua_pcall(L, 2, 1, 0) != 0) {
        report_ui_callback_error(fmt::format("HandleEvent error: {}",
                                             lua_tostring(L, -1)));
        lua_pop(L, 2);
        return false;
    }
    bool consumed = lua_toboolean(L, -1) != 0;
    lua_pop(L, 2); // return value + control table
    return consumed;
}

namespace {

bool call_edit(lua_State* L, UIControl* edit, const char* method, int nargs,
               const std::function<void()>& push) {
    if (edit->lua_table_ref() < 0) {
        return false;
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, edit->lua_table_ref());
    lua_pushstring(L, method);
    lua_gettable(L, -2);
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 2);
        return false;
    }
    lua_pushvalue(L, -2);
    push();
    if (lua_pcall(L, 1 + nargs, 1, 0) != 0) {
        report_ui_callback_error(fmt::format("{} error: {}", method, lua_tostring(L, -1)));
        lua_pop(L, 2);
        return false;
    }
    const bool handled = lua_toboolean(L, -1) != 0;
    lua_pop(L, 2);
    return handled;
}

} // namespace

bool UIDispatch::edit_event(lua_State* L, UIControl* edit, const UIEvent& ev) {
    if (edit->control_type() != UIControl::ControlType::Edit || !edit->input_enabled() ||
        ev.type == UIEventType::KEY_UP) {
        return false;
    }
    EditText t{edit->text_content(), edit->caret_position()};
    const std::string old = t.text;
    const auto push_text = [&] { lua_pushstring(L, t.text.c_str()); };
    const auto char_pressed = [&](u32 code) {
        return call_edit(L, edit, "OnCharPressed", 1,
                         [&] { lua_pushnumber(L, static_cast<lua_Number>(code)); });
    };
    if (ev.type == UIEventType::CHAR) {
        if (ev.char_code < 32 || ev.char_code == 127 || char_pressed(ev.char_code)) {
            return true;
        }
        t.insert(ev.char_code, edit->max_chars());
    } else {
        switch (ev.key_code) {
        case GLFW_KEY_BACKSPACE: t.erase_before(); break;
        case GLFW_KEY_DELETE: t.erase_after(); break;
        case GLFW_KEY_LEFT: t.left(); break;
        case GLFW_KEY_RIGHT: t.right(); break;
        case GLFW_KEY_HOME: t.home(); break;
        case GLFW_KEY_END: t.end(); break;
        case GLFW_KEY_TAB: char_pressed('\t'); return true;
        case GLFW_KEY_ENTER:
        case GLFW_KEY_KP_ENTER: call_edit(L, edit, "OnEnterPressed", 1, push_text); return true;
        case GLFW_KEY_ESCAPE:
            if (call_edit(L, edit, "OnEscPressed", 1, push_text)) {
                return true;
            }
            t.text.clear();
            t.caret = 0;
            break;
        default:
            if (ev.key_code >= GLFW_KEY_SPACE && ev.key_code <= GLFW_KEY_GRAVE_ACCENT &&
                (ev.modifiers & (GLFW_MOD_CONTROL | GLFW_MOD_ALT | GLFW_MOD_SUPER)) == 0) {
                return true;
            }
            call_edit(L, edit, "OnNonTextKeyPressed", 2, [&] {
                lua_pushnumber(L, windows_key_code(ev.key_code));
                push_event_table(L, ev);
            });
            return true;
        }
    }
    edit->set_caret_position(t.caret);
    if (t.text != old) {
        edit->set_text_content(t.text);
        call_edit(L, edit, "OnTextChanged", 2, [&] {
            push_text();
            lua_pushstring(L, old.c_str());
        });
    }
    return true;
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

    // Swap events into a local copy before processing.  Lua pcall during
    // event handling (e.g. ButtonSkirmish → lobby import) can trigger GLFW
    // callbacks that push_back into pending_events_, invalidating iterators.
    std::vector<UIEvent> events;
    events.swap(pending_events_);

    for (const auto& ev : events) {
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
                lua_gettable(L, dragger_idx);
                if (lua_isfunction(L, -1)) {
                    lua_pushvalue(L, dragger_idx);
                    lua_pushnumber(L, ev.mouse_x);
                    lua_pushnumber(L, ev.mouse_y);
                    if (lua_pcall(L, 3, 0, 0) != 0) {
                        report_ui_callback_error(
                            fmt::format("Dragger OnMove error: {}", lua_tostring(L, -1)));
                        lua_pop(L, 1);
                    }
                } else {
                    lua_pop(L, 1);
                }
                handled = true;
            } else if (ev.type == UIEventType::BUTTON_RELEASE) {
                lua_pushstring(L, "OnRelease");
                lua_gettable(L, dragger_idx);
                if (lua_isfunction(L, -1)) {
                    lua_pushvalue(L, dragger_idx);
                    lua_pushnumber(L, ev.mouse_x);
                    lua_pushnumber(L, ev.mouse_y);
                    if (lua_pcall(L, 3, 0, 0) != 0) {
                        report_ui_callback_error(
                            fmt::format("Dragger OnRelease error: {}", lua_tostring(L, -1)));
                        lua_pop(L, 1);
                    }
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
                lua_gettable(L, dragger_idx);
                if (lua_isfunction(L, -1)) {
                    lua_pushvalue(L, dragger_idx);
                    if (lua_pcall(L, 1, 0, 0) != 0) {
                        report_ui_callback_error(
                            fmt::format("Dragger OnCancel error: {}", lua_tostring(L, -1)));
                        lua_pop(L, 1);
                    }
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

        // A dragged scrollbar thumb has the mouse until its button is let go
        if (thumb_drag_ && thumb_drag_->destroyed()) thumb_drag_ = nullptr;
        if (thumb_drag_ &&
            (ev.type == UIEventType::MOUSE_MOTION ||
             (ev.type == UIEventType::BUTTON_RELEASE && ev.key_code == GLFW_MOUSE_BUTTON_LEFT))) {
            drag_thumb(L, ev);
            continue;
        }

        // Keys (Moho): the focused control has them, and they go no further;
        // with no focus, the top input capture; with neither, the key map,
        // for a key going down (CUIKeyHandler sits below the controls).
        if (ev.type == UIEventType::KEY_DOWN ||
            ev.type == UIEventType::KEY_UP ||
            ev.type == UIEventType::CHAR) {
            if (auto* focus = registry.keyboard_focus()) {
                if (!edit_event(L, focus, ev)) {
                    fire_handle_event(L, focus, ev);
                }
            } else if (auto* capture = registry.input_capture()) fire_handle_event(L, capture, ev);
            else if (ev.type == UIEventType::KEY_DOWN) handle_key(L, ev);
            continue;
        }

        // Mouse events: hit-test the control tree -- under the top input
        // capture, if any, whose own control takes a point over none of
        // its children (Moho).
        UIControl* const capture = registry.input_capture();
        UIControl* const hit_root = capture ? capture : root;
        UIControl* target = hit_root ? hit_test(L, hit_root, ev.mouse_x, ev.mouse_y) : nullptr;
        if (!target) target = capture;

        // Mouse enter/exit tracking — fire HandleEvent with MouseEnter/MouseExit
        // FA's Button.HandleEvent expects {Type='MouseEnter'} and {Type='MouseExit'}
        if (ev.type == UIEventType::MOUSE_MOTION && target != hover_control_) {
            if (hover_control_ && !hover_control_->destroyed()) {
                UIEvent exit_ev = ev;
                exit_ev.type = UIEventType::MOUSE_EXIT;
                fire_handle_event(L, hover_control_, exit_ev);
            }
            // Re-validate target after exit callback (may have destroyed it)
            if (target && target->destroyed()) target = nullptr;
            if (target) {
                UIEvent enter_ev = ev;
                enter_ev.type = UIEventType::MOUSE_ENTER;
                fire_handle_event(L, target, enter_ev);
            }
            hover_control_ = target;
        }

        if (ev.type == UIEventType::MOUSE_MOTION) {
            hover_item_list(L, target, ev);
        }

        // Call UIMain.OnMouseButtonPress for global click handlers
        // (e.g. Combo close-on-outside-click via AddOnMouseClickedFunc)
        if (ev.type == UIEventType::BUTTON_PRESS) {
            lua_newtable(L);
            lua_pushstring(L, "Type");
            lua_pushstring(L, "ButtonPress");
            lua_rawset(L, -3);
            lua_pushstring(L, "x");
            lua_pushnumber(L, ev.mouse_x);
            lua_rawset(L, -3);
            lua_pushstring(L, "y");
            lua_pushnumber(L, ev.mouse_y);
            lua_rawset(L, -3);
            core::call_ui_callback(L, core::kUiMainModule, "OnMouseButtonPress", 1);
        }

        // An Edit takes the focus on a left press its script leaves, the
        // press going no further (Moho's CMauiEdit): retail's edit.lua asks
        // for none, a field clicked into is the one typed into.
        if (ev.type == UIEventType::BUTTON_PRESS && ev.key_code == GLFW_MOUSE_BUTTON_LEFT &&
            target && !target->destroyed() &&
            target->control_type() == UIControl::ControlType::Edit && target->input_enabled()) {
            if (!fire_handle_event(L, target, ev) && !target->destroyed()) {
                run_script(L, target, "AcquireFocus");
            }
            continue;
        }

        // A scrollbar takes a left press its script leaves
        if (ev.type == UIEventType::BUTTON_PRESS && ev.key_code == GLFW_MOUSE_BUTTON_LEFT &&
            target && !target->destroyed() &&
            target->control_type() == UIControl::ControlType::Scrollbar) {
            if (!fire_handle_event(L, target, ev) && !target->destroyed()) {
                press_scrollbar(L, target, ev);
            }
            continue;
        }

        // An ItemList takes a left press on a row its script leaves: its
        // OnClick(row, event), the press going no further (Moho's
        // CMauiItemList). A Combo's list picks its item so, the Combo under
        // it not toggling its list back.
        if (ev.type == UIEventType::BUTTON_PRESS && ev.key_code == GLFW_MOUSE_BUTTON_LEFT &&
            target && !target->destroyed() &&
            target->control_type() == UIControl::ControlType::ItemList) {
            const i32 row = item_list_row(L, *target, ev);
            if (row >= 0) {
                if (!fire_handle_event(L, target, ev) && !target->destroyed()) {
                    const f64 at = row;
                    run_script(L, target, "OnClick", &at, &ev);
                }
                continue;
            }
        }

        // Dispatch to hit target, then walk up ancestors.
        // If no one consumes a click, retry hit-test skipping the
        // already-tried leaf — this lets sibling controls (e.g. Combo)
        // receive events when an overlapping non-interactive control
        // is on top in child order.
        bool consumed = false;
        std::unordered_set<UIControl*> skip_set;
        constexpr int kMaxRetries = 16;

        for (int attempt = 0; attempt < kMaxRetries && !consumed; ++attempt) {
            if (!target) break;

            UIControl* c = target;
            while (c) {
                if (fire_handle_event(L, c, ev)) { consumed = true; break; }
                c = c->parent();
            }

            // For clicks: retry with the non-consuming leaf skipped
            if (!consumed &&
                (ev.type == UIEventType::BUTTON_PRESS ||
                 ev.type == UIEventType::BUTTON_RELEASE)) {
                skip_set.insert(target);
                target =
                    hit_root ? hit_test(L, hit_root, ev.mouse_x, ev.mouse_y, &skip_set) : nullptr;
            } else {
                break;
            }
        }
    }
}

namespace {

/// A mouse event's place along a scrollbar's track, and the track's length
struct TrackPoint {
    f32 at = 0;
    f32 track = 0;
};

TrackPoint track_point(lua_State* L, const UIControl& bar, const UIEvent& ev) {
    TrackPoint point;
    lua_rawgeti(L, LUA_REGISTRYINDEX, bar.lua_table_ref());
    const int tbl = lua_gettop(L);
    if (lua_istable(L, tbl)) {
        const auto rect =
            control_rect(read_lazyvar(L, tbl, "Left"), read_lazyvar(L, tbl, "Top"),
                         read_lazyvar(L, tbl, "Right"), read_lazyvar(L, tbl, "Bottom"),
                         read_lazyvar(L, tbl, "Width"), read_lazyvar(L, tbl, "Height"));
        const bool vert = bar.scroll_axis() == "Vert";
        point.at = static_cast<f32>(vert ? ev.mouse_y - rect.y : ev.mouse_x - rect.x);
        point.track = vert ? rect.h : rect.w;
    }
    lua_settop(L, tbl - 1);
    return point;
}

/// The thumb where it was drawn; before its first frame, where it would be
ThumbSpan bar_thumb(lua_State* L, const UIControl& bar, f32 track) {
    if (bar.drawn_thumb_length() > 0) {
        return {bar.drawn_thumb_start(), bar.drawn_thumb_length()};
    }
    return thumb_span(scroll_values(L, bar), track, 16.0f);
}

} // namespace

void UIDispatch::press_scrollbar(lua_State* L, UIControl* bar, const UIEvent& ev) {
    const TrackPoint point = track_point(L, *bar, ev);
    const ThumbSpan thumb = bar_thumb(L, *bar, point.track);
    switch (track_part(thumb, point.at)) {
    case TrackPart::Thumb:
        thumb_drag_ = bar;
        thumb_grab_ = point.at - thumb.start;
        break;
    case TrackPart::Before: scroll_by(L, *bar, -1, true); break;
    case TrackPart::After: scroll_by(L, *bar, 1, true); break;
    }
}

void UIDispatch::drag_thumb(lua_State* L, const UIEvent& ev) {
    UIControl* bar = thumb_drag_;
    if (ev.type == UIEventType::BUTTON_RELEASE) {
        thumb_drag_ = nullptr;
        return;
    }
    const TrackPoint point = track_point(L, *bar, ev);
    const ThumbSpan thumb = bar_thumb(L, *bar, point.track);
    scroll_set_top(
        L, *bar,
        dragged_top(scroll_values(L, *bar), point.track, thumb.length, point.at - thumb_grab_));
}

void UIDispatch::hover_item_list(lua_State* L, UIControl* target, const UIEvent& ev) {
    UIControl* const list =
        target && !target->destroyed() && target->control_type() == UIControl::ControlType::ItemList
            ? target
            : nullptr;
    const i32 row = list ? item_list_row(L, *list, ev) : -1;
    if (list == mouseover_list_ && row == mouseover_row_) {
        return;
    }
    if (mouseover_list_ && mouseover_list_ != list && mouseover_row_ >= 0) {
        const f64 none = -1;
        run_script(L, mouseover_list_, "OnMouseoverItem", &none);
    }
    if (list && (list == mouseover_list_ || row >= 0)) {
        const f64 at = row;
        run_script(L, list, "OnMouseoverItem", &at);
    }
    mouseover_list_ = list;
    mouseover_row_ = row;
}

void UIDispatch::update_controls(lua_State* L, UIControlRegistry& registry,
                                  f64 dt) {
    // Snapshot control pointers before iterating.  OnFrame callbacks (e.g.
    // menu animation completing → lobby import) can create hundreds of new
    // controls, invalidating the registry's internal vector references.
    // unique_ptr elements are stable (the UIControl* doesn't move), so raw
    // pointers remain valid — but we must re-check destroyed() after each pcall.
    auto& all = registry.all();
    std::vector<UIControl*> snapshot;
    snapshot.reserve(all.size());
    for (auto& p : all) {
        if (p && !p->destroyed() && p->needs_frame_update() &&
            p->lua_table_ref() >= 0)
            snapshot.push_back(p.get());
    }

    for (auto* ctrl : snapshot) {
        if (!ctrl || ctrl->destroyed() || ctrl->lua_table_ref() < 0) continue;
        if (!ctrl->needs_frame_update()) continue;
        if (ctrl->control_type() == UIControl::ControlType::Movie) {
            movie_frame(L, ctrl, dt);
            continue;
        }

        lua_rawgeti(L, LUA_REGISTRYINDEX, ctrl->lua_table_ref());
        lua_pushstring(L, "OnFrame");
        lua_gettable(L, -2);
        if (lua_isfunction(L, -1)) {
            lua_pushvalue(L, -2); // self
            lua_pushnumber(L, dt);
            if (lua_pcall(L, 2, 0, 0) != 0) {
                report_ui_callback_error(fmt::format(
                    "OnFrame error for control #{}: {}", ctrl->control_id(),
                    lua_tostring(L, -1)));
                lua_pop(L, 1);
            }
        } else {
            lua_pop(L, 1);
        }
        lua_pop(L, 1); // control table
    }
}

void UIDispatch::handle_key(lua_State* L, const UIEvent& ev) {
    // Moho's CUIKeyHandler::OnKeyDown.
    lua_pushstring(L, "__osc_keymap_registry");
    lua_rawget(L, LUA_REGISTRYINDEX);
    const auto* km = static_cast<const KeyMapRegistry*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    if (!km) return;
    const u32 chord = KeyMapRegistry::chord(ev.key_code, ev.modifiers);
    // An auto-repeat acts only for a chord that asks for it (keyRepeat).
    if (ev.is_repeat && !km->repeats(chord)) return;
    if (const std::string* action = km->action(chord)) {
        lua_pushstring(L, "__osc_console");
        lua_rawget(L, LUA_REGISTRYINDEX);
        auto* console = static_cast<Console*>(lua_touserdata(L, -1));
        lua_pop(L, 1);
        if (console) console->execute(L, *action);
        return;
    }
    // An unbound Enter opens the chat, in a game (UI_ActivateChat).
    if (moho_key_code(ev.key_code) == 13) activate_chat(L, ev);
}

void UIDispatch::activate_chat(lua_State* L, const UIEvent& ev) {
    lua_pushstring(L, "__osc_game_state_mgr");
    lua_rawget(L, LUA_REGISTRYINDEX);
    const auto* mgr = static_cast<const GameStateManager*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    if (!mgr || mgr->current() != GameState::GAME) return;
    const int top = lua_gettop(L);
    lua_pushstring(L, "import");
    lua_rawget(L, LUA_GLOBALSINDEX);
    lua_pushstring(L, "/lua/ui/game/chat.lua");
    if (!lua_isfunction(L, -2) || lua_pcall(L, 1, 1, 0) != 0 || !lua_istable(L, -1)) {
        report_ui_callback_error(
            fmt::format("Error running '/lua/ui/game/chat.lua:ActivateChat': {}",
                        lua_isstring(L, -1) ? lua_tostring(L, -1) : "no module"));
        lua_settop(L, top);
        return;
    }
    lua_pushstring(L, "ActivateChat");
    lua_gettable(L, -2);
    lua_newtable(L);
    for (const auto& [bit, name] :
         {std::pair{GLFW_MOD_SHIFT, "Shift"}, std::pair{GLFW_MOD_CONTROL, "Ctrl"},
          std::pair{GLFW_MOD_ALT, "Alt"}}) {
        if (!(ev.modifiers & bit)) continue;
        lua_pushstring(L, name);
        lua_pushboolean(L, 1);
        lua_rawset(L, -3);
    }
    if (lua_pcall(L, 1, 0, 0) != 0)
        report_ui_callback_error(fmt::format(
            "Error running '/lua/ui/game/chat.lua:ActivateChat': {}", lua_tostring(L, -1)));
    lua_settop(L, top);
}

bool UIDispatch::run_script(lua_State* L, UIControl* ctrl, const char* name, const f64* arg,
                            const UIEvent* event) {
    if (ctrl->destroyed() || ctrl->lua_table_ref() < 0) return false;
    lua_rawgeti(L, LUA_REGISTRYINDEX, ctrl->lua_table_ref());
    lua_pushstring(L, name);
    lua_gettable(L, -2); // through the class, as Moho's RunScript looks
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 2);
        return false;
    }
    lua_pushvalue(L, -2); // self
    int args = 1;
    if (arg) {
        lua_pushnumber(L, *arg);
        ++args;
    }
    if (event) {
        push_event_table(L, *event);
        ++args;
    }
    if (lua_pcall(L, args, 0, 0) != 0) {
        report_ui_callback_error(fmt::format("{} error for control #{}: {}", name,
                                             ctrl->control_id(), lua_tostring(L, -1)));
        lua_pop(L, 1);
    }
    lua_pop(L, 1); // control table
    return true;
}

void UIDispatch::movie_frame(lua_State* L, UIControl* ctrl, f64 dt) {
    // Moho's CMauiMovie::Frame. A control not playing (its movie ended, or
    // Play found none) finishes.
    if (!ctrl->movie_playing()) {
        ctrl->set_needs_frame_update(false);
        run_script(L, ctrl, "OnFinished");
        return;
    }
    run_script(L, ctrl, "OnFrame", &dt);
    if (ctrl->destroyed()) return;
    if (ctrl->movie_stopped()) {
        ctrl->set_needs_frame_update(false);
        run_script(L, ctrl, "OnStopped");
        return;
    }
    video::MoviePlayer* movie = ctrl->movie_player();
    if (!movie) return;
    // Sofdec's clock runs on its own; here it moves with the frames. A movie
    // off screen decodes nothing: back on screen, update_frame catches up to
    // the frame due, as it does for one that decodes slower than it plays.
    movie->advance(dt);
    if (movie->finished()) {
        if (ctrl->movie_looping()) {
            movie->restart(ctrl->movie_on_screen());
        } else {
            ctrl->set_movie_playing(false);
            ctrl->set_needs_frame_update(false);
            run_script(L, ctrl, "OnFinished");
        }
        return;
    }
    if (ctrl->movie_on_screen()) movie->update_frame();
}

} // namespace osc::ui
