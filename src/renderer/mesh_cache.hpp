#pragma once

#include "renderer/vk_types.hpp"
#include "core/types.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

struct lua_State;

namespace osc::vfs {
class VirtualFileSystem;
}

namespace osc::blueprints {
class BlueprintStore;
}

namespace osc::renderer {

/// GPU-resident mesh data for a single blueprint.
struct GPUMesh {
    AllocatedBuffer vertex_buf{};
    AllocatedBuffer index_buf{};
    u32 index_count = 0;
    f32 uniform_scale = 1.0f;
    std::string texture_path;  // VFS path to albedo DDS (empty = no texture)
};

/// Caches GPU mesh buffers per blueprint ID.
/// Lazily loads .scm files via VFS, parses vertices+indices, uploads to GPU.
class MeshCache {
public:
    void init(VkDevice device, VmaAllocator allocator,
              VkCommandPool cmd_pool, VkQueue queue,
              vfs::VirtualFileSystem* vfs,
              blueprints::BlueprintStore* store);

    /// Get or lazily load GPU mesh for a blueprint ID.
    /// Returns nullptr if mesh unavailable (caller falls back to cube).
    const GPUMesh* get(const std::string& blueprint_id, lua_State* L);

    void destroy(VkDevice device, VmaAllocator allocator);

private:
    std::string resolve_mesh_path(const std::string& bp_id, lua_State* L);
    std::string resolve_albedo_path(const std::string& bp_id, lua_State* L);
    f32 resolve_uniform_scale(const std::string& bp_id, lua_State* L);

    std::unordered_map<std::string, std::unique_ptr<GPUMesh>> cache_;
    std::unordered_set<std::string> failed_;

    VkDevice device_ = VK_NULL_HANDLE;
    VmaAllocator allocator_ = VK_NULL_HANDLE;
    VkCommandPool cmd_pool_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    vfs::VirtualFileSystem* vfs_ = nullptr;
    blueprints::BlueprintStore* store_ = nullptr;
};

} // namespace osc::renderer
