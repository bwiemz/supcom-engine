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

const GPUTexture* TextureCache::get(const std::string& vfs_path) {
    if (device_ == VK_NULL_HANDLE) return nullptr;

    auto it = cache_.find(vfs_path);
    if (it != cache_.end()) return it->second.get();

    if (failed_.count(vfs_path)) return nullptr;

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

    auto dds = parse_dds(*file_data);
    if (!dds) {
        spdlog::debug("TextureCache: DDS parse failed for '{}'", vfs_path);
        failed_.insert(vfs_path);
        return nullptr;
    }

    auto image = upload_dds(*dds);
    if (!image.image) {
        spdlog::debug("TextureCache: upload failed for '{}'", vfs_path);
        failed_.insert(vfs_path);
        return nullptr;
    }

    auto ds = allocate_and_write_descriptor(image.view);
    if (ds == VK_NULL_HANDLE) {
        vkDestroyImageView(device_, image.view, nullptr);
        vmaDestroyImage(allocator_, image.image, image.allocation);
        failed_.insert(vfs_path);
        return nullptr;
    }

    auto gpu = std::make_unique<GPUTexture>();
    gpu->image = image;
    gpu->descriptor_set = ds;

    spdlog::debug("TextureCache: loaded '{}' ({}x{}, {} mips)",
                   vfs_path, dds->width, dds->height, dds->mip_count);

    auto* raw = gpu.get();
    cache_[vfs_path] = std::move(gpu);
    return raw;
}

void TextureCache::destroy(VkDevice device, VmaAllocator allocator) {
    for (auto& [path, gpu] : cache_) {
        if (gpu->image.view)
            vkDestroyImageView(device, gpu->image.view, nullptr);
        if (gpu->image.image)
            vmaDestroyImage(allocator, gpu->image.image, gpu->image.allocation);
    }
    cache_.clear();
    failed_.clear();

    // Fallback
    if (fallback_.image.view)
        vkDestroyImageView(device, fallback_.image.view, nullptr);
    if (fallback_.image.image)
        vmaDestroyImage(allocator, fallback_.image.image, fallback_.image.allocation);
    fallback_ = {};

    // Pool (frees all descriptor sets)
    if (descriptor_pool_)
        vkDestroyDescriptorPool(device, descriptor_pool_, nullptr);
    descriptor_pool_ = VK_NULL_HANDLE;
}

} // namespace osc::renderer
