#pragma once

#include "renderer/vk_types.hpp"
#include "renderer/mesh_cache.hpp"
#include "core/types.hpp"

#include <string>
#include <unordered_map>
#include <vector>

struct lua_State;

namespace osc::sim {
class SimState;
}

namespace osc::renderer {

class TextureCache; // forward

/// Per-instance data for cube fallback (old format).
struct CubeInstance {
    f32 x, y, z;     // world position
    f32 scale;        // footprint-based scale
    f32 r, g, b, a;  // army color + alpha
};

/// Per-instance data for real mesh rendering.
struct MeshInstance {
    f32 model[16];    // column-major 4x4 model matrix
    f32 r, g, b, a;  // army color + alpha
};

/// A group of instances sharing the same GPU mesh.
struct MeshDrawGroup {
    const GPUMesh* mesh = nullptr;
    u32 instance_offset = 0;  // offset into shared instance buffer
    u32 instance_count = 0;
    VkDescriptorSet texture_ds = VK_NULL_HANDLE; // albedo texture descriptor
    u32 bone_base_offset = 0; // index into bone SSBO (in mat4 units)
    u32 bones_per_instance = 0; // 0 = no skinning, else bone count
};

/// Renders units as real SCM meshes where available, with cube fallback.
class UnitRenderer {
public:
    /// Upload static cube mesh to GPU.
    void build(VkDevice device, VmaAllocator allocator,
               VkCommandPool cmd_pool, VkQueue queue);

    /// Pre-load GPU meshes for all blueprints currently in the entity registry.
    void preload_meshes(const sim::SimState& sim, MeshCache& mesh_cache,
                        lua_State* L);

    /// Update per-frame instance data from sim state.
    void update(const sim::SimState& sim, MeshCache& mesh_cache,
                lua_State* L, TextureCache* tex_cache = nullptr);

    void destroy(VkDevice device, VmaAllocator allocator);

    // --- Cube fallback accessors ---
    VkBuffer cube_vertex_buffer() const { return cube_verts_.buffer; }
    VkBuffer cube_index_buffer() const { return cube_indices_.buffer; }
    VkBuffer cube_instance_buffer() const { return cube_instance_buf_.buffer; }
    u32 cube_index_count() const { return 36; }
    u32 cube_instance_count() const { return cube_instance_count_; }

    // --- Mesh draw groups ---
    const std::vector<MeshDrawGroup>& mesh_groups() const {
        return mesh_groups_;
    }
    VkBuffer mesh_instance_buffer() const { return mesh_instance_buf_.buffer; }
    VkBuffer bone_ssbo_buffer() const { return bone_ssbo_.buffer; }

    static constexpr u32 MAX_INSTANCES = 2048;
    static constexpr u32 MAX_BONES_PER_UNIT = 64;

private:
    // Cube fallback geometry
    AllocatedBuffer cube_verts_{};
    AllocatedBuffer cube_indices_{};
    AllocatedBuffer cube_instance_buf_{};
    void* cube_instance_mapped_ = nullptr;
    u32 cube_instance_count_ = 0;

    // Mesh instance buffer (shared across all mesh groups)
    AllocatedBuffer mesh_instance_buf_{};
    void* mesh_instance_mapped_ = nullptr;

    // Bone SSBO (all bone matrices for all mesh instances)
    AllocatedBuffer bone_ssbo_{};
    void* bone_ssbo_mapped_ = nullptr;

    // Per-frame draw groups (rebuilt each frame)
    std::vector<MeshDrawGroup> mesh_groups_;
};

} // namespace osc::renderer
