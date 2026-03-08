#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>

namespace osc::renderer {

/// Compile GLSL source to SPIR-V using shaderc, then create a VkShaderModule.
/// Returns VK_NULL_HANDLE on failure.
VkShaderModule compile_glsl(VkDevice device, const char* source,
                            const char* name, bool is_vertex);

/// All embedded shader sources.
namespace shaders {
extern const char* terrain_vert;
extern const char* terrain_frag;
extern const char* unit_vert;
extern const char* unit_frag;
extern const char* water_vert;
extern const char* water_frag;
extern const char* mesh_vert;
extern const char* mesh_frag;
extern const char* decal_vert;
extern const char* decal_frag;
extern const char* shadow_vert;       // terrain shadow (lightVP * position)
extern const char* shadow_mesh_vert;  // mesh shadow (blend-weight skinning + lightVP)
extern const char* shadow_unit_vert;  // cube shadow (instanced + lightVP)
extern const char* shadow_frag;       // empty (depth-only write)
extern const char* ui_vert;           // 2D UI quad (pixel coords → NDC)
extern const char* ui_frag;           // 2D UI quad (texture * color)
} // namespace shaders

} // namespace osc::renderer
