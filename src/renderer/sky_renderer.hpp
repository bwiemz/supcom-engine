#pragma once

#include "core/types.hpp"
#include "renderer/vk_types.hpp"

#include <vulkan/vulkan.h>

#include <array>

namespace osc::map {
class Terrain;
}

namespace osc::renderer {

class Camera;
class TextureCache;

/// FA's sky (M210b): the map's SkyDome, as WRenViewport::RenderSkyDome
/// draws it first in each world view, with sky.fx's techniques. The steps are:
/// - Atmosphere: the dome, the horizon's colour graded into the sky's;
/// - Decal: the sky's billboards, their albedo, then their glow;
/// - Cirrus: the dome again, four drifting cloud layers.
///
/// Moho's Cumulus pass gets no clouds, so it is left out. Nothing is
/// depth-tested; clockwise triangles are culled (CullMode = CW), so the dome
/// shows from inside it.
class SkyRenderer {
public:
    /// Pipelines, samplers and uniform buffers, once, for the scene pass.
    void init(VkDevice device, VmaAllocator allocator, VkRenderPass scene_pass);

    /// The map's sky: its dome, decals and textures. Waits for the device
    /// first, since it replaces buffers a frame may still read.
    void build(const map::Terrain& terrain, TextureCache& textures);

    /// This frame's camera and time: the tick, and the frame's interpolant
    /// toward the next.
    void update(const Camera& camera, const std::array<f32, 16>& view_proj, u32 tick,
                f32 interpolant, u32 fi);

    /// The passes, first in the scene pass (the viewport and scissor set).
    void record(VkCommandBuffer cmd, u32 fi) const;

    /// Forget the map's sky (a new map), keeping the pipelines.
    void clear();
    void destroy(VkDevice device, VmaAllocator allocator);

    bool has_sky() const { return index_count_ > 0; }
    /// The decals drawn: none without both of their textures (RenderDecals).
    u32 decal_count() const { return decal_count_; }

    static constexpr u32 FRAMES_IN_FLIGHT = 2;
    /// Moho's decal buffer holds 1024.
    static constexpr u32 MAX_DECALS = 1024;

private:
    /// sky.fx's parameters (std140).
    struct Uniforms {
        f32 view_proj[16];
        f32 view_right[4];   ///< w the time, in ticks
        f32 view_up[4];      ///< w the decals' glow multiplier
        f32 horizon[4];      ///< its start and end; z the cirrus multiplier
        f32 horizon_color[4];
        f32 sky_color[4];
        f32 cirrus_color[4];
        f32 cirrus_layer[4][4]; ///< frequency xy, direction zw
        f32 cirrus_speed[4];
    };
    /// A decal's instance: sky.fx's DecalVS inputs, 40 bytes as Moho's.
    struct DecalInstance {
        f32 position[4]; ///< w its rotation
        f32 size[2];
        f32 uv[4];
    };

    void write_sets();
    void free_buffers();

    VkDevice device_ = VK_NULL_HANDLE;
    VmaAllocator allocator_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline atmosphere_pipeline_ = VK_NULL_HANDLE;
    VkPipeline cirrus_pipeline_ = VK_NULL_HANDLE;
    VkPipeline decal_albedo_pipeline_ = VK_NULL_HANDLE;
    VkPipeline decal_glow_pipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, FRAMES_IN_FLIGHT> sets_{};
    VkSampler point_ = VK_NULL_HANDLE, wrap_ = VK_NULL_HANDLE, clamp_ = VK_NULL_HANDLE;
    AllocatedBuffer uniform_buf_[FRAMES_IN_FLIGHT] = {};
    void* uniform_mapped_[FRAMES_IN_FLIGHT] = {};
    AllocatedBuffer quad_buf_{};       ///< the billboard's corners, then its six indices
    AllocatedBuffer dome_vertices_{};
    AllocatedBuffer dome_indices_{};
    AllocatedBuffer decal_buf_{};

    // The map's sky
    u32 index_count_ = 0;
    u32 decal_count_ = 0;
    Uniforms params_{};
    VkImageView horizon_lookup_ = VK_NULL_HANDLE;
    VkImageView cirrus_ = VK_NULL_HANDLE;
    VkImageView decal_albedo_ = VK_NULL_HANDLE;
    VkImageView decal_glow_ = VK_NULL_HANDLE;
};

} // namespace osc::renderer
