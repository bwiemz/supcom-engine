#include "renderer/window.hpp"

#include <GLFW/glfw3.h>
#include <spdlog/spdlog.h>

namespace osc::renderer {

namespace {
int open_windows = 0;
} // namespace

void CloseWindow::operator()(GLFWwindow* window) const {
    glfwDestroyWindow(window);
    if (--open_windows == 0) glfwTerminate();
}

WindowPtr open_window(int width, int height, const char* title, bool offscreen) {
    if (open_windows == 0 && !glfwInit()) {
        spdlog::error("Failed to initialize GLFW");
        return nullptr;
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, offscreen ? GLFW_FALSE : GLFW_TRUE);
    // Offscreen keeps a hidden window (input and timing code expect one) but
    // never shows it: GLFW's show waits for the compositor to map the window.
    glfwWindowHint(GLFW_VISIBLE, offscreen ? GLFW_FALSE : GLFW_TRUE);
    // An offscreen capture is its size in pixels, unscaled on a Retina display
    glfwWindowHint(GLFW_SCALE_FRAMEBUFFER, offscreen ? GLFW_FALSE : GLFW_TRUE);
    GLFWwindow* window = glfwCreateWindow(width, height, title, nullptr, nullptr);
    if (!window) {
        spdlog::error("Failed to create GLFW window");
        if (open_windows == 0) glfwTerminate();
        return nullptr;
    }
    ++open_windows;
    // An offscreen capture is the size asked for, whatever the screen's:
    // Windows keeps a window within the screens' height unless its own
    // limits allow more (a 2x capture of a laptop's frame outgrows a 1440p
    // screen). The limit is a Vulkan image's usual largest, so a test that
    // resizes the window later can still grow it.
    if (offscreen) {
        constexpr int kLargestImage = 16384;
        glfwSetWindowSizeLimits(window, GLFW_DONT_CARE, GLFW_DONT_CARE, kLargestImage,
                                kLargestImage);
        glfwSetWindowSize(window, width, height);
    }
    return WindowPtr(window);
}

} // namespace osc::renderer
