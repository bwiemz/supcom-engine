#pragma once

#include "core/types.hpp"
#include "sim/entity.hpp" // Vector3, Quaternion

#include <string>
#include <unordered_map>
#include <vector>

namespace osc::sim {

struct BoneInfo {
    std::string name;
    Vector3 local_position;    // relative to parent
    Quaternion local_rotation; // relative to parent
    i32 parent_index = -1;     // -1 = root
    Vector3 world_position;    // pre-computed: accumulated from root
    Quaternion world_rotation; // pre-computed: accumulated from root
};

/// Shared bone data for all units of the same blueprint.
struct BoneData {
    std::vector<BoneInfo> bones;
    std::unordered_map<std::string, i32> name_to_index; // lowercase → index

    /// Look up bone index by name (case-insensitive). Returns -1 if not found.
    i32 find_bone(const std::string& name) const;

    /// Check if a bone index is valid.
    bool is_valid(i32 index) const {
        return index >= 0 && index < static_cast<i32>(bones.size());
    }

    i32 bone_count() const { return static_cast<i32>(bones.size()); }
};

} // namespace osc::sim
