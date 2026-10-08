#pragma once

#include "core/types.hpp"

#include <array>

namespace osc::core {

/// A cursor position from the window's units to the framebuffer's pixels
/// (M217h). On a scaled display (HiDPI, Wayland at 150%) they differ, and
/// everything the cursor is compared with -- the UI's rects, the viewport,
/// the camera's pan and spin -- is in framebuffer pixels.
inline std::array<f64, 2> to_framebuffer(f64 x, f64 y, i32 window_w, i32 window_h,
                                         i32 framebuffer_w, i32 framebuffer_h) {
    const f64 sx = window_w > 0 ? static_cast<f64>(framebuffer_w) / window_w : 1.0;
    const f64 sy = window_h > 0 ? static_cast<f64>(framebuffer_h) / window_h : 1.0;
    return {x * sx, y * sy};
}

/// `buttons`: a bit per GLFW mouse button held; `mods`: the GLFW_MOD_* keys held
struct ScriptedPointer {
    f64 x = 0;
    f64 y = 0;
    u32 buttons = 0;
    i32 mods = 0;
};

} // namespace osc::core
