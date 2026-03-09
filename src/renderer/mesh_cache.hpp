#pragma once

#include "renderer/vk_types.hpp"
#include "core/types.hpp"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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
    std::string texture_path;   // VFS path to albedo DDS (empty = no texture)
    std::string specteam_path;  // VFS path to SpecTeam DDS (empty = no team color mask)
    std::string normal_path;    // VFS path to normal map DDS (empty = no normal map)
};

/// A single LOD level: mesh data + camera distance cutoff.
struct LODEntry {
    GPUMesh mesh;
    f32 cutoff = 0.0f;  // max camera distance for this LOD (0 = no limit)
};

/// All LOD levels for a single blueprint, sorted by cutoff ascending
/// (highest detail / smallest cutoff first).
struct LODSet {
    std::vector<LODEntry> lods;  // sorted by cutoff ascending (highest detail first)
};

/// Caches GPU mesh buffers per blueprint ID.
/// Lazily loads .scm files via VFS, parses vertices+indices, uploads to GPU.
/// Supports multiple LOD levels per blueprint with distance-based selection.
class MeshCache {
public:
    void init(VkDevice device, VmaAllocator allocator,
              VkCommandPool cmd_pool, VkQueue queue,
              vfs::VirtualFileSystem* vfs,
              blueprints::BlueprintStore* store);

    /// Get or lazily load GPU mesh for a blueprint ID (highest detail LOD).
    /// Returns nullptr if mesh unavailable (caller falls back to cube).
    const GPUMesh* get(const std::string& blueprint_id, lua_State* L);

    /// Get best LOD mesh for given camera distance.
    /// Returns nullptr if mesh unavailable.
    const GPUMesh* get_lod(const std::string& blueprint_id, f32 camera_distance,
                           lua_State* L);

    /// Get the full LODSet for introspection. Returns nullptr if not loaded.
    const LODSet* get_lod_set(const std::string& blueprint_id) const;

    void destroy(VkDevice device, VmaAllocator allocator);

private:
    std::string resolve_mesh_path(const std::string& bp_id, lua_State* L);
    std::string resolve_albedo_path(const std::string& bp_id, lua_State* L);
    std::string resolve_specteam_path(const std::string& bp_id, lua_State* L);
    std::string resolve_normal_path(const std::string& bp_id, lua_State* L);
    f32 resolve_uniform_scale(const std::string& bp_id, lua_State* L);
    /// Get Display.MeshBlueprint ID from a unit blueprint.
    std::string resolve_mesh_bp_id(const std::string& bp_id, lua_State* L);
    /// Derive base path from mesh bp ID: "/units/uel0001/uel0001_mesh" -> "/units/uel0001/uel0001"
    static std::string derive_base_path(const std::string& mesh_bp_id);

    /// Load all LOD levels for a blueprint. Returns true if at least one LOD loaded.
    bool load_lod_set(const std::string& bp_id, lua_State* L);

    /// Read LODCutoff from __blueprints[mesh_bp_id].LODs[lod_index].
    f32 read_lod_cutoff(const std::string& mesh_bp_id, i32 lod_index, lua_State* L);

    /// Read MeshName from __blueprints[mesh_bp_id].LODs[lod_index].
    std::string resolve_mesh_path_for_lod(const std::string& mesh_bp_id, i32 lod_index,
                                          lua_State* L);

    /// Read texture paths from __blueprints[mesh_bp_id].LODs[lod_index].
    /// Falls back to LOD 1 texture if field missing (textures often shared).
    std::string resolve_albedo_path_for_lod(const std::string& mesh_bp_id, i32 lod_index,
                                            lua_State* L);
    std::string resolve_specteam_path_for_lod(const std::string& mesh_bp_id, i32 lod_index,
                                              lua_State* L);
    std::string resolve_normal_path_for_lod(const std::string& mesh_bp_id, i32 lod_index,
                                            lua_State* L);

    /// Read a string field from __blueprints[mesh_bp_id].LODs[lod_index].
    /// Returns empty string if not found.
    std::string read_lod_string_field(const std::string& mesh_bp_id, i32 lod_index,
                                      const char* field_name, lua_State* L);

    /// Upload a single SCM mesh to GPU buffers. Returns empty GPUMesh on failure.
    GPUMesh upload_scm_mesh(const std::string& mesh_path);

    std::unordered_map<std::string, LODSet> lod_cache_;
    std::unordered_set<std::string> failed_;

    VkDevice device_ = VK_NULL_HANDLE;
    VmaAllocator allocator_ = VK_NULL_HANDLE;
    VkCommandPool cmd_pool_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    vfs::VirtualFileSystem* vfs_ = nullptr;
    blueprints::BlueprintStore* store_ = nullptr;
};

} // namespace osc::renderer
