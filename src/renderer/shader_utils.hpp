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
/// The terrain's fragment shader, built around its shared surface (M212b).
const char* terrain_frag();
extern const char* unit_vert;
extern const char* unit_frag;
extern const char* water_vert;
extern const char* water_frag;
extern const char* water_mask_frag; // TWaterLayAlphaMask: alpha 0 over open water (M213a)
extern const char* mesh_vert;
extern const char* mesh_frag;
extern const char* decal_lit_vert; // the map's decals over the terrain's vertices (M212b)
/// The map's decals, lit as the terrain is (DecalsPS, DecalAlbedoXP; M212b).
const char* decal_lit_frag();
/// The terrain in the normal pass: strata normals into RG, the map's normal
/// maps (bicubic) into BA (M212e).
const char* terrain_normal_frag();
/// The normal decals in the normal pass (DecalsNormalsPS; M212e).
const char* decal_normal_frag();
/// Glowing decals, added into the frame's glow (DecalsPSGlow; M212d).
const char* decal_glow_frag();
/// Glow-mask decals, lit and setting the glow to 0.01 (DecalsGlowMaskPS; M212d).
const char* decal_glow_mask_frag();
extern const char* splat_vert; // runtime splats: a quad on the terrain (SplatsVS; M212c)
/// Runtime splats, lit as the terrain is with no specular (SplatsPS; M212c).
const char* splat_frag();
extern const char* shadow_vert;       // terrain shadow (lightVP * position)
extern const char* shadow_mesh_vert;  // mesh shadow (blend-weight skinning + lightVP)
extern const char* shadow_unit_vert;  // cube shadow (instanced + lightVP)
extern const char* shadow_frag;       // empty (depth-only write)
extern const char* shadow_mesh_frag;  // mesh shadows, cut by the albedo's alpha (M211j)
extern const char* ui_vert;           // 2D UI quad (pixel coords → NDC)
extern const char* ui_frag;           // 2D UI quad (texture * color)
extern const char* particle_vert;     // FA's particle quad (M214c: WorldVS)
extern const char* particle_frag;     // particle.fx's WorldPS: texture × ramp
extern const char* particle_refract_frag; // WorldRefractPS: the frame, displaced (M214d)
extern const char* beam_vert;         // FA's beam strip (M214a)
extern const char* beam_frag;         // particle.fx's BeamPS: texture × colour
extern const char* trail_vert;        // FA's trail ribbon (M214b)
extern const char* trail_frag;        // particle.fx's TrailPS: ramp × repeat within its life
extern const char* bloom_bright_vert;     // fullscreen triangle (no VBO)
extern const char* bloom_bright_frag;     // brightness extraction
extern const char* bloom_blur_frag;       // separable Gaussian blur
extern const char* bloom_composite_frag;  // additive composite
} // namespace shaders

} // namespace osc::renderer
