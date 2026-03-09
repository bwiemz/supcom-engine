#include "renderer/mesh_cache.hpp"
#include "sim/scm_parser.hpp"
#include "blueprints/blueprint_store.hpp"
#include "vfs/virtual_file_system.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>

extern "C" {
#include "lua.h"
}

namespace osc::renderer {

/// Lowercase a string in-place (blueprint IDs in __blueprints are lowercased).
static void to_lower(std::string& s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
}

void MeshCache::init(VkDevice device, VmaAllocator allocator,
                     VkCommandPool cmd_pool, VkQueue queue,
                     vfs::VirtualFileSystem* vfs,
                     blueprints::BlueprintStore* store) {
    device_ = device;
    allocator_ = allocator;
    cmd_pool_ = cmd_pool;
    queue_ = queue;
    vfs_ = vfs;
    store_ = store;
}

const GPUMesh* MeshCache::get(const std::string& blueprint_id,
                               lua_State* L) {
    if (device_ == VK_NULL_HANDLE) return nullptr;

    auto it = cache_.find(blueprint_id);
    if (it != cache_.end()) return it->second.get();

    if (failed_.count(blueprint_id)) return nullptr;

    std::string mesh_path = resolve_mesh_path(blueprint_id, L);
    if (mesh_path.empty()) {
        // Log first few failures at INFO level for debugging
        if (failed_.size() < 5)
            spdlog::info("MeshCache: no mesh path for '{}' (cube fallback)", blueprint_id);
        else
            spdlog::debug("MeshCache: no mesh path for '{}'", blueprint_id);
        failed_.insert(blueprint_id);
        return nullptr;
    }

    auto file_data = vfs_->read_file(mesh_path);
    if (!file_data) {
        if (failed_.size() < 5)
            spdlog::info("MeshCache: VFS read failed for '{}' (cube fallback)", mesh_path);
        else
            spdlog::debug("MeshCache: VFS read failed for '{}'", mesh_path);
        failed_.insert(blueprint_id);
        return nullptr;
    }

    auto mesh = sim::parse_scm_mesh(*file_data);
    if (!mesh || mesh->vertices.empty() || mesh->indices.empty()) {
        if (failed_.size() < 5)
            spdlog::info("MeshCache: SCM parse failed for '{}' (cube fallback)", mesh_path);
        else
            spdlog::debug("MeshCache: SCM mesh parse failed for '{}'", mesh_path);
        failed_.insert(blueprint_id);
        return nullptr;
    }

    // Upload vertex buffer (position + normal + UV + bone_indices + bone_weights + tangent = 64 bytes per vert)
    auto vert_buf = upload_buffer(
        device_, allocator_, cmd_pool_, queue_,
        mesh->vertices.data(),
        mesh->vertices.size() * sizeof(sim::SCMMesh::Vertex),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);

    if (!vert_buf.buffer) {
        spdlog::warn("MeshCache: vertex upload failed for '{}'", mesh_path);
        failed_.insert(blueprint_id);
        return nullptr;
    }

    auto idx_buf = upload_buffer(
        device_, allocator_, cmd_pool_, queue_,
        mesh->indices.data(),
        mesh->indices.size() * sizeof(u32),
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT);

    if (!idx_buf.buffer) {
        vmaDestroyBuffer(allocator_, vert_buf.buffer, vert_buf.allocation);
        spdlog::warn("MeshCache: index upload failed for '{}'", mesh_path);
        failed_.insert(blueprint_id);
        return nullptr;
    }

    f32 scale = resolve_uniform_scale(blueprint_id, L);

    auto gpu = std::make_unique<GPUMesh>();
    gpu->vertex_buf = vert_buf;
    gpu->index_buf = idx_buf;
    gpu->index_count = static_cast<u32>(mesh->indices.size());
    gpu->uniform_scale = scale;
    gpu->texture_path = resolve_albedo_path(blueprint_id, L);
    gpu->specteam_path = resolve_specteam_path(blueprint_id, L);
    gpu->normal_path = resolve_normal_path(blueprint_id, L);

    spdlog::debug("MeshCache: loaded '{}' ({} verts, {} indices, scale={:.2f}, tex='{}', normal='{}')",
                   blueprint_id, mesh->vertices.size(), mesh->indices.size(),
                   scale, gpu->texture_path, gpu->normal_path);

    auto* raw = gpu.get();
    cache_[blueprint_id] = std::move(gpu);
    return raw;
}

std::string MeshCache::resolve_mesh_path(const std::string& bp_id,
                                          lua_State* L) {
    // Same strategy as BoneCache::resolve_mesh_path
    if (!store_ || !L) return {};

    auto* entry = store_->find(bp_id);
    if (!entry) return {};

    store_->push_lua_table(*entry, L);
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return {}; }
    int bp_table = lua_gettop(L);

    lua_pushstring(L, "Display");
    lua_rawget(L, bp_table);
    if (!lua_istable(L, -1)) { lua_pop(L, 2); return {}; }
    int display_table = lua_gettop(L);

    lua_pushstring(L, "MeshBlueprint");
    lua_rawget(L, display_table);
    if (!lua_isstring(L, -1)) { lua_pop(L, 3); return {}; }
    std::string mesh_bp_id = lua_tostring(L, -1);
    lua_pop(L, 3);

    if (mesh_bp_id.empty()) return {};

    // __blueprints keys are lowercased — normalize before lookup
    to_lower(mesh_bp_id);

    lua_pushstring(L, "__blueprints");
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return {}; }
    int blueprints_table = lua_gettop(L);

    lua_pushstring(L, mesh_bp_id.c_str());
    lua_rawget(L, blueprints_table);
    if (!lua_istable(L, -1)) { lua_pop(L, 2); return {}; }
    int mesh_bp = lua_gettop(L);

    // Strategy 1: LODs[1].MeshName
    std::string mesh_name;
    lua_pushstring(L, "LODs");
    lua_rawget(L, mesh_bp);
    if (lua_istable(L, -1)) {
        int lods_table = lua_gettop(L);
        lua_rawgeti(L, lods_table, 1);
        if (lua_istable(L, -1)) {
            int lod1 = lua_gettop(L);
            lua_pushstring(L, "MeshName");
            lua_rawget(L, lod1);
            if (lua_isstring(L, -1)) {
                mesh_name = lua_tostring(L, -1);
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);

    if (!mesh_name.empty()) {
        lua_pop(L, 2);
        if (mesh_name[0] != '/') {
            auto slash = mesh_bp_id.rfind('/');
            if (slash != std::string::npos) {
                mesh_name = mesh_bp_id.substr(0, slash + 1) + mesh_name;
            }
        }
        return mesh_name;
    }

    // Strategy 2: derive from BlueprintId
    lua_pushstring(L, "BlueprintId");
    lua_rawget(L, mesh_bp);
    std::string result;
    if (lua_isstring(L, -1)) {
        std::string bp_path = lua_tostring(L, -1);
        const std::string suffix = "_mesh";
        if (bp_path.size() > suffix.size() &&
            bp_path.compare(bp_path.size() - suffix.size(), suffix.size(),
                            suffix) == 0) {
            result = bp_path.substr(0, bp_path.size() - suffix.size()) +
                     "_lod0.scm";
        }
    }
    lua_pop(L, 3);
    return result;
}

std::string MeshCache::resolve_mesh_bp_id(const std::string& bp_id,
                                            lua_State* L) {
    if (!store_ || !L) return {};
    auto* entry = store_->find(bp_id);
    if (!entry) return {};
    store_->push_lua_table(*entry, L);
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return {}; }
    int bp_table = lua_gettop(L);
    lua_pushstring(L, "Display");
    lua_rawget(L, bp_table);
    if (!lua_istable(L, -1)) { lua_pop(L, 2); return {}; }
    int display_table = lua_gettop(L);
    lua_pushstring(L, "MeshBlueprint");
    lua_rawget(L, display_table);
    std::string result;
    if (lua_type(L, -1) == LUA_TSTRING) {
        result = lua_tostring(L, -1);
        to_lower(result); // __blueprints keys are lowercased
    }
    lua_pop(L, 3);
    return result;
}

std::string MeshCache::derive_base_path(const std::string& mesh_bp_id) {
    // "/units/uel0001/uel0001_mesh" → "/units/uel0001/uel0001"
    const std::string suffix = "_mesh";
    if (mesh_bp_id.size() > suffix.size() &&
        mesh_bp_id.compare(mesh_bp_id.size() - suffix.size(),
                           suffix.size(), suffix) == 0) {
        return mesh_bp_id.substr(0, mesh_bp_id.size() - suffix.size());
    }
    return {};
}

std::string MeshCache::resolve_albedo_path(const std::string& bp_id,
                                            lua_State* L) {
    std::string mesh_bp_id = resolve_mesh_bp_id(bp_id, L);
    if (mesh_bp_id.empty()) return {};

    // Strategy 1: LODs[1].AlbedoName from mesh blueprint (works if bp not overwritten)
    lua_pushstring(L, "__blueprints");
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (lua_istable(L, -1)) {
        int bps = lua_gettop(L);
        lua_pushstring(L, mesh_bp_id.c_str());
        lua_rawget(L, bps);
        if (lua_istable(L, -1)) {
            int mbp = lua_gettop(L);
            lua_pushstring(L, "LODs");
            lua_rawget(L, mbp);
            if (lua_istable(L, -1)) {
                int lods = lua_gettop(L);
                lua_rawgeti(L, lods, 1);
                if (lua_istable(L, -1)) {
                    int lod1 = lua_gettop(L);
                    lua_pushstring(L, "AlbedoName");
                    lua_rawget(L, lod1);
                    if (lua_type(L, -1) == LUA_TSTRING) {
                        std::string name = lua_tostring(L, -1);
                        lua_pop(L, 5); // AlbedoName+lod1+lods+mbp+bps
                        if (!name.empty()) {
                            if (name[0] != '/') {
                                auto slash = mesh_bp_id.rfind('/');
                                if (slash != std::string::npos)
                                    name = mesh_bp_id.substr(0, slash + 1) + name;
                            }
                            return name;
                        }
                    } else {
                        lua_pop(L, 1);
                    }
                }
                lua_pop(L, 1); // lod1 or rawgeti result
            }
            lua_pop(L, 1); // LODs
        }
        lua_pop(L, 1); // mbp
    }
    lua_pop(L, 1); // bps

    // Strategy 2: convention-based — derive from mesh bp ID
    // "/units/uel0001/uel0001_mesh" → "/units/uel0001/uel0001_Albedo.dds"
    std::string base = derive_base_path(mesh_bp_id);
    if (!base.empty()) {
        std::string path = base + "_Albedo.dds";
        if (vfs_ && vfs_->read_file(path)) return path;
    }

    return {};
}

std::string MeshCache::resolve_specteam_path(const std::string& bp_id,
                                              lua_State* L) {
    std::string mesh_bp_id = resolve_mesh_bp_id(bp_id, L);
    if (mesh_bp_id.empty()) return {};

    // Strategy 1: LODs[1].SpecularName from mesh blueprint
    lua_pushstring(L, "__blueprints");
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (lua_istable(L, -1)) {
        int bps = lua_gettop(L);
        lua_pushstring(L, mesh_bp_id.c_str());
        lua_rawget(L, bps);
        if (lua_istable(L, -1)) {
            int mbp = lua_gettop(L);
            lua_pushstring(L, "LODs");
            lua_rawget(L, mbp);
            if (lua_istable(L, -1)) {
                int lods = lua_gettop(L);
                lua_rawgeti(L, lods, 1);
                if (lua_istable(L, -1)) {
                    int lod1 = lua_gettop(L);
                    lua_pushstring(L, "SpecularName");
                    lua_rawget(L, lod1);
                    if (lua_type(L, -1) == LUA_TSTRING) {
                        std::string name = lua_tostring(L, -1);
                        lua_pop(L, 5);
                        if (!name.empty()) {
                            if (name[0] != '/') {
                                auto slash = mesh_bp_id.rfind('/');
                                if (slash != std::string::npos)
                                    name = mesh_bp_id.substr(0, slash + 1) + name;
                            }
                            return name;
                        }
                    } else {
                        lua_pop(L, 1);
                    }
                }
                lua_pop(L, 1);
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);

    // Strategy 2: convention-based — derive from mesh bp ID
    // "/units/uel0001/uel0001_mesh" → "/units/uel0001/uel0001_SpecTeam.dds"
    std::string base = derive_base_path(mesh_bp_id);
    if (!base.empty() && vfs_) {
        std::string path = base + "_SpecTeam.dds";
        if (vfs_->read_file(path)) return path;
    }

    return {};
}

std::string MeshCache::resolve_normal_path(const std::string& bp_id,
                                            lua_State* L) {
    std::string mesh_bp_id = resolve_mesh_bp_id(bp_id, L);
    if (mesh_bp_id.empty()) return {};

    // Strategy 1: LODs[1].NormalsName from mesh blueprint
    lua_pushstring(L, "__blueprints");
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (lua_istable(L, -1)) {
        int bps = lua_gettop(L);
        lua_pushstring(L, mesh_bp_id.c_str());
        lua_rawget(L, bps);
        if (lua_istable(L, -1)) {
            int mbp = lua_gettop(L);
            lua_pushstring(L, "LODs");
            lua_rawget(L, mbp);
            if (lua_istable(L, -1)) {
                int lods = lua_gettop(L);
                lua_rawgeti(L, lods, 1);
                if (lua_istable(L, -1)) {
                    int lod1 = lua_gettop(L);
                    lua_pushstring(L, "NormalsName");
                    lua_rawget(L, lod1);
                    if (lua_type(L, -1) == LUA_TSTRING) {
                        std::string name = lua_tostring(L, -1);
                        lua_pop(L, 5);
                        if (!name.empty()) {
                            if (name[0] != '/') {
                                auto slash = mesh_bp_id.rfind('/');
                                if (slash != std::string::npos)
                                    name = mesh_bp_id.substr(0, slash + 1) + name;
                            }
                            return name;
                        }
                    } else {
                        lua_pop(L, 1);
                    }
                }
                lua_pop(L, 1);
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);

    // Strategy 2: convention-based — derive from mesh bp ID
    // Try _normalsTS.dds (most common FA convention)
    std::string base = derive_base_path(mesh_bp_id);
    if (!base.empty() && vfs_) {
        std::string path = base + "_normalsTS.dds";
        if (vfs_->read_file(path)) return path;
        // Try capitalized variant
        path = base + "_NormalsTS.dds";
        if (vfs_->read_file(path)) return path;
    }

    return {};
}

f32 MeshCache::resolve_uniform_scale(const std::string& bp_id,
                                      lua_State* L) {
    if (!store_ || !L) return 1.0f;

    auto* entry = store_->find(bp_id);
    if (!entry) return 1.0f;

    store_->push_lua_table(*entry, L);
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return 1.0f; }
    int bp_table = lua_gettop(L);

    lua_pushstring(L, "Display");
    lua_rawget(L, bp_table);
    if (!lua_istable(L, -1)) { lua_pop(L, 2); return 1.0f; }
    int display_table = lua_gettop(L);

    lua_pushstring(L, "UniformScale");
    lua_rawget(L, display_table);
    f32 scale = 1.0f;
    if (lua_isnumber(L, -1)) {
        scale = static_cast<f32>(lua_tonumber(L, -1));
    }
    lua_pop(L, 3);

    return scale > 0.0f ? scale : 1.0f;
}

void MeshCache::destroy(VkDevice device, VmaAllocator allocator) {
    for (auto& [id, gpu] : cache_) {
        if (gpu->vertex_buf.buffer)
            vmaDestroyBuffer(allocator, gpu->vertex_buf.buffer,
                             gpu->vertex_buf.allocation);
        if (gpu->index_buf.buffer)
            vmaDestroyBuffer(allocator, gpu->index_buf.buffer,
                             gpu->index_buf.allocation);
    }
    cache_.clear();
    failed_.clear();
}

} // namespace osc::renderer
