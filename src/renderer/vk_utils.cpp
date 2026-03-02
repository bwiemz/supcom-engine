#include "renderer/vk_types.hpp"

#include <spdlog/spdlog.h>

#include <cstring>

namespace osc::renderer {

AllocatedBuffer upload_buffer(VkDevice device, VmaAllocator allocator,
                              VkCommandPool cmd_pool, VkQueue queue,
                              const void* data, VkDeviceSize size,
                              VkBufferUsageFlags usage) {
    // Staging buffer (CPU-visible)
    VkBufferCreateInfo staging_ci{};
    staging_ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    staging_ci.size = size;
    staging_ci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

    VmaAllocationCreateInfo staging_alloc_ci{};
    staging_alloc_ci.usage = VMA_MEMORY_USAGE_CPU_ONLY;

    AllocatedBuffer staging{};
    if (vmaCreateBuffer(allocator, &staging_ci, &staging_alloc_ci,
                        &staging.buffer, &staging.allocation,
                        nullptr) != VK_SUCCESS) {
        spdlog::error("upload_buffer: failed to create staging buffer");
        return {};
    }

    void* mapped = nullptr;
    if (vmaMapMemory(allocator, staging.allocation, &mapped) != VK_SUCCESS) {
        spdlog::error("upload_buffer: failed to map staging buffer");
        vmaDestroyBuffer(allocator, staging.buffer, staging.allocation);
        return {};
    }
    std::memcpy(mapped, data, size);
    vmaUnmapMemory(allocator, staging.allocation);

    // Device-local buffer
    VkBufferCreateInfo buf_ci{};
    buf_ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buf_ci.size = size;
    buf_ci.usage = usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

    VmaAllocationCreateInfo alloc_ci{};
    alloc_ci.usage = VMA_MEMORY_USAGE_GPU_ONLY;

    AllocatedBuffer result{};
    if (vmaCreateBuffer(allocator, &buf_ci, &alloc_ci,
                        &result.buffer, &result.allocation,
                        nullptr) != VK_SUCCESS) {
        spdlog::error("upload_buffer: failed to create device buffer");
        vmaDestroyBuffer(allocator, staging.buffer, staging.allocation);
        return {};
    }

    // Copy via one-shot command buffer
    VkCommandBufferAllocateInfo cmd_ai{};
    cmd_ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmd_ai.commandPool = cmd_pool;
    cmd_ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_ai.commandBufferCount = 1;

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device, &cmd_ai, &cmd);

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin_info);

    VkBufferCopy copy{};
    copy.size = size;
    vkCmdCopyBuffer(cmd, staging.buffer, result.buffer, 1, &copy);

    vkEndCommandBuffer(cmd);

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);

    vkFreeCommandBuffers(device, cmd_pool, 1, &cmd);
    vmaDestroyBuffer(allocator, staging.buffer, staging.allocation);

    return result;
}

} // namespace osc::renderer
