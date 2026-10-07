#pragma once

// What draws into Moho's shadow map (M210c), out of the renderer (roadmap
// item 10's decomposition): the terrain, the meshes and the placeholder
// cubes, each with its depth pipeline. The renderer keeps the light camera
// and the light UBO; ShadowMap keeps the map and its blur.

#include "core/types.hpp"

#include <vulkan/vulkan.h>

#include <array>

namespace osc::renderer {

class TerrainMesh;
class UnitRenderer;

class ShadowCasters {
public:
    /// The three pipelines, drawing in the map's pass (`map_pass`): the
    /// meshes read their bones (set 0 of `bone_layout`) and their albedo
    /// (set 1 of `texture_layout`).
    void create(VkDevice device, VkRenderPass map_pass, VkDescriptorSetLayout bone_layout,
                VkDescriptorSetLayout texture_layout);
    void destroy();

    /// What one frame's map draws.
    struct Frame {
        const TerrainMesh* terrain = nullptr;
        const UnitRenderer* units = nullptr;
        VkDescriptorSet bones = VK_NULL_HANDLE;           ///< the frame's bone SSBO
        VkDescriptorSet albedo_fallback = VK_NULL_HANDLE; ///< for a mesh without one
        u32 lane = 0;                                     ///< the lane by graphics fidelity (M211n)
        bool strategic = false;                           ///< icons replace the meshes and cubes
    };
    /// The terrain (R its depth over 128, G 1), then the meshes and the
    /// cubes (R their depth, G 0), seen by `light_vp`, in the map's pass
    /// begun.
    void record(VkCommandBuffer cmd, const std::array<f32, 16>& light_vp, const Frame& frame) const;

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkPipeline terrain_pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout terrain_layout_ = VK_NULL_HANDLE;
    VkPipeline mesh_pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout mesh_layout_ = VK_NULL_HANDLE;
    VkPipeline cube_pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout cube_layout_ = VK_NULL_HANDLE;
};

} // namespace osc::renderer
