#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <cstring>
#include <vector>

namespace osc::test {

/// Whether a Vulkan device exists, asked before any window: a machine without one
/// skips the renderer tests without starting GLFW.
inline bool vulkan_device() {
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

} // namespace osc::test
