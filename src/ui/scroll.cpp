#include "ui/scroll.hpp"

#include "ui/font_metrics_provider.hpp"
#include "ui/lazyvar.hpp"
#include "ui/ui_control.hpp"

#include <algorithm>
#include <cmath>

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

TrackPart track_part(const ThumbSpan& thumb, f32 at) {
    if (at < thumb.start) {
        return TrackPart::Before;
    }
    return at < thumb.start + thumb.length ? TrackPart::Thumb : TrackPart::After;
}

f32 dragged_top(const ScrollValues& values, f32 track, f32 length, f32 start) {
    const f32 travel = track - length;
    const f32 tops =
        (values.range_max - values.range_min) - (values.visible_max - values.visible_min);
    if (travel <= 0 || tops <= 0) {
        return values.visible_min;
    }
    return values.range_min + tops * std::clamp(start, 0.0f, travel) / travel;
}

f32 item_list_row_height(const UIControl& list) {
    const f32 line =
        FontMetricsProvider::instance().line_height(list.font_family(), list.font_pointsize());
    if (line > 0) {
        return line;
    }
    return static_cast<f32>(list.font_pointsize()) + 4.0f;
}

namespace {

i32 shown_rows(const UIControl& list, f32 height) {
    return std::max(1, static_cast<i32>(height / item_list_row_height(list)));
}

/// The top row that keeps `top` in bounds: no gap below the last row
i32 clamped_top(const UIControl& list, f32 height, i32 top) {
    const i32 count = static_cast<i32>(list.items().size());
    return std::clamp(top, 0, std::max(0, count - shown_rows(list, height)));
}

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

/// The pushed scrollable's `method`(axis, amount), if it has one
void call_scrollable(lua_State* L, const UIControl& scrollbar, const char* method, f32 amount) {
    lua_pushstring(L, method);
    lua_gettable(L, -2);
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    lua_pushvalue(L, -2);
    lua_pushstring(L, scrollbar.scroll_axis().c_str());
    lua_pushnumber(L, amount);
    if (lua_pcall(L, 3, 0, 0) != 0) {
        lua_pop(L, 1);
    }
}

} // namespace

ScrollValues item_list_scroll_values(const UIControl& list, f32 height) {
    const i32 count = static_cast<i32>(list.items().size());
    const i32 top = clamped_top(list, height, list.scroll_top());
    return {0, static_cast<f32>(count), static_cast<f32>(top),
            static_cast<f32>(std::min(top + shown_rows(list, height), count))};
}

void scroll_item_list(UIControl& list, f32 height, i32 lines) {
    list.set_scroll_top(clamped_top(list, height, list.scroll_top() + lines));
}

void scroll_item_list_to_bottom(UIControl& list, f32 height) {
    list.set_scroll_top(clamped_top(list, height, static_cast<i32>(list.items().size())));
}

void show_item_list_row(UIControl& list, f32 height, i32 row) {
    const i32 rows = shown_rows(list, height);
    i32 top = list.scroll_top();
    if (row < top) {
        top = row;
    } else if (row >= top + rows) {
        top = row - rows + 1;
    }
    list.set_scroll_top(clamped_top(list, height, top));
}

bool item_list_needs_scrollbar(const UIControl& list, f32 height) {
    return static_cast<i32>(list.items().size()) > shown_rows(list, height);
}

ScrollValues scroll_values(lua_State* L, const UIControl& scrollbar) {
    const int top = lua_gettop(L);
    ScrollValues values;
    UIControl* scrollable = push_scrollable(L, scrollbar);
    if (scrollable && scrollable->control_type() == UIControl::ControlType::ItemList) {
        values = item_list_scroll_values(*scrollable, read_lazyvar(L, -1, "Height"));
    } else if (lua_gettop(L) > top) {
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

void scroll_by(lua_State* L, const UIControl& scrollbar, f32 amount, bool pages) {
    const int top = lua_gettop(L);
    UIControl* scrollable = push_scrollable(L, scrollbar);
    if (scrollable && scrollable->control_type() == UIControl::ControlType::ItemList) {
        const f32 height = read_lazyvar(L, -1, "Height");
        const f32 lines =
            pages ? amount * static_cast<f32>(shown_rows(*scrollable, height)) : amount;
        scroll_item_list(*scrollable, height, static_cast<i32>(std::lround(lines)));
    } else if (lua_gettop(L) > top) {
        call_scrollable(L, scrollbar, pages ? "ScrollPages" : "ScrollLines", amount);
    }
    lua_settop(L, top);
}

void scroll_set_top(lua_State* L, const UIControl& scrollbar, f32 top_row) {
    const int top = lua_gettop(L);
    UIControl* scrollable = push_scrollable(L, scrollbar);
    if (scrollable && scrollable->control_type() == UIControl::ControlType::ItemList) {
        const f32 height = read_lazyvar(L, -1, "Height");
        scrollable->set_scroll_top(
            clamped_top(*scrollable, height, static_cast<i32>(std::lround(top_row))));
    } else if (lua_gettop(L) > top) {
        call_scrollable(L, scrollbar, "ScrollSetTop", top_row);
    }
    lua_settop(L, top);
}

} // namespace osc::ui
