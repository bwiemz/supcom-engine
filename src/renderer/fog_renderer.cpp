#include "renderer/fog_renderer.hpp"
#include "renderer/vk_types.hpp"
#include "map/visibility_grid.hpp"

#include <spdlog/spdlog.h>

#include <cstring>

namespace osc::renderer {

void FogRenderer::init(u32 grid_width, u32 grid_height,
                       VkDevice device, VmaAllocator allocator,
                       VkCommandPool cmd_pool, VkQueue queue) {
    grid_width_ = grid_width;
    grid_height_ = grid_height;
    u32 pixel_count = grid_width_ * grid_height_;

    // Create R8_UNORM image for fog of war
    VkImageCreateInfo img_ci{};
    img_ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    img_ci.imageType = VK_IMAGE_TYPE_2D;
    img_ci.format = VK_FORMAT_R8_UNORM;
    img_ci.extent = {grid_width_, grid_height_, 1};
    img_ci.mipLevels = 1;
    img_ci.arrayLayers = 1;
    img_ci.samples = VK_SAMPLE_COUNT_1_BIT;
    img_ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    img_ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    img_ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo img_alloc{};
    img_alloc.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    vmaCreateImage(allocator, &img_ci, &img_alloc,
                   &image_.image, &image_.allocation, nullptr);

    // Image view
    VkImageViewCreateInfo view_ci{};
    view_ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_ci.image = image_.image;
    view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_ci.format = VK_FORMAT_R8_UNORM;
    view_ci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_ci.subresourceRange.levelCount = 1;
    view_ci.subresourceRange.layerCount = 1;
    vkCreateImageView(device, &view_ci, nullptr, &image_view_);

    // Sampler: bilinear, clamp to edge for smooth fog transitions
    VkSamplerCreateInfo samp_ci{};
    samp_ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samp_ci.magFilter = VK_FILTER_LINEAR;
    samp_ci.minFilter = VK_FILTER_LINEAR;
    samp_ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samp_ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(device, &samp_ci, nullptr, &sampler_);

    // Persistently mapped staging buffer
    VkBufferCreateInfo buf_ci{};
    buf_ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buf_ci.size = pixel_count;
    buf_ci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

    VmaAllocationCreateInfo staging_alloc{};
    staging_alloc.usage = VMA_MEMORY_USAGE_CPU_ONLY;
    staging_alloc.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
    staging_alloc.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

    for (u32 i = 0; i < FRAMES_IN_FLIGHT; i++) {
        VmaAllocationInfo staging_info{};
        vmaCreateBuffer(allocator, &buf_ci, &staging_alloc,
                        &staging_[i].buffer, &staging_[i].allocation,
                        &staging_info);
        staging_mapped_[i] = static_cast<u8*>(staging_info.pMappedData);
        std::memset(staging_mapped_[i], 255, pixel_count);
    }

    // Initial upload: transition to TRANSFER_DST, copy, transition to SHADER_READ
    VkCommandBufferAllocateInfo cmd_ai{};
    cmd_ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmd_ai.commandPool = cmd_pool;
    cmd_ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_ai.commandBufferCount = 1;

    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(device, &cmd_ai, &cmd);

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);

    // Transition UNDEFINED -> TRANSFER_DST
    VkImageMemoryBarrier to_dst{};
    to_dst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_dst.image = image_.image;
    to_dst.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    to_dst.subresourceRange.levelCount = 1;
    to_dst.subresourceRange.layerCount = 1;
    to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &to_dst);

    // Copy staging -> image
    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {grid_width_, grid_height_, 1};
    vkCmdCopyBufferToImage(cmd, staging_[0].buffer, image_.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // Transition TRANSFER_DST -> SHADER_READ
    VkImageMemoryBarrier to_read{};
    to_read.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    to_read.image = image_.image;
    to_read.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    to_read.subresourceRange.levelCount = 1;
    to_read.subresourceRange.layerCount = 1;
    to_read.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_read.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &to_read);

    vkEndCommandBuffer(cmd);

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);
    vkFreeCommandBuffers(device, cmd_pool, 1, &cmd);

    initialized_ = true;
    spdlog::info("Fog of war texture: {}x{} R8", grid_width_, grid_height_);
}

void FogRenderer::stage(const osc::map::VisibilityGrid& grid, u32 army) {
    if (!initialized_ || !staging_mapped_[fi_]) return;

    // Write raw visibility data to temp buffer
    if (raw_grid_.size() != grid_width_ * grid_height_)
        raw_grid_.resize(grid_width_ * grid_height_, 255);

    for (u32 gz = 0; gz < grid_height_; gz++) {
        for (u32 gx = 0; gx < grid_width_; gx++) {
            auto flags = grid.get(gx, gz, army);
            u8 val = 0; // unexplored
            if (map::has_flag(flags, map::VisFlag::Vision))
                val = 255; // fully visible
            else if (map::has_flag(flags, map::VisFlag::Radar) ||
                     map::has_flag(flags, map::VisFlag::Omni))
                val = 200; // radar coverage (slightly dimmed)
            else if (map::has_flag(flags, map::VisFlag::EverSeen))
                val = 100; // explored but dark
            raw_grid_[gz * grid_width_ + gx] = val;
        }
    }

    // Two-pass box blur (radius=2) for smooth FoW transitions
    blur_to_staging();
}

void FogRenderer::blur_to_staging() {
    u32 w = grid_width_;
    u32 h = grid_height_;
    constexpr i32 R = 2; // blur radius

    // Horizontal blur: raw_grid_ -> staging
    for (u32 y = 0; y < h; y++) {
        for (u32 x = 0; x < w; x++) {
            i32 sum = 0;
            i32 count = 0;
            for (i32 dx = -R; dx <= R; dx++) {
                i32 sx = static_cast<i32>(x) + dx;
                if (sx >= 0 && sx < static_cast<i32>(w)) {
                    sum += raw_grid_[y * w + sx];
                    count++;
                }
            }
            staging_mapped_[fi_][y * w + x] = static_cast<u8>(sum / count);
        }
    }

    // Copy staging back to raw for vertical pass
    std::memcpy(raw_grid_.data(), staging_mapped_[fi_], w * h);

    // Vertical blur: raw_grid_ -> staging
    for (u32 y = 0; y < h; y++) {
        for (u32 x = 0; x < w; x++) {
            i32 sum = 0;
            i32 count = 0;
            for (i32 dy = -R; dy <= R; dy++) {
                i32 sy = static_cast<i32>(y) + dy;
                if (sy >= 0 && sy < static_cast<i32>(h)) {
                    sum += raw_grid_[sy * w + x];
                    count++;
                }
            }
            staging_mapped_[fi_][y * w + x] = static_cast<u8>(sum / count);
        }
    }
}

void FogRenderer::record_upload(VkCommandBuffer cmd) {
    if (!initialized_) return;

    // SHADER_READ -> TRANSFER_DST
    VkImageMemoryBarrier to_dst{};
    to_dst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_dst.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_dst.image = image_.image;
    to_dst.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    to_dst.subresourceRange.levelCount = 1;
    to_dst.subresourceRange.layerCount = 1;
    to_dst.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &to_dst);

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {grid_width_, grid_height_, 1};
    vkCmdCopyBufferToImage(cmd, staging_[fi_].buffer, image_.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // TRANSFER_DST -> SHADER_READ
    VkImageMemoryBarrier to_read{};
    to_read.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    to_read.image = image_.image;
    to_read.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    to_read.subresourceRange.levelCount = 1;
    to_read.subresourceRange.layerCount = 1;
    to_read.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_read.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &to_read);
}

void FogRenderer::destroy(VkDevice device, VmaAllocator allocator) {
    if (sampler_) vkDestroySampler(device, sampler_, nullptr);
    if (image_view_) vkDestroyImageView(device, image_view_, nullptr);
    if (image_.image) vmaDestroyImage(allocator, image_.image, image_.allocation);
    for (u32 i = 0; i < FRAMES_IN_FLIGHT; i++) {
        if (staging_[i].buffer)
            vmaDestroyBuffer(allocator, staging_[i].buffer,
                             staging_[i].allocation);
        staging_[i] = {};
        staging_mapped_[i] = nullptr;
    }
    sampler_ = VK_NULL_HANDLE;
    image_view_ = VK_NULL_HANDLE;
    image_ = {};
    initialized_ = false;
}

} // namespace osc::renderer
