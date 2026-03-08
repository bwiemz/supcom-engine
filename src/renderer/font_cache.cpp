#include "stb/stb_truetype.h"

#include "renderer/font_cache.hpp"
#include "vfs/virtual_file_system.hpp"

#include <spdlog/spdlog.h>
#include <vk_mem_alloc.h>

#include <algorithm>
#include <cstring>

// forward-declare from vk_utils.cpp
namespace osc::renderer {
void upload_buffer(VmaAllocator allocator, VkDevice device,
                   VkCommandPool pool, VkQueue queue,
                   const void* data, VkDeviceSize size,
                   VkBufferUsageFlags usage,
                   AllocatedImage& out_image,
                   u32 width, u32 height, VkFormat format);
}

namespace osc::renderer {

// Upload raw R8 pixel data to a GPU image (similar to texture_cache upload but for R8)
static AllocatedImage upload_r8_image(VmaAllocator allocator, VkDevice device,
                                       VkCommandPool pool, VkQueue queue,
                                       const u8* pixels, u32 width, u32 height) {
    AllocatedImage result{};

    // Create staging buffer
    VkBufferCreateInfo staging_ci{};
    staging_ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    staging_ci.size = static_cast<VkDeviceSize>(width) * height;
    staging_ci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

    VmaAllocationCreateInfo staging_alloc{};
    staging_alloc.usage = VMA_MEMORY_USAGE_CPU_ONLY;

    VkBuffer staging_buf = VK_NULL_HANDLE;
    VmaAllocation staging_alloc_handle = VK_NULL_HANDLE;
    if (vmaCreateBuffer(allocator, &staging_ci, &staging_alloc,
                        &staging_buf, &staging_alloc_handle,
                        nullptr) != VK_SUCCESS) {
        spdlog::error("FontCache: staging buffer creation failed");
        return result;
    }

    void* mapped = nullptr;
    vmaMapMemory(allocator, staging_alloc_handle, &mapped);
    std::memcpy(mapped, pixels, static_cast<size_t>(width) * height);
    vmaUnmapMemory(allocator, staging_alloc_handle);

    // Create GPU image (R8_UNORM)
    VkImageCreateInfo img_ci{};
    img_ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    img_ci.imageType = VK_IMAGE_TYPE_2D;
    img_ci.format = VK_FORMAT_R8_UNORM;
    img_ci.extent = {width, height, 1};
    img_ci.mipLevels = 1;
    img_ci.arrayLayers = 1;
    img_ci.samples = VK_SAMPLE_COUNT_1_BIT;
    img_ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    img_ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    img_ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo img_alloc{};
    img_alloc.usage = VMA_MEMORY_USAGE_GPU_ONLY;

    if (vmaCreateImage(allocator, &img_ci, &img_alloc,
                       &result.image, &result.allocation,
                       nullptr) != VK_SUCCESS) {
        spdlog::error("FontCache: image creation failed");
        vmaDestroyBuffer(allocator, staging_buf, staging_alloc_handle);
        return result;
    }

    // Record copy commands
    VkCommandBufferAllocateInfo cmd_ai{};
    cmd_ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmd_ai.commandPool = pool;
    cmd_ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_ai.commandBufferCount = 1;

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device, &cmd_ai, &cmd);

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);

    // Transition to TRANSFER_DST
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.image = result.image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);

    // Copy buffer to image
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {width, height, 1};
    vkCmdCopyBufferToImage(cmd, staging_buf, result.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // Transition to SHADER_READ_ONLY
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);

    vkEndCommandBuffer(cmd);

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);

    vkFreeCommandBuffers(device, pool, 1, &cmd);
    vmaDestroyBuffer(allocator, staging_buf, staging_alloc_handle);

    // Create image view — swizzle R8 to (1,1,1,R) so standard UI shader
    // renders text as fragColor * (1,1,1,glyphAlpha)
    VkImageViewCreateInfo view_ci{};
    view_ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_ci.image = result.image;
    view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_ci.format = VK_FORMAT_R8_UNORM;
    view_ci.components.r = VK_COMPONENT_SWIZZLE_ONE;
    view_ci.components.g = VK_COMPONENT_SWIZZLE_ONE;
    view_ci.components.b = VK_COMPONENT_SWIZZLE_ONE;
    view_ci.components.a = VK_COMPONENT_SWIZZLE_R;
    view_ci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCreateImageView(device, &view_ci, nullptr, &result.view);

    return result;
}

void FontCache::init(VkDevice device, VmaAllocator allocator,
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

    // Create descriptor pool for font atlas textures (64 max fonts)
    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_size.descriptorCount = 64;

    VkDescriptorPoolCreateInfo pool_ci{};
    pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_ci.maxSets = 64;
    pool_ci.poolSizeCount = 1;
    pool_ci.pPoolSizes = &pool_size;
    vkCreateDescriptorPool(device_, &pool_ci, nullptr, &descriptor_pool_);
}

std::string FontCache::make_key(const std::string& family, i32 pointsize) const {
    return family + ":" + std::to_string(pointsize);
}

std::string FontCache::resolve_font_path(const std::string& family) const {
    // Map common family names to font files in /fonts/
    std::string lower = family;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    if (lower == "arial" || lower == "arial bold" || lower.empty())
        return "/fonts/ARIAL.TTF";
    if (lower == "arial bold" || lower == "arial bd")
        return "/fonts/ARIALBD.TTF";
    if (lower == "arial italic")
        return "/fonts/ARIALI.TTF";
    if (lower == "arial narrow")
        return "/fonts/ARIALN.TTF";
    if (lower == "arial black")
        return "/fonts/ARIBLK.TTF";
    if (lower == "butterbe")
        return "/fonts/BUTTERBE.TTF";
    if (lower == "zeroes three" || lower == "zeroes_3")
        return "/fonts/zeroes_3.ttf";
    if (lower == "wintermu" || lower == "wintermute")
        return "/fonts/wintermu.ttf";
    if (lower == "arlrdbd")
        return "/fonts/ARLRDBD.TTF";
    if (lower == "vdub")
        return "/fonts/vdub.ttf";

    // Try direct path
    std::string direct = "/fonts/" + family + ".TTF";
    return direct;
}

FontAtlas* FontCache::load_font(const std::string& family, i32 pointsize) {
    std::string font_path = resolve_font_path(family);

    // Load TTF data from VFS (cache raw data for reuse at different sizes)
    auto ttf_it = ttf_data_cache_.find(font_path);
    if (ttf_it == ttf_data_cache_.end()) {
        if (!vfs_) return nullptr;
        auto data = vfs_->read_file(font_path);
        if (!data || data->empty()) {
            // Try lowercase
            std::string lower_path = font_path;
            std::transform(lower_path.begin(), lower_path.end(),
                           lower_path.begin(), ::tolower);
            data = vfs_->read_file(lower_path);
            if (!data || data->empty()) {
                spdlog::warn("FontCache: font not found: {}", font_path);
                return nullptr;
            }
            font_path = lower_path;
        }
        ttf_data_cache_[font_path] = std::move(*data);
        ttf_it = ttf_data_cache_.find(font_path);
    }

    const auto& ttf_data = ttf_it->second;

    // Initialize stb_truetype
    stbtt_fontinfo font;
    if (!stbtt_InitFont(&font,
                        reinterpret_cast<const unsigned char*>(ttf_data.data()),
                        stbtt_GetFontOffsetForIndex(
                            reinterpret_cast<const unsigned char*>(ttf_data.data()), 0))) {
        spdlog::error("FontCache: failed to init font: {}", font_path);
        return nullptr;
    }

    f32 scale = stbtt_ScaleForPixelHeight(&font, static_cast<f32>(pointsize));

    // Get font metrics
    int ascent_raw, descent_raw, line_gap_raw;
    stbtt_GetFontVMetrics(&font, &ascent_raw, &descent_raw, &line_gap_raw);

    auto atlas = std::make_unique<FontAtlas>();
    atlas->metrics.ascent = ascent_raw * scale;
    atlas->metrics.descent = -descent_raw * scale; // stb gives negative descent
    atlas->metrics.line_gap = line_gap_raw * scale;
    atlas->metrics.line_height = atlas->metrics.ascent + atlas->metrics.descent +
                                  atlas->metrics.line_gap;

    // Rasterize ASCII printable characters (32-126)
    constexpr u32 FIRST_CHAR = 32;
    constexpr u32 LAST_CHAR = 126;
    constexpr u32 CHAR_COUNT = LAST_CHAR - FIRST_CHAR + 1;

    // Determine atlas size (pack glyphs in rows)
    // Estimate: each glyph ~pointsize x pointsize, pack ~16 per row
    u32 cols = 16;
    u32 rows = (CHAR_COUNT + cols - 1) / cols;
    u32 cell_w = static_cast<u32>(pointsize) + 2;
    u32 cell_h = static_cast<u32>(pointsize) + 2;
    atlas->atlas_width = cols * cell_w;
    atlas->atlas_height = rows * cell_h;

    // Ensure power-of-two dimensions for compatibility
    auto next_pow2 = [](u32 v) -> u32 {
        v--;
        v |= v >> 1; v |= v >> 2; v |= v >> 4;
        v |= v >> 8; v |= v >> 16;
        return v + 1;
    };
    atlas->atlas_width = next_pow2(atlas->atlas_width);
    atlas->atlas_height = next_pow2(atlas->atlas_height);

    // Rasterize all glyphs into atlas bitmap
    std::vector<u8> bitmap(atlas->atlas_width * atlas->atlas_height, 0);

    for (u32 i = 0; i < CHAR_COUNT; i++) {
        u32 codepoint = FIRST_CHAR + i;
        u32 col = i % cols;
        u32 row = i / cols;
        u32 x0 = col * cell_w + 1; // 1px padding
        u32 y0 = row * cell_h + 1;

        int gw, gh, xoff, yoff;
        unsigned char* glyph_bmp = stbtt_GetCodepointBitmap(
            &font, 0, scale,
            static_cast<int>(codepoint),
            &gw, &gh, &xoff, &yoff);

        if (glyph_bmp) {
            // Clamp to cell bounds
            u32 copy_w = std::min(static_cast<u32>(gw), cell_w - 2);
            u32 copy_h = std::min(static_cast<u32>(gh), cell_h - 2);

            for (u32 py = 0; py < copy_h; py++) {
                for (u32 px = 0; px < copy_w; px++) {
                    u32 dst_x = x0 + px;
                    u32 dst_y = y0 + py;
                    if (dst_x < atlas->atlas_width && dst_y < atlas->atlas_height) {
                        bitmap[dst_y * atlas->atlas_width + dst_x] =
                            glyph_bmp[py * gw + px];
                    }
                }
            }

            stbtt_FreeBitmap(glyph_bmp, nullptr);

            // Store glyph info
            GlyphInfo gi{};
            gi.u0 = static_cast<f32>(x0) / atlas->atlas_width;
            gi.v0 = static_cast<f32>(y0) / atlas->atlas_height;
            gi.u1 = static_cast<f32>(x0 + copy_w) / atlas->atlas_width;
            gi.v1 = static_cast<f32>(y0 + copy_h) / atlas->atlas_height;
            gi.x_offset = static_cast<f32>(xoff);
            gi.y_offset = static_cast<f32>(yoff);
            gi.width = static_cast<f32>(copy_w);
            gi.height = static_cast<f32>(copy_h);

            int advance_raw, lsb;
            stbtt_GetCodepointHMetrics(&font, static_cast<int>(codepoint),
                                       &advance_raw, &lsb);
            gi.x_advance = advance_raw * scale;

            atlas->glyphs[codepoint] = gi;
        }
    }

    // Upload atlas to GPU
    if (device_ && allocator_) {
        atlas->image = upload_r8_image(allocator_, device_, cmd_pool_, queue_,
                                        bitmap.data(),
                                        atlas->atlas_width, atlas->atlas_height);

        // Create descriptor set
        if (atlas->image.view && descriptor_pool_) {
            VkDescriptorSetAllocateInfo ds_ai{};
            ds_ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            ds_ai.descriptorPool = descriptor_pool_;
            ds_ai.descriptorSetCount = 1;
            ds_ai.pSetLayouts = &ds_layout_;
            vkAllocateDescriptorSets(device_, &ds_ai, &atlas->descriptor_set);

            VkDescriptorImageInfo img_info{};
            img_info.sampler = sampler_;
            img_info.imageView = atlas->image.view;
            img_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkWriteDescriptorSet write{};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = atlas->descriptor_set;
            write.dstBinding = 0;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.pImageInfo = &img_info;
            vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
        }
    }

    std::string key = make_key(family, pointsize);
    auto* ptr = atlas.get();
    cache_[key] = std::move(atlas);

    spdlog::debug("FontCache: loaded {} {}pt ({} glyphs, {}x{} atlas)",
                  family, pointsize,
                  static_cast<u32>(ptr->glyphs.size()),
                  ptr->atlas_width, ptr->atlas_height);
    return ptr;
}

const FontAtlas* FontCache::get(const std::string& family, i32 pointsize) {
    std::string key = make_key(family, pointsize);
    auto it = cache_.find(key);
    if (it != cache_.end()) return it->second.get();
    return load_font(family, pointsize);
}

const FontMetrics* FontCache::get_metrics(const std::string& family, i32 pointsize) {
    const FontAtlas* atlas = get(family, pointsize);
    if (!atlas) return nullptr;
    return &atlas->metrics;
}

f32 FontCache::string_advance(const std::string& family, i32 pointsize,
                               const std::string& text) {
    const FontAtlas* atlas = get(family, pointsize);
    if (!atlas) {
        // Fallback to heuristic
        return static_cast<f32>(pointsize) * 0.6f *
               static_cast<f32>(text.size());
    }

    f32 advance = 0.0f;
    for (unsigned char c : text) {
        auto git = atlas->glyphs.find(static_cast<u32>(c));
        if (git != atlas->glyphs.end()) {
            advance += git->second.x_advance;
        } else {
            // Unknown glyph — use space width or heuristic
            auto space_it = atlas->glyphs.find(32);
            if (space_it != atlas->glyphs.end())
                advance += space_it->second.x_advance;
            else
                advance += static_cast<f32>(pointsize) * 0.6f;
        }
    }
    return advance;
}

void FontCache::destroy(VkDevice device, VmaAllocator allocator) {
    for (auto& [key, atlas] : cache_) {
        if (atlas->image.view)
            vkDestroyImageView(device, atlas->image.view, nullptr);
        if (atlas->image.image)
            vmaDestroyImage(allocator, atlas->image.image, atlas->image.allocation);
    }
    cache_.clear();
    ttf_data_cache_.clear();

    if (descriptor_pool_)
        vkDestroyDescriptorPool(device, descriptor_pool_, nullptr);
    descriptor_pool_ = VK_NULL_HANDLE;
}

} // namespace osc::renderer
