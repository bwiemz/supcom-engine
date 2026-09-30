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

/// What a scrollbar's scrollable shows: its GetScrollValues
ScrollValues scroll_values(lua_State* L, const UIControl& scrollbar);

} // namespace osc::ui
