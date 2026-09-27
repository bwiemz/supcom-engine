#pragma once

#include "sim/bone_data.hpp"

#include <optional>
#include <vector>

namespace osc::sim {

/// Parse bone data from an SCM (Supreme Commander Model) v5 file.
/// Only reads header + bone names + bone entries; skips vertices/indices.
/// Returns nullopt on parse failure.
std::optional<BoneData> parse_scm_bones(const std::vector<char>& file_data);

/// Parsed mesh geometry from an SCM v5 file (position, the normal-mapping
/// basis, UV).
struct SCMMesh {
    struct Vertex {
        f32 px, py, pz;       // position
        f32 nx, ny, nz;       // normal
        f32 u, v;              // UV1 texture coordinates
        u8 bone_indices[4];    // the SCM's four bone indices
        f32 bone_weights[4];   // (1, 0, 0, 0): FA skins by the first alone (M211h)
        f32 tx, ty, tz;        // tangent: along u
        f32 bx, by, bz;        // binormal: along v
    };
    std::vector<Vertex> vertices;
    std::vector<u32> indices;  // converted from u16 to u32
};

/// Parse mesh geometry (vertices + indices) from an SCM v5 file.
/// Reads position, normal, tangent, binormal, UV1 and bone_indices[4] per
/// vertex; skips UV2.
/// Returns nullopt on parse failure.
std::optional<SCMMesh> parse_scm_mesh(const std::vector<char>& file_data);

} // namespace osc::sim
