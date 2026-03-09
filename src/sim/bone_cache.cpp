#include "sim/bone_cache.hpp"
#include "sim/scm_parser.hpp"
#include "blueprints/blueprint_store.hpp"
#include "vfs/virtual_file_system.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>

extern "C" {
#include "lua.h"
}

namespace osc::sim {

BoneCache::BoneCache(vfs::VirtualFileSystem* vfs,
                     blueprints::BlueprintStore* store)
    : vfs_(vfs), store_(store) {
    // Create fallback BoneData: single "root" bone at origin
    fallback_ = std::make_unique<BoneData>();
    BoneInfo root;
    root.name = "root";
    root.parent_index = -1;
    fallback_->bones.push_back(std::move(root));
    fallback_->name_to_index["root"] = 0;
}

const BoneData* BoneCache::get(const std::string& blueprint_id,
                                lua_State* L) {
    // Check cache
    auto it = cache_.find(blueprint_id);
    if (it != cache_.end()) return it->second.get();

    // Check failed set (don't retry)
    if (failed_.count(blueprint_id)) return fallback_.get();

    // Resolve mesh path from blueprint Lua tables
    std::string mesh_path = resolve_mesh_path(blueprint_id, L);
    if (mesh_path.empty()) {
        spdlog::debug("BoneCache: no mesh path for '{}'", blueprint_id);
        failed_.insert(blueprint_id);
        return fallback_.get();
    }

    // Read .scm file from VFS
    auto file_data = vfs_->read_file(mesh_path);
    if (!file_data) {
        spdlog::debug("BoneCache: VFS read failed for '{}'", mesh_path);
        failed_.insert(blueprint_id);
        return fallback_.get();
    }

    // Parse bones
    auto bone_data = parse_scm_bones(*file_data);
    if (!bone_data) {
        spdlog::debug("BoneCache: SCM parse failed for '{}'", mesh_path);
        failed_.insert(blueprint_id);
        return fallback_.get();
    }

    spdlog::debug("BoneCache: loaded {} bones for '{}' from '{}'",
                   bone_data->bone_count(), blueprint_id, mesh_path);

    auto ptr = std::make_unique<BoneData>(std::move(*bone_data));
    auto* raw = ptr.get();
    cache_[blueprint_id] = std::move(ptr);
    return raw;
}

std::string BoneCache::resolve_mesh_path(const std::string& bp_id,
                                          lua_State* L) {
    if (!store_ || !L) return {};

    auto* entry = store_->find(bp_id);
    if (!entry) return {};

    // Push unit blueprint table
    store_->push_lua_table(*entry, L);
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return {}; }
    int bp_table = lua_gettop(L);

    // Navigate: Display.MeshBlueprint → mesh blueprint ID string
    lua_pushstring(L, "Display");
    lua_rawget(L, bp_table);
    if (!lua_istable(L, -1)) { lua_pop(L, 2); return {}; }
    int display_table = lua_gettop(L);

    lua_pushstring(L, "MeshBlueprint");
    lua_rawget(L, display_table);
    if (!lua_isstring(L, -1)) { lua_pop(L, 3); return {}; }
    std::string mesh_bp_id = lua_tostring(L, -1);
    lua_pop(L, 3); // MeshBlueprint + Display + bp_table

    if (mesh_bp_id.empty()) return {};

    // __blueprints keys are lowercased — normalize before lookup
    std::transform(mesh_bp_id.begin(), mesh_bp_id.end(), mesh_bp_id.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    // Look up mesh blueprint in __blueprints global
    lua_pushstring(L, "__blueprints");
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return {}; }
    int blueprints_table = lua_gettop(L);

    lua_pushstring(L, mesh_bp_id.c_str());
    lua_rawget(L, blueprints_table);
    if (!lua_istable(L, -1)) { lua_pop(L, 2); return {}; }
    int mesh_bp = lua_gettop(L);

    // Strategy 1: Try LODs[1].MeshName (standalone _mesh.bp files have this)
    // Stack: [..., __blueprints, mesh_bp]
    std::string mesh_name;
    lua_pushstring(L, "LODs");
    lua_rawget(L, mesh_bp);
    if (lua_istable(L, -1)) {
        int lods_table = lua_gettop(L);
        lua_rawgeti(L, lods_table, 1); // LODs[1]
        if (lua_istable(L, -1)) {
            int lod1 = lua_gettop(L);
            lua_pushstring(L, "MeshName");
            lua_rawget(L, lod1);
            if (lua_isstring(L, -1)) {
                mesh_name = lua_tostring(L, -1);
            }
            lua_pop(L, 1); // MeshName
        }
        lua_pop(L, 1); // LODs[1]
    }
    lua_pop(L, 1); // LODs
    // Stack: [..., __blueprints, mesh_bp]

    if (!mesh_name.empty()) {
        lua_pop(L, 2); // mesh_bp + __blueprints
        // MeshName may be relative (e.g. "uel0001_lod0.scm")
        if (mesh_name[0] != '/') {
            auto slash = mesh_bp_id.rfind('/');
            if (slash != std::string::npos) {
                mesh_name = mesh_bp_id.substr(0, slash + 1) + mesh_name;
            }
        }
        return mesh_name;
    }

    // Strategy 2: Derive SCM path from BlueprintId
    // Convention: BlueprintId "/units/uel0001/uel0001_mesh" → "/units/uel0001/uel0001_lod0.scm"
    lua_pushstring(L, "BlueprintId");
    lua_rawget(L, mesh_bp);
    std::string result;
    if (lua_isstring(L, -1)) {
        std::string bp_path = lua_tostring(L, -1);
        // Replace trailing "_mesh" with "_lod0.scm"
        const std::string suffix = "_mesh";
        if (bp_path.size() > suffix.size() &&
            bp_path.compare(bp_path.size() - suffix.size(), suffix.size(), suffix) == 0) {
            result = bp_path.substr(0, bp_path.size() - suffix.size()) + "_lod0.scm";
        }
    }
    lua_pop(L, 3); // BlueprintId + mesh_bp + __blueprints
    return result;
}

} // namespace osc::sim
