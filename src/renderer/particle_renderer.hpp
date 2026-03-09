#pragma once

#include "renderer/particle_system.hpp"
#include "renderer/vk_types.hpp"
#include "core/types.hpp"

#include <vulkan/vulkan.h>

#include <vector>

namespace osc::renderer {

class TextureCache;
class EmitterBlueprintCache;

/// GPU particle renderer: uploads ParticleInstance data, issues instanced
/// billboard draw calls grouped by texture (blend mode).
class ParticleRenderer {
public:
    void init(VkDevice device, VmaAllocator allocator,
              VkRenderPass render_pass,
              VkDescriptorSetLayout texture_ds_layout,
              VkSampler sampler);

    /// Upload instance data and build draw groups by texture.
    void update(const std::vector<ParticleInstance>& instances,
                const ParticleSystem& psys,
                TextureCache& tex_cache, u32 fi);

    /// Record draw commands. Caller must NOT have any pipeline bound —
    /// this binds its own pipeline.
    void render(VkCommandBuffer cmd, u32 viewport_w, u32 viewport_h,
                const f32* view_proj, const f32* cam_right, const f32* cam_up,
                u32 fi);

    void destroy(VkDevice device, VmaAllocator allocator);

    u32 draw_count() const { return draw_count_; }

    static constexpr u32 MAX_PARTICLES = ParticleSystem::MAX_PARTICLES;
    static constexpr u32 FRAMES_IN_FLIGHT = 2;

private:
    struct DrawGroup {
        VkDescriptorSet texture_ds = VK_NULL_HANDLE;
        u32 instance_offset = 0;
        u32 instance_count = 0;
        bool additive = false;
    };

    VkPipeline alpha_pipeline_ = VK_NULL_HANDLE;
    VkPipeline additive_pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;

    AllocatedBuffer instance_buf_[FRAMES_IN_FLIGHT] = {};
    void* instance_mapped_[FRAMES_IN_FLIGHT] = {};

    std::vector<DrawGroup> groups_;
    u32 draw_count_ = 0;
    u32 instance_count_ = 0;

    VkDevice device_ = VK_NULL_HANDLE;
};

} // namespace osc::renderer
