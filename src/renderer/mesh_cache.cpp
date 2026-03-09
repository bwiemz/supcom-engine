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

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

const GPUMesh* MeshCache::get(const std::string& blueprint_id,
                               lua_State* L) {
    return get_lod(blueprint_id, 0.0f, L);
}

const GPUMesh* MeshCache::get_lod(const std::string& blueprint_id,
                                   f32 camera_distance, lua_State* L) {
    if (device_ == VK_NULL_HANDLE) return nullptr;

    // Check if already loaded
    auto it = lod_cache_.find(blueprint_id);
    if (it != lod_cache_.end()) {
        const auto& lods = it->second.lods;
        if (lods.empty()) return nullptr;

        // Walk from highest detail (index 0) to lowest
        for (const auto& entry : lods) {
            if (entry.cutoff == 0.0f || entry.cutoff >= camera_distance) {
                return &entry.mesh;
            }
        }
        // Camera distance exceeds all cutoffs — return lowest detail
        return &lods.back().mesh;
    }

    if (failed_.count(blueprint_id)) return nullptr;

    // Try loading all LODs
    if (!load_lod_set(blueprint_id, L)) {
        failed_.insert(blueprint_id);
        return nullptr;
    }

    // Recurse once now that lod_cache_ is populated
    return get_lod(blueprint_id, camera_distance, L);
}

const LODSet* MeshCache::get_lod_set(const std::string& blueprint_id) const {
    auto it = lod_cache_.find(blueprint_id);
    if (it != lod_cache_.end()) return &it->second;
    return nullptr;
}

// ---------------------------------------------------------------------------
// LOD loading
// ---------------------------------------------------------------------------

GPUMesh MeshCache::upload_scm_mesh(const std::string& mesh_path) {
    GPUMesh result{};

    auto file_data = vfs_->read_file(mesh_path);
    if (!file_data) {
        spdlog::debug("MeshCache: VFS read failed for '{}'", mesh_path);
        return result;
    }

    auto mesh = sim::parse_scm_mesh(*file_data);
    if (!mesh || mesh->vertices.empty() || mesh->indices.empty()) {
        spdlog::debug("MeshCache: SCM parse failed for '{}'", mesh_path);
        return result;
    }

    auto vert_buf = upload_buffer(
        device_, allocator_, cmd_pool_, queue_,
        mesh->vertices.data(),
        mesh->vertices.size() * sizeof(sim::SCMMesh::Vertex),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);

    if (!vert_buf.buffer) {
        spdlog::warn("MeshCache: vertex upload failed for '{}'", mesh_path);
        return result;
    }

    auto idx_buf = upload_buffer(
        device_, allocator_, cmd_pool_, queue_,
        mesh->indices.data(),
        mesh->indices.size() * sizeof(u32),
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT);

    if (!idx_buf.buffer) {
        vmaDestroyBuffer(allocator_, vert_buf.buffer, vert_buf.allocation);
        spdlog::warn("MeshCache: index upload failed for '{}'", mesh_path);
        return result;
    }

    result.vertex_buf = vert_buf;
    result.index_buf = idx_buf;
    result.index_count = static_cast<u32>(mesh->indices.size());

    spdlog::debug("MeshCache: uploaded '{}' ({} verts, {} indices)",
                   mesh_path, mesh->vertices.size(), mesh->indices.size());
    return result;
}

bool MeshCache::load_lod_set(const std::string& bp_id, lua_State* L) {
    std::string mesh_bp_id = resolve_mesh_bp_id(bp_id, L);
    if (mesh_bp_id.empty()) {
        if (failed_.size() < 5)
            spdlog::info("MeshCache: no mesh blueprint for '{}' (cube fallback)", bp_id);
        else
            spdlog::debug("MeshCache: no mesh blueprint for '{}'", bp_id);
        return false;
    }

    f32 scale = resolve_uniform_scale(bp_id, L);
    LODSet lod_set;

    // Try LODs[1] through LODs[4] from the mesh blueprint
    for (i32 lod_index = 1; lod_index <= 4; ++lod_index) {
        std::string mesh_path = resolve_mesh_path_for_lod(mesh_bp_id, lod_index, L);
        if (mesh_path.empty()) continue;

        GPUMesh gpu = upload_scm_mesh(mesh_path);
        if (!gpu.vertex_buf.buffer) continue;

        gpu.uniform_scale = scale;

        // Resolve textures for this LOD, falling back to LOD 1 textures
        gpu.texture_path = resolve_albedo_path_for_lod(mesh_bp_id, lod_index, L);
        gpu.specteam_path = resolve_specteam_path_for_lod(mesh_bp_id, lod_index, L);
        gpu.normal_path = resolve_normal_path_for_lod(mesh_bp_id, lod_index, L);

        f32 cutoff = read_lod_cutoff(mesh_bp_id, lod_index, L);

        LODEntry entry;
        entry.mesh = std::move(gpu);
        entry.cutoff = cutoff;
        lod_set.lods.push_back(std::move(entry));
    }

    // If no LODs found with MeshName fields (common due to ExtractMeshBlueprint
    // overwriting LODs[1]), fall back to existing single-LOD loading
    if (lod_set.lods.empty()) {
        std::string mesh_path = resolve_mesh_path(bp_id, L);
        if (mesh_path.empty()) {
            if (failed_.size() < 5)
                spdlog::info("MeshCache: no mesh path for '{}' (cube fallback)", bp_id);
            else
                spdlog::debug("MeshCache: no mesh path for '{}'", bp_id);
            return false;
        }

        GPUMesh gpu = upload_scm_mesh(mesh_path);
        if (!gpu.vertex_buf.buffer) {
            if (failed_.size() < 5)
                spdlog::info("MeshCache: upload failed for '{}' (cube fallback)", mesh_path);
            else
                spdlog::debug("MeshCache: upload failed for '{}'", mesh_path);
            return false;
        }

        gpu.uniform_scale = scale;
        gpu.texture_path = resolve_albedo_path(bp_id, L);
        gpu.specteam_path = resolve_specteam_path(bp_id, L);
        gpu.normal_path = resolve_normal_path(bp_id, L);

        LODEntry entry;
        entry.mesh = std::move(gpu);
        entry.cutoff = 0.0f; // no limit — single LOD
        lod_set.lods.push_back(std::move(entry));
    }

    // Sort by cutoff ascending (highest detail / smallest cutoff first)
    std::sort(lod_set.lods.begin(), lod_set.lods.end(),
              [](const LODEntry& a, const LODEntry& b) {
                  // 0 means "no limit" — sort to the end
                  if (a.cutoff == 0.0f && b.cutoff == 0.0f) return false;
                  if (a.cutoff == 0.0f) return false;
                  if (b.cutoff == 0.0f) return true;
                  return a.cutoff < b.cutoff;
              });

    spdlog::debug("MeshCache: loaded '{}' with {} LOD level(s), scale={:.2f}",
                   bp_id, lod_set.lods.size(), scale);

    lod_cache_[bp_id] = std::move(lod_set);
    return true;
}

// ---------------------------------------------------------------------------
// LOD field readers
// ---------------------------------------------------------------------------

std::string MeshCache::read_lod_string_field(const std::string& mesh_bp_id,
                                              i32 lod_index,
                                              const char* field_name,
                                              lua_State* L) {
    if (!L) return {};

    lua_pushstring(L, "__blueprints");
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return {}; }
    int bps = lua_gettop(L);

    lua_pushstring(L, mesh_bp_id.c_str());
    lua_rawget(L, bps);
    if (!lua_istable(L, -1)) { lua_pop(L, 2); return {}; }
    int mbp = lua_gettop(L);

    lua_pushstring(L, "LODs");
    lua_rawget(L, mbp);
    if (!lua_istable(L, -1)) { lua_pop(L, 3); return {}; }
    int lods = lua_gettop(L);

    lua_rawgeti(L, lods, lod_index);
    if (!lua_istable(L, -1)) { lua_pop(L, 4); return {}; }
    int lod_table = lua_gettop(L);

    lua_pushstring(L, field_name);
    lua_rawget(L, lod_table);
    std::string result;
    if (lua_type(L, -1) == LUA_TSTRING) {
        result = lua_tostring(L, -1);
    }
    lua_pop(L, 5); // field + lod_table + lods + mbp + bps

    return result;
}

f32 MeshCache::read_lod_cutoff(const std::string& mesh_bp_id, i32 lod_index,
                                lua_State* L) {
    if (!L) return 0.0f;

    lua_pushstring(L, "__blueprints");
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return 0.0f; }
    int bps = lua_gettop(L);

    lua_pushstring(L, mesh_bp_id.c_str());
    lua_rawget(L, bps);
    if (!lua_istable(L, -1)) { lua_pop(L, 2); return 0.0f; }
    int mbp = lua_gettop(L);

    lua_pushstring(L, "LODs");
    lua_rawget(L, mbp);
    if (!lua_istable(L, -1)) { lua_pop(L, 3); return 0.0f; }
    int lods_tbl = lua_gettop(L);

    lua_rawgeti(L, lods_tbl, lod_index);
    if (!lua_istable(L, -1)) { lua_pop(L, 4); return 0.0f; }
    int lod_table = lua_gettop(L);

    lua_pushstring(L, "LODCutoff");
    lua_rawget(L, lod_table);
    f32 cutoff = 0.0f;
    if (lua_isnumber(L, -1)) {
        cutoff = static_cast<f32>(lua_tonumber(L, -1));
    }
    lua_pop(L, 5); // cutoff + lod_table + lods_tbl + mbp + bps

    return cutoff;
}

std::string MeshCache::resolve_mesh_path_for_lod(const std::string& mesh_bp_id,
                                                  i32 lod_index, lua_State* L) {
    std::string mesh_name = read_lod_string_field(mesh_bp_id, lod_index, "MeshName", L);
    if (mesh_name.empty()) return {};

    // Make path absolute if relative
    if (mesh_name[0] != '/') {
        auto slash = mesh_bp_id.rfind('/');
        if (slash != std::string::npos) {
            mesh_name = mesh_bp_id.substr(0, slash + 1) + mesh_name;
        }
    }
    return mesh_name;
}

std::string MeshCache::resolve_albedo_path_for_lod(const std::string& mesh_bp_id,
                                                    i32 lod_index, lua_State* L) {
    // Try AlbedoName from this LOD
    std::string name = read_lod_string_field(mesh_bp_id, lod_index, "AlbedoName", L);

    // Fall back to LOD 1 if missing (textures often shared across LODs)
    if (name.empty() && lod_index != 1) {
        name = read_lod_string_field(mesh_bp_id, 1, "AlbedoName", L);
    }

    if (!name.empty()) {
        if (name[0] != '/') {
            auto slash = mesh_bp_id.rfind('/');
            if (slash != std::string::npos)
                name = mesh_bp_id.substr(0, slash + 1) + name;
        }
        return name;
    }

    // Convention-based fallback
    std::string base = derive_base_path(mesh_bp_id);
    if (!base.empty() && vfs_) {
        std::string path = base + "_Albedo.dds";
        if (vfs_->read_file(path)) return path;
    }
    return {};
}

std::string MeshCache::resolve_specteam_path_for_lod(const std::string& mesh_bp_id,
                                                      i32 lod_index, lua_State* L) {
    std::string name = read_lod_string_field(mesh_bp_id, lod_index, "SpecularName", L);

    if (name.empty() && lod_index != 1) {
        name = read_lod_string_field(mesh_bp_id, 1, "SpecularName", L);
    }

    if (!name.empty()) {
        if (name[0] != '/') {
            auto slash = mesh_bp_id.rfind('/');
            if (slash != std::string::npos)
                name = mesh_bp_id.substr(0, slash + 1) + name;
        }
        return name;
    }

    std::string base = derive_base_path(mesh_bp_id);
    if (!base.empty() && vfs_) {
        std::string path = base + "_SpecTeam.dds";
        if (vfs_->read_file(path)) return path;
    }
    return {};
}

std::string MeshCache::resolve_normal_path_for_lod(const std::string& mesh_bp_id,
                                                    i32 lod_index, lua_State* L) {
    std::string name = read_lod_string_field(mesh_bp_id, lod_index, "NormalsName", L);

    if (name.empty() && lod_index != 1) {
        name = read_lod_string_field(mesh_bp_id, 1, "NormalsName", L);
    }

    if (!name.empty()) {
        if (name[0] != '/') {
            auto slash = mesh_bp_id.rfind('/');
            if (slash != std::string::npos)
                name = mesh_bp_id.substr(0, slash + 1) + name;
        }
        return name;
    }

    std::string base = derive_base_path(mesh_bp_id);
    if (!base.empty() && vfs_) {
        std::string path = base + "_normalsTS.dds";
        if (vfs_->read_file(path)) return path;
        path = base + "_NormalsTS.dds";
        if (vfs_->read_file(path)) return path;
    }
    return {};
}

// ---------------------------------------------------------------------------
// Existing resolve helpers (kept for single-LOD fallback path)
// ---------------------------------------------------------------------------

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
    // "/units/uel0001/uel0001_mesh" -> "/units/uel0001/uel0001"
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
    // "/units/uel0001/uel0001_mesh" -> "/units/uel0001/uel0001_Albedo.dds"
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
    // "/units/uel0001/uel0001_mesh" -> "/units/uel0001/uel0001_SpecTeam.dds"
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

// ---------------------------------------------------------------------------
// Cleanup
// ---------------------------------------------------------------------------

void MeshCache::destroy(VkDevice device, VmaAllocator allocator) {
    for (auto& [id, lod_set] : lod_cache_) {
        for (auto& entry : lod_set.lods) {
            if (entry.mesh.vertex_buf.buffer)
                vmaDestroyBuffer(allocator, entry.mesh.vertex_buf.buffer,
                                 entry.mesh.vertex_buf.allocation);
            if (entry.mesh.index_buf.buffer)
                vmaDestroyBuffer(allocator, entry.mesh.index_buf.buffer,
                                 entry.mesh.index_buf.allocation);
        }
    }
    lod_cache_.clear();
    failed_.clear();
}

} // namespace osc::renderer
