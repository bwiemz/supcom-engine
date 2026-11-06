#pragma once

#include "renderer/vk_types.hpp"

#include <vulkan/vulkan.h>

#include <vector>

namespace osc::renderer {

class GpuUploads {
public:
    void init(VkDevice device, VmaAllocator allocator, VkCommandPool pool, VkQueue queue);

    struct Staging {
        VkBuffer buffer = VK_NULL_HANDLE;
        void* mapped = nullptr;
    };
    Staging stage(VkDeviceSize size);
    VkCommandBuffer commands();
    AllocatedBuffer buffer(const void* data, VkDeviceSize size, VkBufferUsageFlags usage);

    /// Before any submission that may read what the copies fill.
    void submit();
    /// Before destroying what a copy fills.
    void finish();
    void destroy();

private:
    struct Batch {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        std::vector<AllocatedBuffer> staging;
    };
    void release(Batch& batch);
    void reclaim(bool wait);

    VkDevice device_ = VK_NULL_HANDLE;
    VmaAllocator allocator_ = VK_NULL_HANDLE;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    Batch open_;
    std::vector<Batch> in_flight_;
};

} // namespace osc::renderer
