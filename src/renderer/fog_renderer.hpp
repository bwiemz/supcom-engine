#pragma once

#include "renderer/vk_types.hpp"
#include "core/types.hpp"
#include <vector>

namespace osc::sim {
struct SightMap;
}

namespace osc::renderer {

/// Manages a GPU texture representing fog of war state for one army, a
/// texel per cell of its vision grid (M215g). The texture is R8: 255 where
/// the army sees, 0 where it doesn't. FA keeps no explored ground: what
/// isn't in sight is fogged alike.
class FogRenderer {
public:
    void init(u32 grid_width, u32 grid_height,
              VkDevice device, VmaAllocator allocator,
              VkCommandPool cmd_pool, VkQueue queue);

    /// Fill staging buffer from an army's sight (CPU side); all visible when
    /// it has no fog of war, or its grid isn't the texture's size.
    void stage(const osc::sim::SightMap& sight);

    /// Fill staging buffer as all visible (an observer's view).
    void stage_clear();

    /// Record barriers + copy into an existing command buffer (GPU side).
    void record_upload(VkCommandBuffer cmd);

    void destroy(VkDevice device, VmaAllocator allocator);

    VkImageView image_view() const { return image_view_; }
    VkSampler sampler() const { return sampler_; }
    bool initialized() const { return initialized_; }

    u32 grid_width() const { return grid_width_; }
    u32 grid_height() const { return grid_height_; }
    void set_frame_index(u32 fi) { fi_ = fi; }

    static constexpr u32 FRAMES_IN_FLIGHT = 2;

private:
    u32 fi_ = 0;
    AllocatedImage image_{};
    VkImageView image_view_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    AllocatedBuffer staging_[FRAMES_IN_FLIGHT] = {};
    u8* staging_mapped_[FRAMES_IN_FLIGHT] = {};
    u32 grid_width_ = 0;
    u32 grid_height_ = 0;
    bool initialized_ = false;
};

} // namespace osc::renderer
