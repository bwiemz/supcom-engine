#pragma once

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <deque>
#include <functional>

namespace osc::renderer {

struct AllocatedBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
};

struct AllocatedImage {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};

/// Upload data via staging buffer → device-local buffer.
/// Returns a null buffer on failure.
AllocatedBuffer upload_buffer(VkDevice device, VmaAllocator allocator,
                              VkCommandPool cmd_pool, VkQueue queue,
                              const void* data, VkDeviceSize size,
                              VkBufferUsageFlags usage);

/// LIFO cleanup queue — objects are destroyed in reverse insertion order.
class DeletionQueue {
public:
    void push(std::function<void()>&& fn) { deletors_.push_back(std::move(fn)); }

    void flush() {
        for (auto it = deletors_.rbegin(); it != deletors_.rend(); ++it)
            (*it)();
        deletors_.clear();
    }

private:
    std::deque<std::function<void()>> deletors_;
};

} // namespace osc::renderer
