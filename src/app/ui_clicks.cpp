#include "app/ui_clicks.hpp"

#include "app/mods_flow_test.hpp"
#include "lua/lua_state.hpp"
#include "ui/ui_dispatch.hpp"

#include <GLFW/glfw3.h>
#include <fmt/format.h>

namespace osc::app {

void UiClicks::frame(lua::LuaState& ui, ui::UIDispatch& input, ui::UIControlRegistry& controls) {
    if (done()) {
        return;
    }
    ++frames_since_;
    const UiClick& step = steps_[next_];
    if (step.label.empty()) {
        if (frames_since_ < 2) {
            return;
        }
        input.on_cursor_pos(step.x, step.y);
        input.on_mouse_button(GLFW_MOUSE_BUTTON_LEFT, GLFW_PRESS, 0);
        input.on_mouse_button(GLFW_MOUSE_BUTTON_LEFT, GLFW_RELEASE, 0);
    } else {
        lua_State* L = ui.raw();
        if (!click(L, input, controls, find_control(L, controls, labelled(step.label.c_str())))) {
            return;
        }
    }
    ++next_;
    frames_since_ = 0;
}

std::string UiClicks::never_clicked() const {
    const UiClick& step = steps_[next_];
    if (step.label.empty()) {
        return fmt::format("--click-at {},{}: the run ended first", step.x, step.y);
    }
    return fmt::format("--click '{}': the button never took a click", step.label);
}

} // namespace osc::app
