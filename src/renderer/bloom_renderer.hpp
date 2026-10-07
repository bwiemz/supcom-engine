#pragma once

// FA's bloom (M211e; faf-re CBloomRenderer::DoBloom), out of the renderer
// (roadmap item 10's decomposition): the glow copied out of the frame at
// half size, blurred, and added back as the frame goes to the screen.

#include "core/types.hpp"
#include "renderer/vk_types.hpp"

#include <vulkan/vulkan.h>

namespace osc::renderer {

class BloomRenderer {
public:
    /// The half-size targets, their pass and framebuffers, and the sets the
    /// passes read: the scene (`scene_view`, w x h) and each target, through
    /// `sampler`, as set 0 of `texture_layout`.
    void create(VkDevice device, VmaAllocator allocator, u32 width, u32 height,
                VkImageView scene_view, VkDescriptorSetLayout texture_layout, VkSampler sampler);
    /// The copy and blur pipelines (its pass), and the composite (on
    /// `present_pass`, the frame's).
    void create_pipelines(VkRenderPass present_pass, VkDescriptorSetLayout texture_layout);
    /// Everything (targets and pipelines both: the targets follow the
    /// window's size, and are made again with it).
    void destroy();

    /// Its pipelines made.
    bool ready() const { return bright_pipeline_ && composite_pipeline_; }

    /// CBloomRenderer::DoBloom: the glow copied out of the scene (scaled by
    /// `copy_scale`, `add` added: the map's Bloom), then blurred `blur_count`
    /// times across and down by `kernel_scale`.
    void record(VkCommandBuffer cmd, f32 copy_scale, f32 add, f32 kernel_scale,
                int blur_count) const;
    /// The scene onto the frame, with the bloom added when `with_bloom`, in
    /// the frame's pass (its viewport set). Nothing without its pipeline.
    void composite(VkCommandBuffer cmd, bool with_bloom) const;

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VmaAllocator allocator_ = VK_NULL_HANDLE;
    u32 half_w_ = 1;
    u32 half_h_ = 1;
    AllocatedImage bright_image_{};
    AllocatedImage blur_h_image_{};
    AllocatedImage blur_v_image_{};
    VkRenderPass pass_ = VK_NULL_HANDLE; ///< one colour target, no depth
    VkFramebuffer bright_fb_ = VK_NULL_HANDLE;
    VkFramebuffer blur_h_fb_ = VK_NULL_HANDLE;
    VkFramebuffer blur_v_fb_ = VK_NULL_HANDLE;
    VkPipeline bright_pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout bright_layout_ = VK_NULL_HANDLE;
    VkPipeline blur_pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout blur_layout_ = VK_NULL_HANDLE;
    VkPipeline composite_pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout composite_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool ds_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet scene_ds_ = VK_NULL_HANDLE;  ///< the scene's colour
    VkDescriptorSet bright_ds_ = VK_NULL_HANDLE; ///< the copied glow
    VkDescriptorSet blur_h_ds_ = VK_NULL_HANDLE; ///< blurred across
    VkDescriptorSet blur_v_ds_ = VK_NULL_HANDLE; ///< and down
};

} // namespace osc::renderer
