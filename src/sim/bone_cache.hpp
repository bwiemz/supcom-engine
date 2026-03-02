#pragma once

#include "sim/bone_data.hpp"

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

namespace osc::sim {

/// Caches parsed BoneData per blueprint ID.
/// Lazily loads .scm files on first access via VFS.
class BoneCache {
public:
    BoneCache(vfs::VirtualFileSystem* vfs, blueprints::BlueprintStore* store);

    /// Get bone data for a blueprint. Lazily parses .scm on first access.
    /// Returns pointer to single-root fallback if mesh not found.
    const BoneData* get(const std::string& blueprint_id, lua_State* L);

private:
    /// Resolve blueprint ID → .scm VFS path by navigating Lua tables:
    /// unitBP.Display.MeshBlueprint → __blueprints[meshId].LODs[1].MeshName
    std::string resolve_mesh_path(const std::string& bp_id, lua_State* L);

    vfs::VirtualFileSystem* vfs_;
    blueprints::BlueprintStore* store_;
    std::unordered_map<std::string, std::unique_ptr<BoneData>> cache_;
    std::unordered_set<std::string> failed_; // IDs that failed to load
    std::unique_ptr<BoneData> fallback_;     // single "root" bone at origin
};

} // namespace osc::sim
