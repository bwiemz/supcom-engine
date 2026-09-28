#include "ui/ui_dispatch.hpp"
#include "ui/ui_control.hpp"
#include "core/game_state.hpp"
#include "ui/console.hpp"
#include "ui/key_codes.hpp"
#include "ui/keymap.hpp"
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

/// Read a LazyVar float from a control's Lua table.
static f32 read_lazyvar_dispatch(lua_State* L, int tbl_idx, const char* field) {
    lua_pushstring(L, field);
    lua_rawget(L, tbl_idx);
    if (!lua_istable(L, -1)) {
        if (lua_isnumber(L, -1)) {
            f32 val = static_cast<f32>(lua_tonumber(L, -1));
            lua_pop(L, 1);
            return val;
        }
        lua_pop(L, 1);
        return 0.0f;
    }
    if (lua_pcall(L, 0, 1, 0) != 0) {
        lua_pop(L, 1);
        return 0.0f;
    }
    f32 val = static_cast<f32>(lua_tonumber(L, -1));
    lua_pop(L, 1);
    return val;
}

namespace {

/// Walk the visible tree under `ctrl`, keeping the deepest hit-testable
/// control whose rect contains (x, y). Parents are visited before their
/// children and siblings in order, so `>=` lets the later one win a tie.
void collect_hit(lua_State* L, UIControl* ctrl, f32 x, f32 y,
                 const std::unordered_set<UIControl*>* skip, UIControl*& best,
                 f32& best_depth) {
    if (!ctrl || ctrl->hidden() || ctrl->destroyed()) return;
    if (ctrl->lua_table_ref() < 0) return;

    if (!ctrl->hit_test_disabled() && !(skip && skip->count(ctrl))) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, ctrl->lua_table_ref());
        const int tbl = lua_gettop(L);
        const f32 left = read_lazyvar_dispatch(L, tbl, "Left");
        const f32 top = read_lazyvar_dispatch(L, tbl, "Top");
        const auto rect = control_rect(left, top, read_lazyvar_dispatch(L, tbl, "Right"),
                                       read_lazyvar_dispatch(L, tbl, "Bottom"),
                                       read_lazyvar_dispatch(L, tbl, "Width"),
                                       read_lazyvar_dispatch(L, tbl, "Height"));
        const bool inside = x >= rect.x && x < rect.x + rect.w &&
                            y >= rect.y && y < rect.y + rect.h;
        const f32 depth = inside ? read_lazyvar_dispatch(L, tbl, "Depth") : 0.0f;
        lua_pop(L, 1);
        if (inside && (!best || depth >= best_depth)) {
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

        // Keys (Moho): the focused control has them, and they go no further;
        // with no focus, the top input capture; with neither, the key map,
        // for a key going down (CUIKeyHandler sits below the controls).
        if (ev.type == UIEventType::KEY_DOWN ||
            ev.type == UIEventType::KEY_UP ||
            ev.type == UIEventType::CHAR) {
            if (auto* focus = registry.keyboard_focus()) fire_handle_event(L, focus, ev);
            else if (auto* capture = registry.input_capture()) fire_handle_event(L, capture, ev);
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

        // Call UIMain.OnMouseButtonPress for global click handlers
        // (e.g. Combo close-on-outside-click via AddOnMouseClickedFunc)
        if (ev.type == UIEventType::BUTTON_PRESS) {
            lua_pushstring(L, "OnMouseButtonPress");
            lua_rawget(L, LUA_GLOBALSINDEX);
            if (lua_isfunction(L, -1)) {
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
                if (lua_pcall(L, 1, 0, 0) != 0) {
                    report_ui_callback_error(fmt::format(
                        "OnMouseButtonPress error: {}", lua_tostring(L, -1)));
                    lua_pop(L, 1);
                }
            } else {
                lua_pop(L, 1);
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
        lua_rawget(L, -2);
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

bool UIDispatch::run_script(lua_State* L, UIControl* ctrl, const char* name, const f64* arg) {
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
    // Sofdec's clock runs on its own; here it moves with the frames.
    movie->advance(dt);
    if (movie->finished()) {
        if (ctrl->movie_looping()) {
            movie->restart();
        } else {
            ctrl->set_movie_playing(false);
            ctrl->set_needs_frame_update(false);
            run_script(L, ctrl, "OnFinished");
        }
        return;
    }
    movie->update_frame();
}

} // namespace osc::ui
