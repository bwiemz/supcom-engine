#pragma once

#include "core/types.hpp"
#include "sim/entity.hpp" // Vector3, Quaternion

#include <optional>
#include <string>
#include <vector>

namespace osc::sim {

/// Per-bone transform for a single animation frame.
struct SCABoneFrame {
    Vector3 position;     // bone-local position
    Quaternion rotation;  // bone-local rotation (x,y,z,w)
};

/// A single animation frame (all bones).
struct SCAFrame {
    f32 time = 0.0f;                    // timestamp in seconds
    std::vector<SCABoneFrame> bones;    // num_bones entries
};

/// Parsed SCA (Supreme Commander Animation) v5 data.
struct SCAData {
    u32 num_frames = 0;
    u32 num_bones = 0;
    f32 duration = 0.0f;                       // total duration in seconds
    std::vector<std::string> bone_names;       // from NAME section
    std::vector<i32> parent_indices;           // from LINK section
    std::vector<SCAFrame> frames;              // num_frames entries
};

/// Parse an SCA (Supreme Commander Animation) v5 file.
/// Returns nullopt on parse failure.
std::optional<SCAData> parse_sca(const std::vector<char>& file_data);

} // namespace osc::sim
