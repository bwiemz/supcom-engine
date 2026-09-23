#pragma once

#include "core/types.hpp"

#include <vector>

namespace osc::ui {

/// A control's screen rectangle.
struct ControlRect {
    f32 x = 0;
    f32 y = 0;
    f32 w = 0;
    f32 h = 0;
};

/// The rectangle Moho draws and hit-tests a control over: its edges
/// (Left/Top/Right/Bottom). MAUI's Width/Height are layout inputs -- a
/// bitmap's default to its texture size even after LayoutHelpers.FillParent
/// has set all four edges -- so each only stands in when its axis has no
/// valid edge pair.
inline ControlRect control_rect(f32 left, f32 top, f32 right, f32 bottom,
                                f32 width, f32 height) {
    return {left, top, right > left ? right - left : width,
            bottom > top ? bottom - top : height};
}

/// A main (non-minimap) WorldView as the UI sees it: FA draws the 3D world
/// into it, opaque, at its depth.
struct WorldOccluder {
    ControlRect rect;
    f32 depth = 0;
};

/// Whether UI drawn at `depth` over `r` is hidden behind a world view: it
/// lies below the view's depth and entirely inside the view. (Moho leaves UI
/// under the world this way -- retail never destroys its loading movie.)
inline bool hidden_by_world(const ControlRect& r, f32 depth,
                            const std::vector<WorldOccluder>& views) {
    for (const auto& v : views) {
        if (depth >= v.depth) continue;
        if (r.x >= v.rect.x && r.y >= v.rect.y &&
            r.x + r.w <= v.rect.x + v.rect.w && r.y + r.h <= v.rect.y + v.rect.h)
            return true;
    }
    return false;
}

} // namespace osc::ui
