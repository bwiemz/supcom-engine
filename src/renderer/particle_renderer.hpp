#pragma once

#include "core/types.hpp"
#include "renderer/particle_system.hpp"
#include "renderer/vk_types.hpp"

#include <vulkan/vulkan.h>

#include <array>
#include <vector>

namespace osc::renderer {

class TextureCache;

/// Draws FA's particles (M214c): each particle's quad, as ParticleSystem
/// placed it, its texture times its ramp (particle.fx's WorldPS), blended
/// by its emitter's TRamp technique. The refracting ones (M214d) apart:
/// the frame behind them, displaced (WorldRefractPS).
class ParticleRenderer {
public:
    void init(VkDevice device, VmaAllocator allocator, VkRenderPass render_pass,
              VkDescriptorSetLayout texture_ds_layout);

    /// Upload this frame's quads and runs.
    void update(const ParticleSystem& particles, TextureCache& tex_cache, u32 fi);

    /// Draw the particles under the water (a negative SortOrder's) or the
    /// others, but the refracting; the scene pass must be open.
    void render(VkCommandBuffer cmd, u32 viewport_w, u32 viewport_h, const f32* view_proj,
                bool under_water, u32 fi) const;

    /// The copy of the frame the refracting particles bend (M214d; set again
    /// after a resize).
    void set_background(VkImageView view);
    /// Whether this frame draws refracting particles, which want the frame
    /// copied before them.
    bool refracting() const;
    /// Draw the refracting particles, last, over the copy set_background
    /// names; the scene pass must be open.
    void render_refracting(VkCommandBuffer cmd, u32 viewport_w, u32 viewport_h,
                           const f32* view_proj, u32 fi) const;

    void destroy(VkDevice device, VmaAllocator allocator);

    u32 draw_count() const { return draw_count_; }

    static constexpr u32 MAX_PARTICLES = ParticleSystem::MAX_PARTICLES;
    static constexpr u32 FRAMES_IN_FLIGHT = 2;

private:
    struct Group {
        bool under_water = false;
        i32 blendmode = 0;
        bool light = false;
        VkDescriptorSet texture = VK_NULL_HANDLE, ramp = VK_NULL_HANDLE;
        u32 offset = 0, count = 0;
    };

    std::array<VkPipeline, 5> pipelines_{};
    VkPipeline light_pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    // The refracting ones' (M214d): their pipeline, and the background, a
    // third set.
    VkDevice device_ = VK_NULL_HANDLE;
    VkPipeline refract_pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout refract_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool background_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet background_set_ = VK_NULL_HANDLE;
    VkSampler background_sampler_ = VK_NULL_HANDLE;
    bool background_ready_ = false;
    AllocatedBuffer instance_buf_[FRAMES_IN_FLIGHT] = {};
    void* instance_mapped_[FRAMES_IN_FLIGHT] = {};
    std::vector<Group> groups_;
    u32 draw_count_ = 0;
};

} // namespace osc::renderer
