#pragma once

#include "core/types.hpp"

#include <string>
#include <vector>

namespace osc::vfs { class VirtualFileSystem; }

namespace osc::renderer {

struct NormalDecalInfo {
    std::string texture_path;
    f32 position_x, position_z;  // World-space XZ center
    f32 scale_x, scale_z;        // World-space footprint
    f32 rotation_y;              // Y-axis rotation in radians
};

struct PredecodedNormal {
    std::string path;
    std::vector<u8> pixels; // RGBA, width*height*4 bytes
    u32 width, height;
};

struct NormalOverlay {
    u32 width = 0;
    u32 height = 0;
    std::vector<f32> pixels; // width*height*2 floats (RG pairs: nx, ny perturbation)
};

/// Bake normal map decals into a CPU-side RG overlay buffer.
/// Loads textures via VFS, decodes DXT5nm (BC3), rasterizes with rotation.
NormalOverlay bake_normal_overlay(
    const std::vector<NormalDecalInfo>& decals,
    u32 overlay_w, u32 overlay_h,
    vfs::VirtualFileSystem* vfs);

/// Bake normal map decals using pre-decoded RGBA pixel data (for testing).
NormalOverlay bake_normal_overlay_with_predecoded(
    const std::vector<NormalDecalInfo>& decals,
    u32 overlay_w, u32 overlay_h,
    const std::vector<PredecodedNormal>& predecoded);

} // namespace osc::renderer
