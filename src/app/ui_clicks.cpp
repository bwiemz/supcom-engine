#include "app/ui_clicks.hpp"

#include "app/mods_flow_test.hpp"
#include "lua/lua_state.hpp"

namespace osc::app {

void UiClicks::frame(lua::LuaState& ui, ui::UIDispatch& input, ui::UIControlRegistry& controls) {
    if (done()) {
        return;
    }
    lua_State* L = ui.raw();
    if (click(L, input, controls, find_control(L, controls, labelled(labels_[next_].c_str())))) {
        ++next_;
    }
}

} // namespace osc::app
