#pragma once

#include "core/types.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace osc::core {

/// A cursor position from the window's units to the framebuffer's pixels
/// (M217h), the camera's units
inline std::array<f64, 2> to_framebuffer(f64 x, f64 y, i32 window_w, i32 window_h,
                                         i32 framebuffer_w, i32 framebuffer_h) {
    const f64 sx = window_w > 0 ? static_cast<f64>(framebuffer_w) / window_w : 1.0;
    const f64 sy = window_h > 0 ? static_cast<f64>(framebuffer_h) / window_h : 1.0;
    return {x * sx, y * sy};
}

inline f32 content_scale(i32 window_w, i32 framebuffer_w) {
    if (window_w <= 0 || framebuffer_w <= 0) {
        return 1.0f;
    }
    return static_cast<f32>(framebuffer_w) / static_cast<f32>(window_w);
}

inline std::array<u32, 2> ui_extent(u32 framebuffer_w, u32 framebuffer_h, f32 scale) {
    const f32 s = scale > 0.0f ? scale : 1.0f;
    const auto side = [s](u32 px) {
        return static_cast<u32>(std::max(1L, std::lround(static_cast<f32>(px) / s)));
    };
    return {side(framebuffer_w), side(framebuffer_h)};
}

inline std::array<f64, 2> ui_to_pixels(f64 x, f64 y, f32 scale) {
    return {x * scale, y * scale};
}

/// `buttons`: a bit per GLFW mouse button held; `mods`: the GLFW_MOD_* keys held
struct ScriptedPointer {
    f64 x = 0;
    f64 y = 0;
    u32 buttons = 0;
    i32 mods = 0;
};

} // namespace osc::core
