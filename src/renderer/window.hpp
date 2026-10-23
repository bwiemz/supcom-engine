#pragma once

#include <memory>

struct GLFWwindow;

namespace osc::renderer {

struct CloseWindow {
    void operator()(GLFWwindow* window) const;
};

/// A GLFW window. GLFW runs while any window is open: the first one opens
/// starts it and closing the last stops it. Main thread only, as GLFW is.
using WindowPtr = std::unique_ptr<GLFWwindow, CloseWindow>;

/// Opens a window of `width` x `height` pixels, hidden and unscaled when
/// `offscreen`. Null if GLFW or the window cannot be made.
WindowPtr open_window(int width, int height, const char* title, bool offscreen);

} // namespace osc::renderer
