#pragma once

#include "renderer/vk_types.hpp"
#include "core/types.hpp"

#include <vulkan/vulkan.h>

#include <string>
#include <unordered_map>
#include <vector>

namespace osc::vfs {
class VirtualFileSystem;
}

namespace osc::renderer {

/// Per-glyph data for a rasterized character.
struct GlyphInfo {
    f32 u0, v0, u1, v1;  // UV coords in atlas
    f32 x_offset;         // left bearing in pixels
    f32 y_offset;         // top bearing in pixels (from baseline)
    f32 width;            // glyph bitmap width in pixels
    f32 height;           // glyph bitmap height in pixels
    f32 x_advance;        // horizontal advance in pixels
};

/// Font metrics for a loaded font at a specific point size.
struct FontMetrics {
    f32 ascent;           // pixels above baseline
    f32 descent;          // pixels below baseline (positive value)
    f32 line_gap;         // extra leading between lines
    f32 line_height;      // ascent + descent + line_gap
};

/// A GPU-resident font atlas with glyph data.
struct FontAtlas {
    AllocatedImage image{};
    VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
    FontMetrics metrics{};
    std::unordered_map<u32, GlyphInfo> glyphs; // codepoint → glyph
    u32 atlas_width = 0;
    u32 atlas_height = 0;
};

/// Caches rasterized font atlases keyed by (family, pointsize).
class FontCache {
public:
    void init(VkDevice device, VmaAllocator allocator,
              VkCommandPool cmd_pool, VkQueue queue,
              VkDescriptorSetLayout ds_layout, VkSampler sampler,
              vfs::VirtualFileSystem* vfs);

    /// Get or lazily load a font atlas for a given family and pointsize.
    /// Returns nullptr if the font cannot be loaded.
    const FontAtlas* get(const std::string& family, i32 pointsize);

    /// Get the metrics for a font (loads if needed).
    const FontMetrics* get_metrics(const std::string& family, i32 pointsize);

    /// Compute pixel width of a string at a given font and size.
    f32 string_advance(const std::string& family, i32 pointsize,
                       const std::string& text);

    void destroy(VkDevice device, VmaAllocator allocator);

private:
    std::string make_key(const std::string& family, i32 pointsize) const;
    std::string resolve_font_path(const std::string& family) const;
    FontAtlas* load_font(const std::string& family, i32 pointsize);

    std::unordered_map<std::string, std::unique_ptr<FontAtlas>> cache_;
    std::unordered_map<std::string, std::vector<char>> ttf_data_cache_;

    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout ds_layout_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;

    VkDevice device_ = VK_NULL_HANDLE;
    VmaAllocator allocator_ = VK_NULL_HANDLE;
    VkCommandPool cmd_pool_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    vfs::VirtualFileSystem* vfs_ = nullptr;
};

} // namespace osc::renderer
