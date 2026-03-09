#include "renderer/texture_cache.hpp"
#include "renderer/dds_parser.hpp"
#include "vfs/virtual_file_system.hpp"

#include <spdlog/spdlog.h>

#include <cstring>

namespace osc::renderer {

void TextureCache::init(VkDevice device, VmaAllocator allocator,
                        VkCommandPool cmd_pool, VkQueue queue,
                        VkDescriptorSetLayout ds_layout, VkSampler sampler,
                        vfs::VirtualFileSystem* vfs) {
    device_ = device;
    allocator_ = allocator;
    cmd_pool_ = cmd_pool;
    queue_ = queue;
    ds_layout_ = ds_layout;
    sampler_ = sampler;
    vfs_ = vfs;

    // Descriptor pool — up to 512 combined image samplers
    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_size.descriptorCount = 512;

    VkDescriptorPoolCreateInfo pool_ci{};
    pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_ci.maxSets = 512;
    pool_ci.poolSizeCount = 1;
    pool_ci.pPoolSizes = &pool_size;
    if (vkCreateDescriptorPool(device_, &pool_ci, nullptr, &descriptor_pool_) !=
        VK_SUCCESS) {
        spdlog::error("TextureCache: failed to create descriptor pool");
        descriptor_pool_ = VK_NULL_HANDLE;
        return;
    }

    create_fallback();
    create_specteam_fallback();
    create_normal_fallback();
}

void TextureCache::create_fallback() {
    // 1x1 white pixel (R8G8B8A8_UNORM)
    u8 white[] = {255, 255, 255, 255};

    // Staging buffer
    VkBufferCreateInfo staging_ci{};
    staging_ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    staging_ci.size = 4;
    staging_ci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

    VmaAllocationCreateInfo staging_alloc{};
    staging_alloc.usage = VMA_MEMORY_USAGE_CPU_ONLY;

    AllocatedBuffer staging{};
    vmaCreateBuffer(allocator_, &staging_ci, &staging_alloc,
                    &staging.buffer, &staging.allocation, nullptr);

    void* mapped = nullptr;
    vmaMapMemory(allocator_, staging.allocation, &mapped);
    std::memcpy(mapped, white, 4);
    vmaUnmapMemory(allocator_, staging.allocation);

    // Create 1x1 image
    VkImageCreateInfo img_ci{};
    img_ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    img_ci.imageType = VK_IMAGE_TYPE_2D;
    img_ci.format = VK_FORMAT_R8G8B8A8_UNORM;
    img_ci.extent = {1, 1, 1};
    img_ci.mipLevels = 1;
    img_ci.arrayLayers = 1;
    img_ci.samples = VK_SAMPLE_COUNT_1_BIT;
    img_ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    img_ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    img_ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo img_alloc{};
    img_alloc.usage = VMA_MEMORY_USAGE_GPU_ONLY;

    vmaCreateImage(allocator_, &img_ci, &img_alloc,
                   &fallback_.image.image, &fallback_.image.allocation, nullptr);

    // One-shot command: transition + copy + transition
    VkCommandBufferAllocateInfo cmd_ai{};
    cmd_ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmd_ai.commandPool = cmd_pool_;
    cmd_ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_ai.commandBufferCount = 1;

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device_, &cmd_ai, &cmd);

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin_info);

    // Transition UNDEFINED -> TRANSFER_DST
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = fallback_.image.image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);

    // Copy staging -> image
    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {1, 1, 1};
    vkCmdCopyBufferToImage(cmd, staging.buffer, fallback_.image.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // Transition TRANSFER_DST -> SHADER_READ_ONLY
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);

    vkEndCommandBuffer(cmd);

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vkQueueSubmit(queue_, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue_);

    vkFreeCommandBuffers(device_, cmd_pool_, 1, &cmd);
    vmaDestroyBuffer(allocator_, staging.buffer, staging.allocation);

    // Image view
    VkImageViewCreateInfo view_ci{};
    view_ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_ci.image = fallback_.image.image;
    view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_ci.format = VK_FORMAT_R8G8B8A8_UNORM;
    view_ci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_ci.subresourceRange.levelCount = 1;
    view_ci.subresourceRange.layerCount = 1;
    vkCreateImageView(device_, &view_ci, nullptr, &fallback_.image.view);

    // Descriptor set
    fallback_.descriptor_set = allocate_and_write_descriptor(fallback_.image.view);

    spdlog::debug("TextureCache: fallback 1x1 white created");
}

void TextureCache::create_specteam_fallback() {
    // 1x1 transparent black pixel (alpha=0 means "no team color here")
    u8 black[] = {0, 0, 0, 0};

    VkBufferCreateInfo staging_ci{};
    staging_ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    staging_ci.size = 4;
    staging_ci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

    VmaAllocationCreateInfo staging_alloc{};
    staging_alloc.usage = VMA_MEMORY_USAGE_CPU_ONLY;

    AllocatedBuffer staging{};
    if (vmaCreateBuffer(allocator_, &staging_ci, &staging_alloc,
                        &staging.buffer, &staging.allocation, nullptr) != VK_SUCCESS) {
        spdlog::error("TextureCache: specteam fallback staging alloc failed");
        return;
    }

    void* mapped = nullptr;
    vmaMapMemory(allocator_, staging.allocation, &mapped);
    std::memcpy(mapped, black, 4);
    vmaUnmapMemory(allocator_, staging.allocation);

    VkImageCreateInfo img_ci{};
    img_ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    img_ci.imageType = VK_IMAGE_TYPE_2D;
    img_ci.format = VK_FORMAT_R8G8B8A8_UNORM;
    img_ci.extent = {1, 1, 1};
    img_ci.mipLevels = 1;
    img_ci.arrayLayers = 1;
    img_ci.samples = VK_SAMPLE_COUNT_1_BIT;
    img_ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    img_ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    img_ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo img_alloc{};
    img_alloc.usage = VMA_MEMORY_USAGE_GPU_ONLY;

    if (vmaCreateImage(allocator_, &img_ci, &img_alloc,
                       &specteam_fallback_.image.image,
                       &specteam_fallback_.image.allocation, nullptr) != VK_SUCCESS) {
        vmaDestroyBuffer(allocator_, staging.buffer, staging.allocation);
        spdlog::error("TextureCache: specteam fallback image alloc failed");
        return;
    }

    VkCommandBufferAllocateInfo cmd_ai{};
    cmd_ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmd_ai.commandPool = cmd_pool_;
    cmd_ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_ai.commandBufferCount = 1;

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device_, &cmd_ai, &cmd);

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin_info);

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = specteam_fallback_.image.image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {1, 1, 1};
    vkCmdCopyBufferToImage(cmd, staging.buffer, specteam_fallback_.image.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);

    vkEndCommandBuffer(cmd);

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vkQueueSubmit(queue_, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue_);

    vkFreeCommandBuffers(device_, cmd_pool_, 1, &cmd);
    vmaDestroyBuffer(allocator_, staging.buffer, staging.allocation);

    VkImageViewCreateInfo view_ci{};
    view_ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_ci.image = specteam_fallback_.image.image;
    view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_ci.format = VK_FORMAT_R8G8B8A8_UNORM;
    view_ci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_ci.subresourceRange.levelCount = 1;
    view_ci.subresourceRange.layerCount = 1;
    vkCreateImageView(device_, &view_ci, nullptr,
                      &specteam_fallback_.image.view);

    specteam_fallback_.descriptor_set =
        allocate_and_write_descriptor(specteam_fallback_.image.view);

    spdlog::debug("TextureCache: specteam fallback 1x1 transparent created");
}

void TextureCache::create_normal_fallback() {
    // 1x1 flat-normal pixel — FA DXT5nm encoding: X=Green, Y=Alpha
    // Flat normal (0,0,1) in tangent space → G=128 (x≈0), A=128 (y≈0), z=sqrt(1)=1
    u8 flat_normal[] = {0, 128, 0, 128};  // RGBA: R=unused, G=128, B=unused, A=128

    VkBufferCreateInfo staging_ci{};
    staging_ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    staging_ci.size = 4;
    staging_ci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

    VmaAllocationCreateInfo staging_alloc{};
    staging_alloc.usage = VMA_MEMORY_USAGE_CPU_ONLY;

    AllocatedBuffer staging{};
    if (vmaCreateBuffer(allocator_, &staging_ci, &staging_alloc,
                        &staging.buffer, &staging.allocation, nullptr) != VK_SUCCESS) {
        spdlog::error("TextureCache: normal fallback staging alloc failed");
        return;
    }

    void* mapped = nullptr;
    if (vmaMapMemory(allocator_, staging.allocation, &mapped) != VK_SUCCESS) {
        vmaDestroyBuffer(allocator_, staging.buffer, staging.allocation);
        spdlog::error("TextureCache: normal fallback memory map failed");
        return;
    }
    std::memcpy(mapped, flat_normal, 4);
    vmaUnmapMemory(allocator_, staging.allocation);

    VkImageCreateInfo img_ci{};
    img_ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    img_ci.imageType = VK_IMAGE_TYPE_2D;
    img_ci.format = VK_FORMAT_R8G8B8A8_UNORM;
    img_ci.extent = {1, 1, 1};
    img_ci.mipLevels = 1;
    img_ci.arrayLayers = 1;
    img_ci.samples = VK_SAMPLE_COUNT_1_BIT;
    img_ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    img_ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    img_ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo img_alloc{};
    img_alloc.usage = VMA_MEMORY_USAGE_GPU_ONLY;

    if (vmaCreateImage(allocator_, &img_ci, &img_alloc,
                       &normal_fallback_.image.image,
                       &normal_fallback_.image.allocation, nullptr) != VK_SUCCESS) {
        vmaDestroyBuffer(allocator_, staging.buffer, staging.allocation);
        spdlog::error("TextureCache: normal fallback image alloc failed");
        return;
    }

    VkCommandBufferAllocateInfo cmd_ai{};
    cmd_ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmd_ai.commandPool = cmd_pool_;
    cmd_ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_ai.commandBufferCount = 1;

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device_, &cmd_ai, &cmd);

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin_info);

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = normal_fallback_.image.image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {1, 1, 1};
    vkCmdCopyBufferToImage(cmd, staging.buffer, normal_fallback_.image.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);

    vkEndCommandBuffer(cmd);

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vkQueueSubmit(queue_, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue_);

    vkFreeCommandBuffers(device_, cmd_pool_, 1, &cmd);
    vmaDestroyBuffer(allocator_, staging.buffer, staging.allocation);

    VkImageViewCreateInfo view_ci{};
    view_ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_ci.image = normal_fallback_.image.image;
    view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_ci.format = VK_FORMAT_R8G8B8A8_UNORM;
    view_ci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_ci.subresourceRange.levelCount = 1;
    view_ci.subresourceRange.layerCount = 1;
    vkCreateImageView(device_, &view_ci, nullptr,
                      &normal_fallback_.image.view);

    normal_fallback_.descriptor_set =
        allocate_and_write_descriptor(normal_fallback_.image.view);

    spdlog::debug("TextureCache: normal fallback 1x1 flat-normal created");
}

AllocatedImage TextureCache::upload_dds(const DDSTexture& dds) {
    AllocatedImage result{};

    // Total staging size for all mip levels
    VkDeviceSize total_size = 0;
    for (auto& mip : dds.mips)
        total_size += mip.size;

    // Staging buffer
    VkBufferCreateInfo staging_ci{};
    staging_ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    staging_ci.size = total_size;
    staging_ci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

    VmaAllocationCreateInfo staging_alloc{};
    staging_alloc.usage = VMA_MEMORY_USAGE_CPU_ONLY;

    AllocatedBuffer staging{};
    if (vmaCreateBuffer(allocator_, &staging_ci, &staging_alloc,
                        &staging.buffer, &staging.allocation,
                        nullptr) != VK_SUCCESS) {
        spdlog::warn("TextureCache: staging buffer creation failed");
        return {};
    }

    // Copy all mip data into staging buffer contiguously
    void* mapped = nullptr;
    vmaMapMemory(allocator_, staging.allocation, &mapped);
    size_t offset = 0;
    for (auto& mip : dds.mips) {
        std::memcpy(static_cast<char*>(mapped) + offset, mip.data, mip.size);
        offset += mip.size;
    }
    vmaUnmapMemory(allocator_, staging.allocation);

    // Create VkImage
    VkImageCreateInfo img_ci{};
    img_ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    img_ci.imageType = VK_IMAGE_TYPE_2D;
    img_ci.format = dds.format;
    img_ci.extent = {dds.width, dds.height, 1};
    img_ci.mipLevels = dds.mip_count;
    img_ci.arrayLayers = 1;
    img_ci.samples = VK_SAMPLE_COUNT_1_BIT;
    img_ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    img_ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    img_ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo img_alloc{};
    img_alloc.usage = VMA_MEMORY_USAGE_GPU_ONLY;

    if (vmaCreateImage(allocator_, &img_ci, &img_alloc,
                       &result.image, &result.allocation,
                       nullptr) != VK_SUCCESS) {
        spdlog::warn("TextureCache: image creation failed");
        vmaDestroyBuffer(allocator_, staging.buffer, staging.allocation);
        return {};
    }

    // One-shot command buffer
    VkCommandBufferAllocateInfo cmd_ai{};
    cmd_ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmd_ai.commandPool = cmd_pool_;
    cmd_ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_ai.commandBufferCount = 1;

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device_, &cmd_ai, &cmd);

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin_info);

    // Transition entire image UNDEFINED -> TRANSFER_DST
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = result.image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = dds.mip_count;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);

    // Copy each mip level from staging buffer
    VkDeviceSize buf_offset = 0;
    for (u32 i = 0; i < dds.mip_count; i++) {
        VkBufferImageCopy region{};
        region.bufferOffset = buf_offset;
        region.bufferRowLength = 0;   // tightly packed
        region.bufferImageHeight = 0; // tightly packed
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = i;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = 1;
        region.imageOffset = {0, 0, 0};
        region.imageExtent = {dds.mips[i].width, dds.mips[i].height, 1};

        vkCmdCopyBufferToImage(cmd, staging.buffer, result.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               1, &region);

        buf_offset += dds.mips[i].size;
    }

    // Transition TRANSFER_DST -> SHADER_READ_ONLY
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);

    vkEndCommandBuffer(cmd);

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vkQueueSubmit(queue_, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue_);

    vkFreeCommandBuffers(device_, cmd_pool_, 1, &cmd);
    vmaDestroyBuffer(allocator_, staging.buffer, staging.allocation);

    // Image view (all mip levels)
    VkImageViewCreateInfo view_ci{};
    view_ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_ci.image = result.image;
    view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_ci.format = dds.format;
    view_ci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_ci.subresourceRange.baseMipLevel = 0;
    view_ci.subresourceRange.levelCount = dds.mip_count;
    view_ci.subresourceRange.layerCount = 1;

    if (vkCreateImageView(device_, &view_ci, nullptr, &result.view) !=
        VK_SUCCESS) {
        spdlog::warn("TextureCache: image view creation failed");
        vmaDestroyImage(allocator_, result.image, result.allocation);
        result = {};
    }

    return result;
}

VkDescriptorSet TextureCache::allocate_and_write_descriptor(VkImageView view) {
    VkDescriptorSetAllocateInfo alloc_ci{};
    alloc_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_ci.descriptorPool = descriptor_pool_;
    alloc_ci.descriptorSetCount = 1;
    alloc_ci.pSetLayouts = &ds_layout_;

    VkDescriptorSet ds = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(device_, &alloc_ci, &ds) != VK_SUCCESS) {
        spdlog::warn("TextureCache: descriptor set allocation failed");
        return VK_NULL_HANDLE;
    }

    VkDescriptorImageInfo img_info{};
    img_info.sampler = sampler_;
    img_info.imageView = view;
    img_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = ds;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &img_info;

    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    return ds;
}

const GPUTexture* TextureCache::get_raw(const std::string& key,
                                        const std::vector<char>& raw_dds) {
    if (device_ == VK_NULL_HANDLE) return nullptr;

    auto it = cache_.find(key);
    if (it != cache_.end()) return it->second.get();

    if (failed_.count(key)) return nullptr;

    auto dds = parse_dds(raw_dds);
    if (!dds) {
        spdlog::debug("TextureCache: DDS parse failed for raw key '{}'", key);
        failed_.insert(key);
        return nullptr;
    }

    auto image = upload_dds(*dds);
    if (!image.image) {
        spdlog::debug("TextureCache: upload failed for raw key '{}'", key);
        failed_.insert(key);
        return nullptr;
    }

    auto ds = allocate_and_write_descriptor(image.view);
    if (ds == VK_NULL_HANDLE) {
        vkDestroyImageView(device_, image.view, nullptr);
        vmaDestroyImage(allocator_, image.image, image.allocation);
        failed_.insert(key);
        return nullptr;
    }

    auto gpu = std::make_unique<GPUTexture>();
    gpu->image = image;
    gpu->descriptor_set = ds;

    spdlog::debug("TextureCache: loaded raw '{}' ({}x{}, {} mips)",
                   key, dds->width, dds->height, dds->mip_count);

    auto* raw = gpu.get();
    cache_[key] = std::move(gpu);
    return raw;
}

const GPUTexture* TextureCache::upload_rgba(const std::string& key,
                                              const u8* pixels,
                                              u32 width, u32 height) {
    if (device_ == VK_NULL_HANDLE) return nullptr;

    auto it = cache_.find(key);
    if (it != cache_.end()) return it->second.get();

    u32 tex_size = width * height * 4;

    // Staging buffer
    VkBufferCreateInfo staging_ci{};
    staging_ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    staging_ci.size = tex_size;
    staging_ci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

    VmaAllocationCreateInfo staging_alloc{};
    staging_alloc.usage = VMA_MEMORY_USAGE_CPU_ONLY;

    AllocatedBuffer staging{};
    if (vmaCreateBuffer(allocator_, &staging_ci, &staging_alloc,
                        &staging.buffer, &staging.allocation, nullptr) != VK_SUCCESS) {
        return nullptr;
    }

    void* mapped = nullptr;
    vmaMapMemory(allocator_, staging.allocation, &mapped);
    std::memcpy(mapped, pixels, tex_size);
    vmaUnmapMemory(allocator_, staging.allocation);

    // Create image
    VkImageCreateInfo img_ci{};
    img_ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    img_ci.imageType = VK_IMAGE_TYPE_2D;
    img_ci.format = VK_FORMAT_R8G8B8A8_UNORM;
    img_ci.extent = {width, height, 1};
    img_ci.mipLevels = 1;
    img_ci.arrayLayers = 1;
    img_ci.samples = VK_SAMPLE_COUNT_1_BIT;
    img_ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    img_ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    img_ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo img_alloc{};
    img_alloc.usage = VMA_MEMORY_USAGE_GPU_ONLY;

    AllocatedImage image{};
    if (vmaCreateImage(allocator_, &img_ci, &img_alloc,
                       &image.image, &image.allocation, nullptr) != VK_SUCCESS) {
        vmaDestroyBuffer(allocator_, staging.buffer, staging.allocation);
        return nullptr;
    }

    // One-shot command: transition + copy + transition
    VkCommandBufferAllocateInfo cmd_ai{};
    cmd_ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmd_ai.commandPool = cmd_pool_;
    cmd_ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_ai.commandBufferCount = 1;

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device_, &cmd_ai, &cmd);

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin_info);

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image.image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {width, height, 1};
    vkCmdCopyBufferToImage(cmd, staging.buffer, image.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);

    vkEndCommandBuffer(cmd);

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vkQueueSubmit(queue_, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue_);
    vkFreeCommandBuffers(device_, cmd_pool_, 1, &cmd);
    vmaDestroyBuffer(allocator_, staging.buffer, staging.allocation);

    // Create image view
    VkImageViewCreateInfo view_ci{};
    view_ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_ci.image = image.image;
    view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_ci.format = VK_FORMAT_R8G8B8A8_UNORM;
    view_ci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_ci.subresourceRange.levelCount = 1;
    view_ci.subresourceRange.layerCount = 1;
    if (vkCreateImageView(device_, &view_ci, nullptr, &image.view) != VK_SUCCESS) {
        vmaDestroyImage(allocator_, image.image, image.allocation);
        return nullptr;
    }

    auto ds = allocate_and_write_descriptor(image.view);
    if (ds == VK_NULL_HANDLE) {
        vkDestroyImageView(device_, image.view, nullptr);
        vmaDestroyImage(allocator_, image.image, image.allocation);
        return nullptr;
    }

    auto gpu = std::make_unique<GPUTexture>();
    gpu->image = image;
    gpu->descriptor_set = ds;

    auto* result = gpu.get();
    cache_[key] = std::move(gpu);
    return result;
}

const GPUTexture* TextureCache::finalize_load(const std::string& path,
                                              const std::vector<char>& file_data) {
    auto dds = parse_dds(file_data);
    if (!dds) {
        spdlog::debug("TextureCache: DDS parse failed for '{}'", path);
        failed_.insert(path);
        return nullptr;
    }

    auto image = upload_dds(*dds);
    if (!image.image) {
        spdlog::debug("TextureCache: upload failed for '{}'", path);
        failed_.insert(path);
        return nullptr;
    }

    auto ds = allocate_and_write_descriptor(image.view);
    if (ds == VK_NULL_HANDLE) {
        vkDestroyImageView(device_, image.view, nullptr);
        vmaDestroyImage(allocator_, image.image, image.allocation);
        failed_.insert(path);
        return nullptr;
    }

    auto gpu = std::make_unique<GPUTexture>();
    gpu->image = image;
    gpu->descriptor_set = ds;

    spdlog::debug("TextureCache: loaded '{}' ({}x{}, {} mips)",
                   path, dds->width, dds->height, dds->mip_count);

    auto* raw = gpu.get();
    cache_[path] = std::move(gpu);
    return raw;
}

const GPUTexture* TextureCache::get(const std::string& vfs_path) {
    if (device_ == VK_NULL_HANDLE) return nullptr;

    auto it = cache_.find(vfs_path);
    if (it != cache_.end()) return it->second.get();

    if (failed_.count(vfs_path) || pending_.count(vfs_path)) return nullptr;

    if (!vfs_) {
        failed_.insert(vfs_path);
        return nullptr;
    }

    // Launch async VFS read on a background thread
    pending_.insert(vfs_path);
    auto* vfs = vfs_;
    async_loads_.push_back({vfs_path,
        std::async(std::launch::async, [vfs, vfs_path]()
            -> std::optional<std::vector<char>> {
            return vfs->read_file(vfs_path);
        })
    });
    return nullptr;
}

const GPUTexture* TextureCache::get_blocking(const std::string& vfs_path) {
    if (device_ == VK_NULL_HANDLE) return nullptr;

    auto it = cache_.find(vfs_path);
    if (it != cache_.end()) return it->second.get();

    if (failed_.count(vfs_path)) return nullptr;

    // If already pending async, block until that specific future completes
    if (pending_.count(vfs_path)) {
        for (auto it2 = async_loads_.begin(); it2 != async_loads_.end(); ++it2) {
            if (it2->path == vfs_path) {
                auto file_data = it2->future.get(); // truly blocks
                auto path = std::move(it2->path);
                async_loads_.erase(it2);
                pending_.erase(path);
                if (file_data)
                    return finalize_load(path, *file_data);
                failed_.insert(path);
                return nullptr;
            }
        }
        return nullptr;
    }

    if (!vfs_) {
        failed_.insert(vfs_path);
        return nullptr;
    }

    auto file_data = vfs_->read_file(vfs_path);
    if (!file_data) {
        spdlog::debug("TextureCache: VFS read failed for '{}'", vfs_path);
        failed_.insert(vfs_path);
        return nullptr;
    }

    return finalize_load(vfs_path, *file_data);
}

void TextureCache::flush_uploads(u32 max_per_frame) {
    u32 processed = 0;

    for (auto it = async_loads_.begin();
         it != async_loads_.end() && processed < max_per_frame; ) {
        // Check if the future is ready (non-blocking)
        auto status = it->future.wait_for(std::chrono::seconds(0));
        if (status != std::future_status::ready) {
            ++it;
            continue;
        }

        auto file_data = it->future.get();
        auto path = std::move(it->path);
        it = async_loads_.erase(it);
        pending_.erase(path);

        if (!file_data) {
            spdlog::debug("TextureCache: async VFS read failed for '{}'", path);
            failed_.insert(path);
        } else {
            finalize_load(path, *file_data);
        }
        processed++;
    }
}

void TextureCache::destroy(VkDevice device, VmaAllocator allocator) {
    // Drain all in-flight async loads before tearing down Vulkan resources
    for (auto& al : async_loads_)
        al.future.wait();
    async_loads_.clear();
    pending_.clear();

    for (auto& [path, gpu] : cache_) {
        if (gpu->image.view)
            vkDestroyImageView(device, gpu->image.view, nullptr);
        if (gpu->image.image)
            vmaDestroyImage(allocator, gpu->image.image, gpu->image.allocation);
    }
    cache_.clear();
    failed_.clear();

    // Fallbacks
    if (fallback_.image.view)
        vkDestroyImageView(device, fallback_.image.view, nullptr);
    if (fallback_.image.image)
        vmaDestroyImage(allocator, fallback_.image.image, fallback_.image.allocation);
    fallback_ = {};

    if (specteam_fallback_.image.view)
        vkDestroyImageView(device, specteam_fallback_.image.view, nullptr);
    if (specteam_fallback_.image.image)
        vmaDestroyImage(allocator, specteam_fallback_.image.image,
                        specteam_fallback_.image.allocation);
    specteam_fallback_ = {};

    if (normal_fallback_.image.view)
        vkDestroyImageView(device, normal_fallback_.image.view, nullptr);
    if (normal_fallback_.image.image)
        vmaDestroyImage(allocator, normal_fallback_.image.image,
                        normal_fallback_.image.allocation);
    normal_fallback_ = {};

    // Pool (frees all descriptor sets)
    if (descriptor_pool_)
        vkDestroyDescriptorPool(device, descriptor_pool_, nullptr);
    descriptor_pool_ = VK_NULL_HANDLE;
}

} // namespace osc::renderer
