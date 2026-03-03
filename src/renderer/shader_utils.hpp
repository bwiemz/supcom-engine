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
} // namespace shaders

} // namespace osc::renderer
