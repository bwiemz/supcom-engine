#pragma once

// The frame's own targets, out of the renderer (roadmap item 10's
// decomposition): the HDR scene it draws into, the copies the water refracts
// (M213a) and reflects (M213b), the normal target (M212e), the scene's passes
// and their framebuffers. All follow the window's size, and are made again
// with it.

#include "core/types.hpp"
#include "renderer/vk_types.hpp"

#include <vulkan/vulkan.h>

namespace osc::renderer {

class FrameTargets {
public:
    /// The targets at `width` x `height`, their passes, and the framebuffers
    /// that draw them on the frame's depth buffer (`depth_view`, of
    /// `depth_format`).
    void create(VkDevice device, VmaAllocator allocator, u32 width, u32 height,
                VkFormat depth_format, VkImageView depth_view);
    void destroy();

    /// The scene, HDR (overbright for the bloom), which every scene
    /// pipeline draws into.
    const AllocatedImage& scene_color() const { return scene_color_image_; }
    /// The frame before the water, which the water refracts (M213a).
    const AllocatedImage& refraction() const { return refraction_image_; }
    /// The units reflected in the water, drawn mirrored before the scene
    /// (M213b).
    const AllocatedImage& reflection() const { return reflection_image_; }
    /// The normal pass's target (M212e), which the scene reads.
    const AllocatedImage& terrain_normal() const { return terrain_normal_image_; }

    /// The scene's pass, and its two passes around the water on a map with
    /// it (M213a), with one between them that goes on from the first and
    /// ends as it does (M214d). Their dependencies match, so one set of
    /// pipelines draws in all four.
    VkRenderPass scene_pass() const { return scene_render_pass_; }
    VkRenderPass first_pass() const { return scene_first_pass_; }
    VkRenderPass second_pass() const { return scene_second_pass_; }
    VkRenderPass middle_pass() const { return scene_middle_pass_; }

    /// The scene's framebuffer, the reflection's and the normal pass's, all
    /// on the frame's depth.
    VkFramebuffer scene_framebuffer() const { return scene_framebuffer_; }
    VkFramebuffer reflection_framebuffer() const { return reflection_framebuffer_; }
    VkFramebuffer terrain_normal_framebuffer() const { return terrain_normal_framebuffer_; }

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VmaAllocator allocator_ = VK_NULL_HANDLE;
    AllocatedImage scene_color_image_{};
    AllocatedImage refraction_image_{};
    AllocatedImage reflection_image_{};
    AllocatedImage terrain_normal_image_{};
    VkRenderPass scene_render_pass_ = VK_NULL_HANDLE;
    VkRenderPass scene_first_pass_ = VK_NULL_HANDLE;
    VkRenderPass scene_second_pass_ = VK_NULL_HANDLE;
    VkRenderPass scene_middle_pass_ = VK_NULL_HANDLE;
    VkFramebuffer scene_framebuffer_ = VK_NULL_HANDLE;
    VkFramebuffer reflection_framebuffer_ = VK_NULL_HANDLE;
    VkFramebuffer terrain_normal_framebuffer_ = VK_NULL_HANDLE;
};

} // namespace osc::renderer
