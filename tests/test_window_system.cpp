#include <catch2/catch_test_macros.hpp>

#include "renderer/renderer.hpp"
#include "renderer/window.hpp"

#include <GLFW/glfw3.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <cstring>
#include <vector>

using osc::renderer::open_window;

namespace {

/// Whether a Vulkan device exists, asked before any window: a machine without one
/// skips the renderer tests without starting GLFW.
bool vulkan_device() {
    uint32_t count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> extensions(count);
    vkEnumerateInstanceExtensionProperties(nullptr, &count, extensions.data());
    VkInstanceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    const char* portability = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
    for (const auto& e : extensions) {
        if (std::strcmp(e.extensionName, portability) == 0) {
            info.flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
            info.enabledExtensionCount = 1;
            info.ppEnabledExtensionNames = &portability;
        }
    }
    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&info, nullptr, &instance) != VK_SUCCESS) return false;
    uint32_t devices = 0;
    vkEnumeratePhysicalDevices(instance, &devices, nullptr);
    vkDestroyInstance(instance, nullptr);
    return devices > 0;
}

struct NullPlatform {
    NullPlatform() { glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_NULL); }
    ~NullPlatform() { glfwInitHint(GLFW_PLATFORM, GLFW_ANY_PLATFORM); }
};

} // namespace

TEST_CASE("Shutting down one renderer leaves another's window open", "[renderer][window]") {
    if (!vulkan_device()) SKIP("no Vulkan device");
    osc::renderer::Renderer outer;
    osc::renderer::Renderer inner;
    REQUIRE(outer.init(64, 64, "outer", /*offscreen=*/true));
    REQUIRE(inner.init(64, 64, "inner", /*offscreen=*/true));

    inner.shutdown();
    glfwGetError(nullptr);
    outer.set_window_title("still open");

    REQUIRE(glfwGetError(nullptr) == GLFW_NO_ERROR);
}

TEST_CASE("Each window's input reaches its own renderer", "[renderer][window]") {
    if (!vulkan_device()) SKIP("no Vulkan device");
    osc::renderer::Renderer outer;
    osc::renderer::Renderer inner;
    REQUIRE(outer.init(64, 64, "outer", /*offscreen=*/true));
    REQUIRE(inner.init(64, 64, "inner", /*offscreen=*/true));

    GLFWwindow* const window = outer.glfw_window();
    REQUIRE(window);
    const auto key = glfwSetKeyCallback(window, nullptr);
    const auto button = glfwSetMouseButtonCallback(window, nullptr);
    const auto cursor = glfwSetCursorPosCallback(window, nullptr);
    const auto character = glfwSetCharCallback(window, nullptr);
    glfwSetKeyCallback(window, key);
    glfwSetMouseButtonCallback(window, button);
    glfwSetCursorPosCallback(window, cursor);
    glfwSetCharCallback(window, character);
    REQUIRE(key);
    REQUIRE(button);
    REQUIRE(cursor);
    REQUIRE(character);

    cursor(window, 12.0, 34.0);
    CHECK(outer.ui_dispatch().mouse_x() == 12.0);
    CHECK(outer.ui_dispatch().mouse_y() == 34.0);
    CHECK(inner.ui_dispatch().mouse_x() == 0.0);
    CHECK(inner.ui_dispatch().mouse_y() == 0.0);

    inner.shutdown();
    cursor(window, 56.0, 78.0);
    key(window, GLFW_KEY_A, 0, GLFW_PRESS, 0);
    button(window, GLFW_MOUSE_BUTTON_LEFT, GLFW_PRESS, 0);
    character(window, 'a');
    CHECK(outer.ui_dispatch().mouse_x() == 56.0);
    CHECK(outer.ui_dispatch().mouse_y() == 78.0);
}

TEST_CASE("GLFW runs while any window is open", "[window]") {
    if (!glfwPlatformSupported(GLFW_PLATFORM_NULL)) SKIP("GLFW is built without its null platform");
    const NullPlatform null_platform;
    glfwGetError(nullptr);

    SECTION("closing one window leaves another") {
        const auto first = open_window(64, 48, "first", /*offscreen=*/true);
        REQUIRE(first);
        REQUIRE(glfwGetPlatform() == GLFW_PLATFORM_NULL);
        {
            const auto second = open_window(32, 24, "second", /*offscreen=*/true);
            REQUIRE(second);
        }

        int width = 0;
        int height = 0;
        glfwGetWindowSize(first.get(), &width, &height);
        CHECK(glfwGetError(nullptr) == GLFW_NO_ERROR);
        CHECK(width == 64);
        CHECK(height == 48);
    }

    SECTION("a window that cannot be made leaves GLFW to the open ones") {
        const auto open = open_window(64, 48, "open", /*offscreen=*/true);
        REQUIRE(open);
        REQUIRE(glfwGetPlatform() == GLFW_PLATFORM_NULL);

        CHECK_FALSE(open_window(0, 0, "empty", /*offscreen=*/true));
        glfwGetError(nullptr);

        int width = 0;
        int height = 0;
        glfwGetWindowSize(open.get(), &width, &height);
        CHECK(glfwGetError(nullptr) == GLFW_NO_ERROR);
        CHECK(width == 64);
        CHECK(height == 48);
    }

    SECTION("GLFW ends with the last window and starts again with the next") {
        {
            const auto only = open_window(64, 48, "only", /*offscreen=*/true);
            REQUIRE(only);
            REQUIRE(glfwGetPlatform() == GLFW_PLATFORM_NULL);
        }
        glfwGetError(nullptr);
        CHECK(glfwGetPlatform() == 0);
        CHECK(glfwGetError(nullptr) == GLFW_NOT_INITIALIZED);

        const auto next = open_window(64, 48, "next", /*offscreen=*/true);
        REQUIRE(next);
        CHECK(glfwGetPlatform() == GLFW_PLATFORM_NULL);
    }

    SECTION("a window that cannot be made as the first leaves GLFW stopped") {
        CHECK_FALSE(open_window(0, 0, "empty", /*offscreen=*/true));
        glfwGetError(nullptr);
        CHECK(glfwGetPlatform() == 0);
        CHECK(glfwGetError(nullptr) == GLFW_NOT_INITIALIZED);
    }
}
