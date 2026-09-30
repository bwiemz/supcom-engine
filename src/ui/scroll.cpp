#include "ui/scroll.hpp"

#include "ui/ui_control.hpp"

#include <algorithm>

extern "C" {
#include <lua.h>
}

namespace osc::ui {

ThumbSpan thumb_span(const ScrollValues& values, f32 track, f32 min_length) {
    const f32 range = values.range_max - values.range_min;
    const f32 shown = values.visible_max - values.visible_min;
    if (range <= 0 || shown >= range) {
        return {0, track};
    }
    const f32 length = std::min(std::max(track * shown / range, min_length), track);
    const f32 start = track * (values.visible_min - values.range_min) / range;
    return {std::clamp(start, 0.0f, track - length), length};
}

namespace {

/// The scrollbar's scrollable (its Lua table pushed) and its control; nothing
/// pushed and null without one
UIControl* push_scrollable(lua_State* L, const UIControl& scrollbar) {
    if (scrollbar.scrollable_ref() < 0) {
        return nullptr;
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, scrollbar.scrollable_ref());
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return nullptr;
    }
    lua_pushstring(L, "_c_object");
    lua_rawget(L, -2);
    auto* control = static_cast<UIControl*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return control;
}

} // namespace

ScrollValues scroll_values(lua_State* L, const UIControl& scrollbar) {
    const int top = lua_gettop(L);
    ScrollValues values;
    push_scrollable(L, scrollbar);
    if (lua_gettop(L) > top) {
        lua_pushstring(L, "GetScrollValues");
        lua_gettable(L, -2);
        if (lua_isfunction(L, -1)) {
            lua_pushvalue(L, -2);
            lua_pushstring(L, scrollbar.scroll_axis().c_str());
            if (lua_pcall(L, 2, 4, 0) == 0) {
                values = {
                    static_cast<f32>(lua_tonumber(L, -4)), static_cast<f32>(lua_tonumber(L, -3)),
                    static_cast<f32>(lua_tonumber(L, -2)), static_cast<f32>(lua_tonumber(L, -1))};
            }
        }
    }
    lua_settop(L, top);
    return values;
}

} // namespace osc::ui
