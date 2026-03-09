#pragma once

#include "core/types.hpp"
#include "map/terrain.hpp"

#include <string>
#include <vector>

namespace osc::vfs { class VirtualFileSystem; }

namespace osc::renderer {

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
    const std::vector<osc::map::NormalDecalInfo>& decals,
    u32 overlay_w, u32 overlay_h,
    vfs::VirtualFileSystem* vfs);

/// Bake normal map decals using pre-decoded RGBA pixel data (for testing).
NormalOverlay bake_normal_overlay_with_predecoded(
    const std::vector<osc::map::NormalDecalInfo>& decals,
    u32 overlay_w, u32 overlay_h,
    const std::vector<PredecodedNormal>& predecoded);

} // namespace osc::renderer
