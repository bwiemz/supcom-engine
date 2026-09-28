#include "renderer/movie_textures.hpp"

#include "renderer/texture_cache.hpp"
#include "ui/ui_control.hpp"
#include "video/movie_player.hpp"

#include <spdlog/spdlog.h>

#include <cstring>
#include <unordered_set>

namespace osc::renderer {

void MovieTextures::init(VkDevice device, VmaAllocator allocator, TextureCache* textures) {
    device_ = device;
    allocator_ = allocator;
    textures_ = textures;
}

bool MovieTextures::create(Entry& e, u32 width, u32 height) {
    e.width = width;
    e.height = height;
    VkImageCreateInfo image_ci{};
    image_ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_ci.imageType = VK_IMAGE_TYPE_2D;
    image_ci.format = VK_FORMAT_R8G8B8A8_UNORM;
    image_ci.extent = {width, height, 1};
    image_ci.mipLevels = 1;
    image_ci.arrayLayers = 1;
    image_ci.samples = VK_SAMPLE_COUNT_1_BIT;
    image_ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    image_ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo image_alloc{};
    image_alloc.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    if (vmaCreateImage(allocator_, &image_ci, &image_alloc, &e.image.image, &e.image.allocation,
                       nullptr) != VK_SUCCESS)
        return false;

    VkImageViewCreateInfo view_ci{};
    view_ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_ci.image = e.image.image;
    view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_ci.format = VK_FORMAT_R8G8B8A8_UNORM;
    view_ci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_ci.subresourceRange.levelCount = 1;
    view_ci.subresourceRange.layerCount = 1;
    if (vkCreateImageView(device_, &view_ci, nullptr, &e.image.view) != VK_SUCCESS) return false;

    const VkDeviceSize bytes = static_cast<VkDeviceSize>(width) * height * 4;
    for (u32 i = 0; i < FRAMES_IN_FLIGHT; ++i) {
        VkBufferCreateInfo buf_ci{};
        buf_ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buf_ci.size = bytes;
        buf_ci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        VmaAllocationCreateInfo buf_alloc{};
        buf_alloc.usage = VMA_MEMORY_USAGE_CPU_ONLY;
        buf_alloc.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo info{};
        if (vmaCreateBuffer(allocator_, &buf_ci, &buf_alloc, &e.staging[i].buffer,
                            &e.staging[i].allocation, &info) != VK_SUCCESS)
            return false;
        e.mapped[i] = info.pMappedData;
    }
    e.descriptor = textures_->make_descriptor(e.image.view);
    return e.descriptor != VK_NULL_HANDLE;
}

void MovieTextures::free_entry(Entry& e) {
    for (u32 i = 0; i < FRAMES_IN_FLIGHT; ++i)
        if (e.staging[i].buffer)
            vmaDestroyBuffer(allocator_, e.staging[i].buffer, e.staging[i].allocation);
    if (e.image.view) vkDestroyImageView(device_, e.image.view, nullptr);
    if (e.image.image) vmaDestroyImage(allocator_, e.image.image, e.image.allocation);
    e = Entry{};
    // Its descriptor set stays in the texture cache's pool, unused.
}

void MovieTextures::retire(const Entry& e) {
    // The frames in flight may still sample it.
    retired_.emplace_back(frame_ + FRAMES_IN_FLIGHT, e);
}

void MovieTextures::prepare(const ui::UIControlRegistry& registry, u32 fi) {
    ++frame_;
    for (auto it = retired_.begin(); it != retired_.end();) {
        if (it->first < frame_) {
            free_entry(it->second);
            it = retired_.erase(it);
        } else {
            ++it;
        }
    }

    std::unordered_set<const ui::UIControl*> seen;
    for (const auto& ptr : registry.all()) {
        const ui::UIControl* ctrl = ptr.get();
        if (!ctrl || ctrl->destroyed()) continue;
        const video::MoviePlayer* movie = ctrl->movie_player();
        if (!movie || !movie->loaded()) continue;
        seen.insert(ctrl);

        auto found = entries_.find(ctrl);
        if (found != entries_.end() && found->second.movie_id != movie->id()) {
            retire(found->second);
            entries_.erase(found);
            found = entries_.end();
        }
        if (found == entries_.end()) {
            Entry e;
            if (!create(e, movie->width(), movie->height())) {
                spdlog::warn("MovieTextures: no texture for a {}x{} movie", movie->width(),
                             movie->height());
                free_entry(e);
                continue;
            }
            e.movie_id = movie->id();
            found = entries_.emplace(ctrl, e).first;
        }
        Entry& e = found->second;
        e.staged_serial = 0;
        if (movie->frame_serial() != e.uploaded_serial && movie->rgba()) {
            std::memcpy(e.mapped[fi], movie->rgba(), static_cast<size_t>(e.width) * e.height * 4);
            e.staged_serial = movie->frame_serial();
            e.staged_slot = fi;
        }
    }
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (!seen.count(it->first)) {
            retire(it->second);
            it = entries_.erase(it);
        } else {
            ++it;
        }
    }
}

void MovieTextures::record(VkCommandBuffer cmd) {
    for (auto& [ctrl, e] : entries_) {
        if (e.staged_serial == 0) continue;
        VkImageMemoryBarrier to_dst{};
        to_dst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        to_dst.oldLayout = e.shader_readable ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                             : VK_IMAGE_LAYOUT_UNDEFINED;
        to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_dst.image = e.image.image;
        to_dst.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        to_dst.subresourceRange.levelCount = 1;
        to_dst.subresourceRange.layerCount = 1;
        // The last frame's UI may still be sampling it.
        to_dst.srcAccessMask = e.shader_readable ? VK_ACCESS_SHADER_READ_BIT : 0;
        to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_dst);

        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {e.width, e.height, 1};
        vkCmdCopyBufferToImage(cmd, e.staging[e.staged_slot].buffer, e.image.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        VkImageMemoryBarrier to_read = to_dst;
        to_read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        to_read.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        to_read.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &to_read);
        e.shader_readable = true;
        e.uploaded_serial = e.staged_serial;
        e.staged_serial = 0;
    }
}

VkDescriptorSet MovieTextures::descriptor(const ui::UIControl* ctrl) const {
    const auto it = entries_.find(ctrl);
    if (it == entries_.end() || !it->second.shader_readable) return VK_NULL_HANDLE;
    return it->second.descriptor;
}

void MovieTextures::forget() {
    for (auto& [ctrl, e] : entries_) retire(e);
    entries_.clear();
}

void MovieTextures::destroy() {
    for (auto& [ctrl, e] : entries_) free_entry(e);
    entries_.clear();
    for (auto& [when, e] : retired_) free_entry(e);
    retired_.clear();
}

} // namespace osc::renderer
