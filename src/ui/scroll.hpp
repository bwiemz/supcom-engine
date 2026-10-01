#pragma once

#include "core/types.hpp"

struct lua_State;

namespace osc::ui {

class UIControl;

/// What a scrollable holds and the part of it shown, in the order of Moho's
/// GetScrollValues: rangeMin, rangeMax, visibleMin, visibleMax
struct ScrollValues {
    f32 range_min = 0;
    f32 range_max = 0;
    f32 visible_min = 0;
    f32 visible_max = 0;
};

/// Where a scrollbar's thumb lies along its track
struct ThumbSpan {
    f32 start = 0;
    f32 length = 0;
};

/// The thumb on a track `track` long: the shown part's share of it, at the
/// shown part's place, no shorter than `min_length`; the whole track when all
/// of it shows
ThumbSpan thumb_span(const ScrollValues& values, f32 track, f32 min_length);

/// Where a point `at` along a track falls: before the thumb, on it, after it
enum class TrackPart { Before, Thumb, After };
TrackPart track_part(const ThumbSpan& thumb, f32 at);

/// The top shown by a thumb `length` long dragged to `start` on a track
/// `track` long: its travel spans the tops the scrollable can show
f32 dragged_top(const ScrollValues& values, f32 track, f32 length, f32 start);

/// The height an ItemList's rows are drawn at
f32 item_list_row_height(const UIControl& list);

/// An ItemList `height` tall: its rows, and those it shows
ScrollValues item_list_scroll_values(const UIControl& list, f32 height);

/// Scrolls an ItemList `height` tall by `lines`, keeping its rows in view
void scroll_item_list(UIControl& list, f32 height, i32 lines);

/// Its last rows in view
void scroll_item_list_to_bottom(UIControl& list, f32 height);

/// Scrolls it no further than `row` needs to be in view
void show_item_list_row(UIControl& list, f32 height, i32 row);

/// Whether its rows need more than `height`
bool item_list_needs_scrollbar(const UIControl& list, f32 height);

/// What a scrollbar's scrollable shows: an ItemList's own values (Moho keeps
/// them in C++, its Lua sees Control's stub), another's GetScrollValues
ScrollValues scroll_values(lua_State* L, const UIControl& scrollbar);

/// Scrolls a scrollbar's scrollable by `amount` lines, or pages: an ItemList
/// here, another by its ScrollLines or ScrollPages
void scroll_by(lua_State* L, const UIControl& scrollbar, f32 amount, bool pages);

/// Shows a scrollbar's scrollable from `top`: an ItemList here, another by
/// its ScrollSetTop
void scroll_set_top(lua_State* L, const UIControl& scrollbar, f32 top);

} // namespace osc::ui
