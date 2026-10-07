#pragma once

// Moho's shadow map (M210c), its own targets and blur, out of the renderer
// (roadmap item 10's decomposition). The renderer keeps what draws into it,
// the casters' pipelines, and the lit shaders' lighting that reads it.

#include "core/types.hpp"
#include "renderer/vk_types.hpp"

#include <vulkan/vulkan.h>

namespace osc::renderer {

/// The map: a colour target (R the caster's depth along the light, G 0 where
/// a mesh is the surface nearest the sun), its depth buffer, and the blur's
/// two targets, B of the second being the terrain's mask. FA's map is G16R16
/// (A8R8G8B8 at shadow fidelity 1, where nothing reads R).
class ShadowMap {
public:
    static constexpr VkFormat kFormat = VK_FORMAT_R16G16_UNORM;

    /// The targets, passes, framebuffers and samplers, and the blur's
    /// pipelines and sources (set 0 of `texture_layout`).
    void create(VkDevice device, VmaAllocator allocator, u32 size,
                VkDescriptorSetLayout texture_layout);
    void destroy();

    u32 size() const { return size_; }
    /// The casters' pass and its framebuffer (colour and depth, cleared to
    /// 1: lit, and far).
    VkRenderPass map_pass() const { return map_pass_; }
    bool ready() const { return map_pass_ && map_fb_; }
    /// What the scene reads: the map, and the terrain's mask (blur B).
    VkImageView map_view() const { return map_.view; }
    VkImageView mask_view() const { return blur_b_.view; }
    /// Point and clamped (FA's shadowSampler, the meshes' one tap);
    /// bilinear with a white border (FA's shadowPCFSampler, and the
    /// terrain's mask's ShadowSampler).
    VkSampler point_sampler() const { return point_sampler_; }
    VkSampler linear_sampler() const { return linear_sampler_; }

    /// Begin the casters' pass: cleared, drawn inside a one-pixel border
    /// that keeps the clear (a lookup off the volume reads lit,
    /// Shadow::RenderShadowMap's viewport).
    void begin_map(VkCommandBuffer cmd) const;
    /// The terrain's mask, B: Moho's blur, once (across into A, down into
    /// B), or with ren_ShadowBlur off a copy of the map's G.
    void record_mask(VkCommandBuffer cmd, bool blur) const;

private:
    void begin(VkCommandBuffer cmd, VkRenderPass pass, VkFramebuffer fb, u32 clear_count) const;

    VkDevice device_ = VK_NULL_HANDLE;
    VmaAllocator allocator_ = VK_NULL_HANDLE;
    u32 size_ = 0;
    AllocatedImage map_{};
    AllocatedImage depth_{};
    AllocatedImage blur_a_{};
    AllocatedImage blur_b_{};
    VkSampler point_sampler_ = VK_NULL_HANDLE;
    VkSampler linear_sampler_ = VK_NULL_HANDLE;
    VkRenderPass map_pass_ = VK_NULL_HANDLE;
    VkRenderPass blur_pass_ = VK_NULL_HANDLE;
    VkFramebuffer map_fb_ = VK_NULL_HANDLE;
    VkFramebuffer blur_a_fb_ = VK_NULL_HANDLE;
    VkFramebuffer blur_b_fb_ = VK_NULL_HANDLE;
    VkPipeline blur_h_pipeline_ = VK_NULL_HANDLE;
    VkPipeline blur_v_pipeline_ = VK_NULL_HANDLE;
    VkPipeline copy_pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout blur_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool blur_ds_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet blur_h_ds_ = VK_NULL_HANDLE; ///< the map, point
    VkDescriptorSet blur_v_ds_ = VK_NULL_HANDLE; ///< target A, bilinear
};

} // namespace osc::renderer
