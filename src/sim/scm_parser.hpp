#pragma once

#include "sim/bone_data.hpp"

#include <optional>
#include <vector>

namespace osc::sim {

/// Parse bone data from an SCM (Supreme Commander Model) v5 file.
/// Only reads header + bone names + bone entries; skips vertices/indices.
/// Returns nullopt on parse failure.
std::optional<BoneData> parse_scm_bones(const std::vector<char>& file_data);

/// Parsed mesh geometry from an SCM v5 file (position + normal + UV).
struct SCMMesh {
    struct Vertex {
        f32 px, py, pz;   // position
        f32 nx, ny, nz;   // normal
        f32 u, v;          // UV1 texture coordinates
        i32 bone_index;    // rigid skinning: primary bone index
    };
    std::vector<Vertex> vertices;
    std::vector<u32> indices;  // converted from u16 to u32
};

/// Parse mesh geometry (vertices + indices) from an SCM v5 file.
/// Reads position + normal + UV1 per vertex, skips tangent/binormal/UV2/bone.
/// Returns nullopt on parse failure.
std::optional<SCMMesh> parse_scm_mesh(const std::vector<char>& file_data);

} // namespace osc::sim
