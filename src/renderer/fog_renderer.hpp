#pragma once

#include "renderer/vk_types.hpp"
#include "core/types.hpp"

namespace osc::map { class VisibilityGrid; }

namespace osc::renderer {

/// Manages a GPU texture representing fog of war state for one army.
/// The texture is R8 format where:
///   255 = fully visible (has vision)
///   128 = explored but not visible (ever_seen but no current vision)
///     0 = unexplored (never seen)
class FogRenderer {
public:
    void init(u32 grid_width, u32 grid_height,
              VkDevice device, VmaAllocator allocator,
              VkCommandPool cmd_pool, VkQueue queue);

    /// Fill staging buffer from visibility grid (CPU side).
    void stage(const osc::map::VisibilityGrid& grid, u32 army);

    /// Record barriers + copy into an existing command buffer (GPU side).
    void record_upload(VkCommandBuffer cmd);

    void destroy(VkDevice device, VmaAllocator allocator);

    VkImageView image_view() const { return image_view_; }
    VkSampler sampler() const { return sampler_; }
    bool initialized() const { return initialized_; }

    u32 grid_width() const { return grid_width_; }
    u32 grid_height() const { return grid_height_; }

private:
    AllocatedImage image_{};
    VkImageView image_view_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    AllocatedBuffer staging_{};
    u8* staging_mapped_ = nullptr;
    u32 grid_width_ = 0;
    u32 grid_height_ = 0;
    bool initialized_ = false;
};

} // namespace osc::renderer
