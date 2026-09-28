#pragma once

#include "core/types.hpp"
#include "renderer/decal_math.hpp"
#include "renderer/runtime_decals.hpp"
#include "renderer/vk_types.hpp"

#include <vulkan/vulkan.h>

#include <array>
#include <vector>

namespace osc::map {
class Terrain;
}
namespace osc::sim {
struct WorldSnapshot;
}

namespace osc::renderer {

class Camera;
class Frustum;
class TerrainMesh;
class TextureCache;

/// Draws the runtime decals and splats (M212c) as HighFidelityTerrain does:
/// the decals in the map's decal passes (TDecals, TDecalsXP) over the
/// terrain's own triangles, and the splats after them (TSplats), each a
/// quad on its four corners.
class RuntimeDecalRenderer {
public:
    /// The splat pipeline: set 0 the terrain's textures, set 1 the shadow
    /// and light, set 2 the splat's albedo; the decals' push block.
    void init(VkDevice device, VmaAllocator allocator, VkRenderPass render_pass,
              VkDescriptorSetLayout terrain_layout, VkDescriptorSetLayout shadow_layout,
              VkDescriptorSetLayout texture_layout);

    /// This frame: `snap`'s tick for `focus_army` (-1: an observer), the
    /// decals' triangles (gathered again as they come and go) and fades,
    /// and the splats' quads. Their textures load as they are first drawn,
    /// as Moho loads them. `view` and `eye` are the camera's.
    void update(const sim::WorldSnapshot* snap, i32 focus_army, const map::Terrain& terrain,
                const TerrainMesh& mesh, const std::array<f32, 16>& view,
                const std::array<f32, 3>& eye, f32 aspect, const Frustum& frustum,
                TextureCache& textures, u32 fi);

    /// A runtime decal this frame draws, over the terrain's vertices with
    /// index_buffer(), in its technique's pass.
    struct DecalDraw {
        const RuntimeDecals::Decal* decal = nullptr;
        DecalTechnique technique = DecalTechnique::Albedo;
        f32 u[4] = {};
        f32 v[4] = {};
        f32 alpha = 1; ///< its LOD fade times its own
        u32 first_index = 0, index_count = 0;
    };
    const std::vector<DecalDraw>& decal_draws() const { return decal_draws_; }
    VkBuffer index_buffer(u32 fi) const { return index_buf_[fi].buffer; }

    /// Draw the splats (the scene pass open, after the decals), lit over the
    /// terrain's sets.
    void draw_splats(VkCommandBuffer cmd, u32 fi, const std::array<f32, 16>& view_proj,
                     const std::array<f32, 3>& eye, f32 map_width, f32 map_height,
                     VkDescriptorSet terrain_set, VkDescriptorSet shadow_set) const;

    /// Forget them all (a new scene).
    void clear();
    void destroy(VkDevice device, VmaAllocator allocator);

    const RuntimeDecals& decals() const { return decals_; }
    /// The splats this frame draws.
    u32 splat_count() const { return splat_count_; }

    /// HighFidelityTerrain's kMaxSplatsPerFrame.
    static constexpr u32 kMaxSplats = 2500;
    static constexpr u32 FRAMES_IN_FLIGHT = 2;

private:
    /// A decal's triangles and matrix, gathered when the decals change.
    struct Gathered {
        f32 u[4] = {};
        f32 v[4] = {};
        f32 mid_x = 0, mid_z = 0;
        f32 radius = 0;
        u32 first_index = 0, index_count = 0;
    };
    /// A run of splats with one texture.
    struct Run {
        VkDescriptorSet texture = VK_NULL_HANDLE;
        u32 first_vertex = 0, vertex_count = 0;
    };

    void gather(const TerrainMesh& mesh, TextureCache& textures);
    void upload_indices(u32 fi);
    void build_splats(const map::Terrain& terrain, const std::array<f32, 16>& view,
                      const std::array<f32, 3>& eye, f32 aspect, const Frustum& frustum,
                      TextureCache& textures, u32 fi);

    VkDevice device_ = VK_NULL_HANDLE;
    VmaAllocator allocator_ = nullptr;
    VkPipeline splat_pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout splat_layout_ = VK_NULL_HANDLE;

    RuntimeDecals decals_;
    std::vector<Gathered> gathered_; ///< per decals_.decals()
    std::vector<u32> indices_;
    u32 gathered_generation_ = ~0u;
    std::vector<DecalDraw> decal_draws_;

    AllocatedBuffer index_buf_[FRAMES_IN_FLIGHT] = {};
    void* index_mapped_[FRAMES_IN_FLIGHT] = {};
    u32 index_capacity_[FRAMES_IN_FLIGHT] = {};
    u32 index_generation_[FRAMES_IN_FLIGHT] = {~0u, ~0u};

    AllocatedBuffer splat_buf_[FRAMES_IN_FLIGHT] = {};
    void* splat_mapped_[FRAMES_IN_FLIGHT] = {};
    std::vector<Run> runs_;
    u32 splat_count_ = 0;
};

} // namespace osc::renderer
