#include "renderer/stencil_volume.hpp"

#include "renderer/range_overlays.hpp"

#include <spdlog/spdlog.h>

#include <array>
#include <cstring>

namespace osc::renderer::stencil_volume {

// range.fx's vertexShader: a circle's point scaled by the radius its
// weights pick, about the ring's centre; its height the volume's bottom or
// top (the template holds 0 or 1)
const char* const kCastVert = R"glsl(
#version 450

layout(push_constant) uniform PushConstants {
    mat4 viewProj;
    vec2 heights; // bottom, top
} pc;

layout(location = 0) in vec3 inVertex;
layout(location = 1) in vec2 inCoeff;  // inner, outer weights
layout(location = 2) in vec2 inCentre; // x, z
layout(location = 3) in vec2 inRadius; // inner, outer

void main() {
    float r = inCoeff.x * inRadius.x + inCoeff.y * inRadius.y;
    vec3 p = vec3(inVertex.x * r + inCentre.x, mix(pc.heights.x, pc.heights.y, inVertex.y),
                  inVertex.z * r + inCentre.y);
    gl_Position = pc.viewProj * vec4(p, 1.0);
}
)glsl";

const char* const kCastFrag = R"glsl(
#version 450

layout(location = 0) out vec4 outColor;

void main() {
    outColor = vec4(0.0);
}
)glsl";

// frame.fx's full-screen passes: one colour (RangePS)
const char* const kScreenVert = R"glsl(
#version 450

void main() {
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)glsl";

const char* const kScreenFrag = R"glsl(
#version 450

layout(push_constant) uniform PushConstants {
    vec4 color;
} pc;

layout(location = 0) out vec4 outColor;

void main() {
    outColor = pc.color;
}
)glsl";

VkStencilOpState stencil(VkCompareOp compare, u32 compare_mask, u32 reference, u32 write_mask,
                         VkStencilOp fail, VkStencilOp pass, VkStencilOp depth_fail) {
    VkStencilOpState s{};
    s.failOp = fail;
    s.passOp = pass;
    s.depthFailOp = depth_fail;
    s.compareOp = compare;
    s.compareMask = compare_mask;
    s.writeMask = write_mask;
    s.reference = reference;
    return s;
}

VkPipeline make_pipeline(VkDevice device, VkRenderPass render_pass, VkPipelineLayout layout,
                         VkShaderModule vert, VkShaderModule frag, const PassState& state) {
    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";

    // The volume's vertices (binding 0), and a ring per instance (binding 1)
    const std::array<VkVertexInputBindingDescription, 2> bindings = {{
        {0, sizeof(RingVertex), VK_VERTEX_INPUT_RATE_VERTEX},
        {1, sizeof(RangeRing), VK_VERTEX_INPUT_RATE_INSTANCE},
    }};
    const std::array<VkVertexInputAttributeDescription, 4> attrs = {{
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(RingVertex, x)},
        {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(RingVertex, inner)},
        {2, 1, VK_FORMAT_R32G32_SFLOAT, offsetof(RangeRing, x)},
        {3, 1, VK_FORMAT_R32G32_SFLOAT, offsetof(RangeRing, inner)},
    }};
    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    if (state.cast && state.ring_input) {
        vi.vertexBindingDescriptionCount = static_cast<u32>(bindings.size());
        vi.pVertexBindingDescriptions = bindings.data();
        vi.vertexAttributeDescriptionCount = static_cast<u32>(attrs.size());
        vi.pVertexAttributeDescriptions = attrs.data();
    }
    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    const std::array<VkDynamicState, 2> dyn_states = {VK_DYNAMIC_STATE_VIEWPORT,
                                                      VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = static_cast<u32>(dyn_states.size());
    dyn.pDynamicStates = dyn_states.data();
    // Both faces (Cast's two passes, one per facing, in one), unless culled
    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = state.cull;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth.depthTestEnable = state.cast ? VK_TRUE : VK_FALSE;
    depth.depthWriteEnable = VK_FALSE;
    depth.depthCompareOp = VK_COMPARE_OP_LESS;
    depth.stencilTestEnable = VK_TRUE;
    depth.front = state.front;
    depth.back = state.back;
    VkPipelineColorBlendAttachmentState att{};
    att.blendEnable = state.blend ? VK_TRUE : VK_FALSE;
    att.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    att.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    att.colorBlendOp = VK_BLEND_OP_ADD;
    att.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    att.alphaBlendOp = VK_BLEND_OP_ADD;
    att.colorWriteMask = state.write;
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &att;

    VkGraphicsPipelineCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    ci.stageCount = static_cast<u32>(stages.size());
    ci.pStages = stages.data();
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &raster;
    ci.pMultisampleState = &ms;
    ci.pDepthStencilState = &depth;
    ci.pColorBlendState = &blend;
    ci.pDynamicState = &dyn;
    ci.layout = layout;
    ci.renderPass = render_pass;
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline) != VK_SUCCESS)
        spdlog::error("stencil volume: pipeline creation failed");
    return pipeline;
}

VkPipelineLayout make_layout(VkDevice device, VkShaderStageFlags stages, u32 size) {
    VkPushConstantRange push{};
    push.stageFlags = stages;
    push.size = size;
    VkPipelineLayoutCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    ci.pushConstantRangeCount = 1;
    ci.pPushConstantRanges = &push;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    vkCreatePipelineLayout(device, &ci, nullptr, &layout);
    return layout;
}

void* make_buffer(VmaAllocator allocator, VkBufferUsageFlags usage, size_t size,
                  AllocatedBuffer& out, const void* data) {
    VkBufferCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    ci.size = size;
    ci.usage = usage;
    VmaAllocationCreateInfo alloc{};
    alloc.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
    alloc.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
    alloc.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(allocator, &ci, &alloc, &out.buffer, &out.allocation, &info) != VK_SUCCESS)
        return nullptr;
    if (data) std::memcpy(info.pMappedData, data, size);
    return info.pMappedData;
}

} // namespace osc::renderer::stencil_volume
