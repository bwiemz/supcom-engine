#include "ui/keyboard_focus.hpp"

#include "core/test_status.hpp"
#include "ui/ui_control.hpp"

#include <spdlog/spdlog.h>

extern "C" {
#include <lua.h>
}

#include <string>

namespace osc::ui {

namespace {

/// The control's script method `name`(self), if it has one.
void run_script(lua_State* L, UIControl* control, const char* name) {
    if (!L || control->destroyed() || control->lua_table_ref() < 0) return;
    const int top = lua_gettop(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, control->lua_table_ref());
    lua_pushstring(L, name);
    lua_gettable(L, -2); // through the class, as Moho's RunScript looks
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, -2);
        if (lua_pcall(L, 1, 0, 0) != 0) {
            const char* err = lua_tostring(L, -1);
            const std::string what = std::string(name) + " error for control #" +
                                     std::to_string(control->control_id()) + ": " +
                                     (err ? err : "(unknown)");
            spdlog::warn("{}", what);
            if (test_status::count_lua_failures())
                test_status::record_failure("Lua UI callback error: " + what);
        }
    }
    lua_settop(L, top);
}

} // namespace

void set_keyboard_focus(lua_State* L, UIControlRegistry& registry, UIControl* control,
                        bool blocks_key_down) {
    UIControl* const previous = registry.keyboard_focus();
    registry.set_keyboard_focus(control, blocks_key_down);
    // Told after the change, even when it is the control focused again (a
    // click on an edit focuses it through its script and once more).
    if (previous && !previous->destroyed()) run_script(L, previous, "OnKeyboardFocusChange");
}

void abandon_keyboard_focus(lua_State* L, UIControlRegistry& registry, UIControl* control) {
    if (control && registry.keyboard_focus() == control)
        set_keyboard_focus(L, registry, nullptr, true);
}

void losing_keyboard_focus(lua_State* L, UIControlRegistry& registry, UIControl* focused) {
    if (!focused || focused->destroyed()) return;
    if (focused->control_type() == UIControl::ControlType::Edit)
        abandon_keyboard_focus(L, registry, focused);
    run_script(L, focused, "OnLoseKeyboardFocus");
}

} // namespace osc::ui
