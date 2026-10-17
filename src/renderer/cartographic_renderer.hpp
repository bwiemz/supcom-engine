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
//
// A cartographic main view (SetCartographic, its camera orthographic) has a
// frame of its own at the window's size, seen through the camera; its
// Stage 1 draws straight into the scene, as Moho's draws into the head.

#include "core/types.hpp"
#include "renderer/cartographic_math.hpp"
#include "renderer/vk_types.hpp"

#include <vulkan/vulkan.h>

#include <array>
#include <functional>

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

    /// Samplers, layouts, passes and pipelines, once. `ui_layout` is the
    /// UI's texture set layout (one combined sampler), which ui_descriptor's
    /// sets take; `scene_pass` the scene's, which the main view's Stage 1
    /// draws in.
    void init(VkDevice device, VmaAllocator allocator, VkFormat depth_format,
              VkDescriptorSetLayout ui_layout, VkRenderPass scene_pass);

    /// The map's lookup data (Cartographic::InitializeTerrain), made once
    /// from its heights as they are now. Waits for the device.
    void build(const map::Terrain& terrain, TextureCache& textures);

    /// Frame `fi`'s targets at `width` x `height`. A new size waits for the
    /// device and makes them again, so call it before anything this frame
    /// refers to them (ui_descriptor), then record() at that size. False with
    /// no map built, or if they couldn't be made.
    bool prepare(u32 fi, u32 width, u32 height);

    /// What is drawn into the frame over Stage 0, before Stage 1 finds the
    /// contour lines (Moho's range rings, fog and boundary): given the
    /// command buffer inside the frame's pass, its view-projection and size.
    using Overlays =
        std::function<void(VkCommandBuffer cmd, const f32* view_proj, u32 width, u32 height)>;

    /// What is drawn into the output after Stage 1 (Moho's RenderMeshes:
    /// the minimap's meshes), through mesh_view_proj, over a depth cleared
    /// to 1: given the command buffer inside the output's pass.
    using Meshes = std::function<void(VkCommandBuffer cmd, const f32* view_proj)>;

    /// Both stages into frame `fi`'s targets, outside any render pass; the
    /// output ends shader-readable. Nothing without a map built. Prepares
    /// the targets at the view's size first (see prepare()).
    void record(VkCommandBuffer cmd, u32 fi, const View& view, const Overlays& overlays = {},
                const Meshes& meshes = {});

    /// The main view's Stage 0, and its overlays, into its frame for `fi`
    /// at `width` x `height` (the scene's), through `view_proj` (the
    /// camera's), outside any render pass; the frame ends shader-readable.
    /// A new size waits for the device. False, and nothing drawn, without a
    /// map built or if the frame couldn't be made.
    bool record_main_frame(VkCommandBuffer cmd, u32 fi, u32 width, u32 height, const f32* view_proj,
                           const Overlays& overlays = {});
    /// The main view's Stage 1 over the whole of the open scene pass (which
    /// is the frame's size): the frame less its contour lines, alpha 0 (no
    /// glow). After record_main_frame drew it this frame.
    void record_main_edge(VkCommandBuffer cmd, u32 fi) const;

    /// The frame's view-projection for `view` (record()'s): top down over
    /// its world rect, its depth over the heights, the quad and the
    /// overlays' volumes.
    std::array<f32, 16> view_proj(const View& view) const;
    /// The meshes' (Meshes): view_proj's, its top raised by kMeshHeadroom,
    /// so what flies over the highest ground is not clipped away.
    std::array<f32, 16> mesh_view_proj(const View& view) const;
    static constexpr f32 kMeshHeadroom = 1000.0f;
    /// The output's pass (Stage 1, then the meshes, over a depth with a
    /// stencil where the format has one), which the meshes' pipelines are
    /// made for.
    VkRenderPass output_pass() const { return output_pass_; }

    /// The frame's pass, which the overlays' pipelines are made for (its
    /// depth has a stencil where the format does).
    VkRenderPass frame_pass() const { return frame_pass_; }
    /// The heights the overlays' volumes span (the range rings' and fog's:
    /// from 5 below the map's lowest point to its highest), which the
    /// frame's depth range takes in so none is clipped.
    void set_volume_heights(f32 bottom, f32 top) {
        volume_bottom_ = bottom;
        volume_top_ = top;
    }

    /// Frame `fi`'s output for the UI to draw: the point sampler, so a frame
    /// drawn at its own size is copied pixel for pixel, and alpha read as 1.
    /// Stage 1 writes alpha 0 (Moho draws the frame straight into the view;
    /// the UI blends). Valid from prepare() until the size changes.
    VkDescriptorSet ui_descriptor(u32 fi) const {
        return fi < FRAMES_IN_FLIGHT ? ui_sets_[fi] : VK_NULL_HANDLE;
    }

    bool ready() const { return built_ && terrain_pipeline_ && edge_pipeline_; }
    /// The map's height ramp (the texture cache's set for it), which a
    /// cartographic view's features take their colour from; null until
    /// built.
    VkDescriptorSet hypsometric_descriptor() const {
        return built_ ? hypsometric_ds_ : VK_NULL_HANDLE;
    }
    const CartographicParams& params() const { return params_; }
    /// Frame `fi`'s targets, as the last record left them.
    const AllocatedImage& frame(u32 fi) const { return targets_[fi].frame; }
    const AllocatedImage& output(u32 fi) const { return targets_[fi].output; }
    /// The main view's frame for `fi`, as record_main_frame left it.
    const AllocatedImage& main_frame(u32 fi) const { return main_targets_[fi].frame; }
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
        VkImageView ui_view = VK_NULL_HANDLE; ///< the output, its alpha 1
        VkFramebuffer frame_fb = VK_NULL_HANDLE;
        VkFramebuffer output_fb = VK_NULL_HANDLE;
    };

    /// A target at a size, made again when it changes: with an output for
    /// Stage 1 and the UI's view of it (the minimap's), or the frame alone
    /// (the main view's). `frame_set` takes the frame, `ui_set` the output.
    bool ensure_target(Target& t, u32 width, u32 height, bool with_output,
                       VkDescriptorSet frame_set, VkDescriptorSet ui_set);
    /// Stage 0 into `t`'s frame through `view_proj`, then the overlays.
    void record_stage0(VkCommandBuffer cmd, const Target& t, const f32* view_proj,
                       const Overlays& overlays) const;
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
    std::array<VkDescriptorSet, FRAMES_IN_FLIGHT> ui_sets_{};
    VkPipelineLayout terrain_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout edge_layout_ = VK_NULL_HANDLE;
    VkPipeline terrain_pipeline_ = VK_NULL_HANDLE;
    VkPipeline edge_pipeline_ = VK_NULL_HANDLE;
    VkPipeline scene_edge_pipeline_ = VK_NULL_HANDLE; ///< Stage 1 in the scene
    std::array<Target, FRAMES_IN_FLIGHT> targets_{};
    std::array<Target, FRAMES_IN_FLIGHT> main_targets_{};
    std::array<VkDescriptorSet, FRAMES_IN_FLIGHT> main_frame_sets_{};

    bool built_ = false;
    VkDescriptorSet hypsometric_ds_ = VK_NULL_HANDLE;
    CartographicParams params_{};
    i16 surface_word_ = 0;
    u32 span_x_ = 0, span_z_ = 0;
    f32 volume_bottom_ = 0.0f, volume_top_ = 0.0f;
};

} // namespace osc::renderer
