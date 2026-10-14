#pragma once

// The cartographic frame (Moho's Cartographic::Render, faf-re
// render/Cartographic.cpp; retail's cartographic.fx): the map as the
// minimap draws it, a flat hypsometric terrain with contour lines.
// - Stage 0 (Terrain_Stage0): the ground quad at the water's surface, its
//   colour the ramp's at each point's elevation, its alpha the contour
//   band's shade; drawn into the frame target.
// - Stage 1 (Terrain_Stage1): the frame copied to the output with each
//   pixel darkened by how far its band's shade stands over its eight
//   neighbours': the contour lines.
// Both targets are 8-bit, as Moho's are, and one pair a frame in flight.

#include "core/types.hpp"
#include "renderer/cartographic_math.hpp"
#include "renderer/vk_types.hpp"

#include <vulkan/vulkan.h>

#include <array>

namespace osc::map {
class Terrain;
}

namespace osc::renderer {

class TextureCache;

class CartographicRenderer {
public:
    static constexpr u32 FRAMES_IN_FLIGHT = 2;
    static constexpr VkFormat kTargetFormat = VK_FORMAT_R8G8B8A8_UNORM;

    /// A frame's view: its target's size and the world rect that fills it
    /// (z0 at the top; beyond the map it stays the clear's black).
    struct View {
        u32 width = 0, height = 0;
        f32 x0 = 0, z0 = 0, x1 = 0, z1 = 0;
    };

    /// Samplers, layouts, passes and pipelines, once.
    void init(VkDevice device, VmaAllocator allocator, VkFormat depth_format);

    /// The map's lookup data (Cartographic::InitializeTerrain), made once
    /// from its heights as they are now. Waits for the device.
    void build(const map::Terrain& terrain, TextureCache& textures);

    /// Both stages into frame `fi`'s targets, outside any render pass; the
    /// output ends shader-readable. Nothing without a map built. A new size
    /// waits for the device and makes the targets again, so record before
    /// anything in the command buffer uses them.
    void record(VkCommandBuffer cmd, u32 fi, const View& view);

    bool ready() const { return built_ && terrain_pipeline_ && edge_pipeline_; }
    const CartographicParams& params() const { return params_; }
    /// Frame `fi`'s targets, as the last record left them.
    const AllocatedImage& frame(u32 fi) const { return targets_[fi].frame; }
    const AllocatedImage& output(u32 fi) const { return targets_[fi].output; }
    u32 width(u32 fi) const { return targets_[fi].width; }
    u32 height(u32 fi) const { return targets_[fi].height; }

    /// Forget the map (a new map), keeping the pipelines. The device must be
    /// idle.
    void clear(TextureCache& textures);
    void destroy();

private:
    struct Target {
        u32 width = 0, height = 0;
        AllocatedImage frame{};
        AllocatedImage depth{};
        AllocatedImage output{};
        VkFramebuffer frame_fb = VK_NULL_HANDLE;
        VkFramebuffer output_fb = VK_NULL_HANDLE;
    };

    /// Frame `fi`'s targets at a size, made again when it changes.
    bool ensure_target(u32 fi, u32 width, u32 height);
    void destroy_target(Target& t);

    VkDevice device_ = VK_NULL_HANDLE;
    VmaAllocator allocator_ = VK_NULL_HANDLE;
    VkFormat depth_format_ = VK_FORMAT_D32_SFLOAT;
    VkSampler point_ = VK_NULL_HANDLE;
    VkRenderPass frame_pass_ = VK_NULL_HANDLE;
    VkRenderPass output_pass_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout lookup_layout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout frame_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet lookup_set_ = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, FRAMES_IN_FLIGHT> frame_sets_{};
    VkPipelineLayout terrain_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout edge_layout_ = VK_NULL_HANDLE;
    VkPipeline terrain_pipeline_ = VK_NULL_HANDLE;
    VkPipeline edge_pipeline_ = VK_NULL_HANDLE;
    std::array<Target, FRAMES_IN_FLIGHT> targets_{};

    bool built_ = false;
    CartographicParams params_{};
    i16 surface_word_ = 0;
    u32 span_x_ = 0, span_z_ = 0;
};

} // namespace osc::renderer
