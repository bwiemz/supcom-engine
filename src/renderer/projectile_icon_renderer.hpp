#pragma once

#include "core/types.hpp"
#include "renderer/strategic_icon_renderer.hpp"
#include "renderer/vk_types.hpp"

#include <vulkan/vulkan.h>

#include <span>
#include <vector>

namespace osc::renderer {

/// Projectiles' icon textures, before the bloom as WRenViewport::Render draws its world views'
/// overlays: primbatcher.fx's TAlphaBlendLinearSampleNoDepth, then TCommandGlow's alpha.
class ProjectileIconRenderer {
public:
    void init(VkDevice device, VmaAllocator allocator, VkRenderPass render_pass,
              VkDescriptorSetLayout texture_ds_layout);

    void update(std::span<const ProjectileIcon> icons, u32 fi);

    void render(VkCommandBuffer cmd, u32 viewport_w, u32 viewport_h, u32 fi) const;

    void destroy(VkDevice device, VmaAllocator allocator);

    static constexpr u32 MAX_ICONS = 1024;
    static constexpr u32 FRAMES_IN_FLIGHT = 2;

private:
    struct Instance {
        f32 rect[4];
        f32 glow;
    };
    struct Group {
        VkDescriptorSet ds = VK_NULL_HANDLE;
        u32 first = 0, count = 0;
    };

    VkPipeline colour_pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout colour_layout_ = VK_NULL_HANDLE;
    VkPipeline glow_pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout glow_layout_ = VK_NULL_HANDLE;
    AllocatedBuffer instance_buf_[FRAMES_IN_FLIGHT] = {};
    void* instance_mapped_[FRAMES_IN_FLIGHT] = {};
    std::vector<Group> groups_;
};

} // namespace osc::renderer
