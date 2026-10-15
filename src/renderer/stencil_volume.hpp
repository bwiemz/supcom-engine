#pragma once

// What Moho's stencil-volume passes share (range.fx's Cast, vision.fx's
// CastVision, frame.fx's full-screen passes): a ring volume (an annular
// cylinder, range_overlays' ring_volume_*) cast once per ring instance into
// the stencil against the depth so far, then full-screen passes of one
// colour that read it. The range rings (RangeRenderer) and the fog of war
// (VisionRenderer) are both drawn so.

#include "core/types.hpp"
#include "renderer/vk_types.hpp"

#include <vulkan/vulkan.h>

#include <cstddef>

namespace osc::renderer::stencil_volume {

/// A volume pass's constants: the view-projection and the volume's bottom
/// and top heights (the template's 0 and 1).
struct CastPush {
    f32 view_proj[16];
    f32 heights[2];
};

/// The volume's vertex shader: a circle's point scaled by the radius its
/// weights pick, about the ring's centre (range.fx's vertexShader;
/// vision.fx's visionVertexShader is the same with no inner radius).
extern const char* const kCastVert;
/// Writes nothing (colour writes are off).
extern const char* const kCastFrag;
/// A triangle over the whole target, for frame.fx's passes.
extern const char* const kScreenVert;
/// One colour, the push constant's (frame.fx's RangePS, VisionPS).
extern const char* const kScreenFrag;

VkStencilOpState stencil(VkCompareOp compare, u32 compare_mask, u32 reference, u32 write_mask,
                         VkStencilOp fail, VkStencilOp pass, VkStencilOp depth_fail);

struct PassState {
    bool cast = false; ///< the volumes (vertex input, depth test), else full screen
    VkStencilOpState front{}, back{};
    VkColorComponentFlags write = 0;
    bool blend = false; ///< SrcAlpha, InvSrcAlpha
};

/// A pass's pipeline in `render_pass`: both faces drawn (a cast's two
/// passes, one per facing, in one), viewport and scissor dynamic.
VkPipeline make_pipeline(VkDevice device, VkRenderPass render_pass, VkPipelineLayout layout,
                         VkShaderModule vert, VkShaderModule frag, const PassState& state);

/// A layout of one push-constant range of `size` bytes.
VkPipelineLayout make_layout(VkDevice device, VkShaderStageFlags stages, u32 size);

/// A mapped host-visible buffer of `size` bytes, with `data` in it if given
void* make_buffer(VmaAllocator allocator, VkBufferUsageFlags usage, size_t size,
                  AllocatedBuffer& out, const void* data = nullptr);

} // namespace osc::renderer::stencil_volume
