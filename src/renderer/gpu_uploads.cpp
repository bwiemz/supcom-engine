#include "renderer/gpu_uploads.hpp"
#include "renderer/vk_cmd.hpp"

#include <spdlog/spdlog.h>

#include <cstdint>
#include <cstring>
#include <utility>

namespace osc::renderer {

void GpuUploads::init(VkDevice device, VmaAllocator allocator, VkCommandPool pool, VkQueue queue) {
    device_ = device;
    allocator_ = allocator;
    pool_ = pool;
    queue_ = queue;
}

GpuUploads::Staging GpuUploads::stage(VkDeviceSize size) {
    if (size == 0) {
        return {};
    }
    VkBufferCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    ci.size = size;
    ci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VmaAllocationCreateInfo alloc_ci{};
    alloc_ci.usage = VMA_MEMORY_USAGE_CPU_ONLY;
    alloc_ci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
    AllocatedBuffer buffer{};
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(allocator_, &ci, &alloc_ci, &buffer.buffer, &buffer.allocation, &info) !=
        VK_SUCCESS) {
        spdlog::warn("GpuUploads: no staging buffer of {} bytes", size);
        return {};
    }
    open_.staging.push_back(buffer);
    return {buffer.buffer, info.pMappedData};
}

VkCommandBuffer GpuUploads::commands() {
    if (open_.cmd) {
        return open_.cmd;
    }
    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = pool_;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(device_, &ai, &open_.cmd) != VK_SUCCESS) {
        open_.cmd = VK_NULL_HANDLE;
        return VK_NULL_HANDLE;
    }
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(open_.cmd, &begin);
    return open_.cmd;
}

void GpuUploads::submit() {
    if (open_.cmd) {
        vkEndCommandBuffer(open_.cmd);
        VkFenceCreateInfo fence_ci{};
        fence_ci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &open_.cmd;
        if (vkCreateFence(device_, &fence_ci, nullptr, &open_.fence) == VK_SUCCESS &&
            vkQueueSubmit(queue_, 1, &submit, open_.fence) == VK_SUCCESS) {
            in_flight_.push_back(std::move(open_));
        } else {
            spdlog::error("GpuUploads: {} uploads failed to submit", open_.staging.size());
            release(open_);
        }
    } else {
        release(open_);
    }
    open_ = {};
    reclaim(false);
}

void GpuUploads::finish() {
    submit();
    if (!in_flight_.empty()) {
        ++command_counts().upload_waits;
    }
    reclaim(true);
}

void GpuUploads::destroy() {
    if (device_) {
        finish();
    }
}

void GpuUploads::release(Batch& batch) {
    if (batch.cmd) {
        vkFreeCommandBuffers(device_, pool_, 1, &batch.cmd);
    }
    for (const AllocatedBuffer& b : batch.staging) {
        vmaDestroyBuffer(allocator_, b.buffer, b.allocation);
    }
    if (batch.fence) {
        vkDestroyFence(device_, batch.fence, nullptr);
    }
    batch = {};
}

void GpuUploads::reclaim(bool wait) {
    std::vector<Batch> running;
    for (Batch& b : in_flight_) {
        if (wait) {
            vkWaitForFences(device_, 1, &b.fence, VK_TRUE, UINT64_MAX);
        } else if (vkGetFenceStatus(device_, b.fence) != VK_SUCCESS) {
            running.push_back(std::move(b));
            continue;
        }
        release(b);
    }
    in_flight_ = std::move(running);
}

AllocatedBuffer GpuUploads::buffer(const void* data, VkDeviceSize size, VkBufferUsageFlags usage) {
    VkBufferCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    ci.size = size;
    ci.usage = usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo alloc_ci{};
    alloc_ci.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    AllocatedBuffer result{};
    if (vmaCreateBuffer(allocator_, &ci, &alloc_ci, &result.buffer, &result.allocation, nullptr) !=
        VK_SUCCESS) {
        spdlog::error("GpuUploads: no device buffer of {} bytes", size);
        return {};
    }
    const Staging staging = stage(size);
    VkCommandBuffer cmd = staging.buffer ? commands() : VK_NULL_HANDLE;
    if (!cmd) {
        vmaDestroyBuffer(allocator_, result.buffer, result.allocation);
        return {};
    }
    std::memcpy(staging.mapped, data, size);
    VkBufferCopy copy{};
    copy.size = size;
    vkCmdCopyBuffer(cmd, staging.buffer, result.buffer, 1, &copy);
    VkBufferMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = result.buffer;
    barrier.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                         0, nullptr, 1, &barrier, 0, nullptr);
    return result;
}

} // namespace osc::renderer
