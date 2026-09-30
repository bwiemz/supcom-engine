#pragma once

#include "core/types.hpp"
#include "renderer/vk_types.hpp"

#include <vulkan/vulkan.h>

#include <array>
#include <vector>

namespace osc::map {
class Terrain;
}

namespace osc::renderer {

class Camera;
class TextureCache;

/// FA's water (M213a): water2.fx's Water_HighFidelity over one quad that
/// covers the map at the water's elevation, as Moho's HighFidelityWater draws
/// it. Its normal is four scrolling wave layers; it refracts the frame drawn
/// so far and reflects the sky, by the map's water parameters, a Fresnel
/// table and the water map (flatness, depth, land, foam). Before the frame
/// is copied for it, its alpha mask (TWaterLayAlphaMask) marks open water.
class WaterRenderer {
public:
    /// Pipelines, samplers and uniform buffers, once, for the scene pass.
    void init(VkDevice device, VmaAllocator allocator, VkRenderPass scene_pass);

    /// The map's water: its quad, water map, Fresnel table, wave textures
    /// and cubemap. Nothing on a map without water.
    void build(const map::Terrain& terrain, TextureCache& textures);

    /// The frame's images the water samples, made again after a resize
    struct FrameImages {
        VkImageView refraction = VK_NULL_HANDLE; ///< the copy of the frame it refracts
        VkImageView reflection = VK_NULL_HANDLE; ///< the units' reflection (M213b)
    };
    /// Both at once: each write of the water's descriptors reads both.
    void set_frame_images(const FrameImages& images);

    /// This frame's parameters: the camera, and FA's time (ticks, with the
    /// frame's interpolant).
    void update(const Camera& camera, const std::array<f32, 16>& view_proj, f32 time, u32 fi);

    /// The alpha mask: alpha 0 over open water (the first scene pass).
    void render_mask(VkCommandBuffer cmd, u32 viewport_w, u32 viewport_h, u32 fi) const;
    /// The surface (the second scene pass, after the copy).
    void render_surface(VkCommandBuffer cmd, u32 viewport_w, u32 viewport_h, u32 fi) const;

    /// Forget the map's water (a new map), keeping the pipelines.
    void clear();
    void destroy(VkDevice device, VmaAllocator allocator);

    bool has_water() const { return has_water_; }
    f32 water_elevation() const { return water_elevation_; }

    /// The water map, RGBA a texel, half the map's size (tests read it): R
    /// flatness, G depth, B land, A foam.
    const std::vector<u8>& water_map() const { return water_map_; }
    u32 water_map_width() const { return water_map_w_; }
    u32 water_map_height() const { return water_map_h_; }
    /// The Fresnel table, RGBA, FRESNEL_SIZE square: row the incidence,
    /// column the depth; R the Fresnel term.
    const std::vector<u8>& fresnel_table() const { return fresnel_; }

    /// What the terrain tints itself by under the water (terrain.fx's
    /// ApplyWaterColor): the water map (its depth, G) and the map's water
    /// ramp, with a clamping sampler for both.
    VkImageView water_map_view() const { return water_map_view_; }
    VkImageView ramp_view() const { return ramp_view_; }
    VkSampler clamp_sampler() const { return clamp_; }

    static constexpr u32 FRESNEL_SIZE = 128;
    static constexpr u32 FRAMES_IN_FLIGHT = 2;

private:
    /// The shader's parameters (std140).
    struct Uniforms {
        f32 view_proj[16];
        f32 view_pos[4];    ///< xyz the eye, w the water's elevation
        f32 params[4];      ///< time, refraction scale, unit reflection, sky reflection
        f32 water_color[4]; ///< rgb, a sun shininess
        f32 lerp[4];        ///< the colour lerp's range
        f32 repeat[4];      ///< the four layers' repeat rates
        f32 move01[4];      ///< layers 0 and 1's movement
        f32 move23[4];      ///< layers 2 and 3's
        f32 sun_dir[4];
        f32 sun_color[4]; ///< times its reflection amount, as Moho sets it
    };
    struct Vertex {
        f32 pos[3];
        f32 uv[2];
    };

    void write_sets();

    VkDevice device_ = VK_NULL_HANDLE;
    VmaAllocator allocator_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline mask_pipeline_ = VK_NULL_HANDLE;
    VkPipeline surface_pipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, FRAMES_IN_FLIGHT> sets_{};
    VkSampler wrap_ = VK_NULL_HANDLE, clamp_ = VK_NULL_HANDLE, point_ = VK_NULL_HANDLE;
    AllocatedBuffer uniform_buf_[FRAMES_IN_FLIGHT] = {};
    void* uniform_mapped_[FRAMES_IN_FLIGHT] = {};
    AllocatedBuffer vertex_buf_{};
    AllocatedBuffer index_buf_{};
    void* vertex_mapped_ = nullptr;
    void* index_mapped_ = nullptr;

    // The map's water
    bool has_water_ = false;
    f32 water_elevation_ = 0;
    Uniforms params_{};
    std::array<VkImageView, 4> normals_{};
    VkImageView cube_ = VK_NULL_HANDLE;
    VkImageView water_map_view_ = VK_NULL_HANDLE;
    VkImageView fresnel_view_ = VK_NULL_HANDLE;
    VkImageView reflection_view_ = VK_NULL_HANDLE;
    VkImageView ramp_view_ = VK_NULL_HANDLE;
    VkImageView refraction_view_ = VK_NULL_HANDLE;
    std::vector<u8> water_map_;
    u32 water_map_w_ = 0, water_map_h_ = 0;
    std::vector<u8> fresnel_;
};

} // namespace osc::renderer
