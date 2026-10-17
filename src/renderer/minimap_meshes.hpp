#pragma once

// The cartographic minimap's meshes (Moho's Cartographic::RenderMeshes; faf-re
// render/Cartographic.cpp, camera/GeomCamera3.cpp, mesh/Mesh.cpp): every mesh
// batched through the minimap's own camera and drawn in its cartographic
// technique, after the frame's Stage 1.
// - The camera is orthographic, so its LOD metric is the same everywhere: the
//   view's width times the camera's LOD scale (GeomCamera3: viewport row 1 is
//   row 0, the width at a depth, times lodScale). A minimap view's camera
//   takes cam_DefaultMiniLOD (1.8) when made.
// - A mesh shows while some LOD takes the metric (Mesh::ComputeLOD): one whose
//   LODCutoff is 0 or less takes any, another one up to its cutoff
//   (RMeshBlueprintLOD's default 1000 where the blueprint gives none).
// - FAF's patched game skips RenderMeshes while cam_DefaultMiniLOD is 0
//   (FA-Binary-Patches MinimapMesh.cpp), which FAF's minimap toggle sets.

#include "core/types.hpp"

#include <limits>
#include <optional>
#include <vector>

namespace osc::renderer {

/// RMeshBlueprintLOD's LODCutoff where a LOD gives none.
inline constexpr f32 kDefaultLodCutoff = 1000.0f;
/// A reach no metric passes: a LOD with a cutoff of 0 or less.
inline constexpr f32 kUnlimitedReach = std::numeric_limits<f32>::infinity();

/// How far a mesh shows, from its LODs' LODCutoffs (none: the default): the
/// largest, or for ever when any is 0 or less. 0 with no LODs (nothing shows).
f32 lod_reach(const std::vector<std::optional<f32>>& cutoffs);

/// The minimap camera's LOD metric: its view's width (world units) times its
/// LOD scale.
inline f32 minimap_lod_metric(f32 view_width, f32 lod_scale) {
    return view_width * lod_scale;
}

/// Whether the minimap draws its meshes this frame: not in FAF's patched
/// game while cam_DefaultMiniLOD is 0 (`faf_exe`: FAF's data, which the
/// engine plays as its patched game).
inline bool minimap_meshes_on(bool faf_exe, f32 default_mini_lod) {
    return !(faf_exe && default_mini_lod == 0.0f);
}

} // namespace osc::renderer
