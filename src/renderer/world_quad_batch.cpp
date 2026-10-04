#include "renderer/world_quad_batch.hpp"
#include "renderer/vk_cmd.hpp"

#include "renderer/shader_utils.hpp"

#include <spdlog/spdlog.h>

#include <cstddef>
#include <cstring>

namespace osc::renderer {

void WorldQuadBatch::init(VkDevice device, VmaAllocator allocator, VkRenderPass render_pass,
                          VkDescriptorSetLayout texture_ds_layout, u32 max_quads,
                          const char* name) {
    max_quads_ = max_quads;
    for (u32 i = 0; i < FRAMES_IN_FLIGHT; ++i) {
        VkBufferCreateInfo buf_ci{};
        buf_ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buf_ci.size = sizeof(Vertex) * 6 * max_quads;
        buf_ci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        VmaAllocationCreateInfo alloc_ci{};
        alloc_ci.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
        alloc_ci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
        alloc_ci.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        VmaAllocationInfo info{};
        vmaCreateBuffer(allocator, &buf_ci, &alloc_ci, &vertex_buf_[i].buffer,
                        &vertex_buf_[i].allocation, &info);
        vertex_mapped_[i] = info.pMappedData;
    }

    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    push.size = sizeof(f32) * 16;
    VkPipelineLayoutCreateInfo layout_ci{};
    layout_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_ci.setLayoutCount = 1;
    layout_ci.pSetLayouts = &texture_ds_layout;
    layout_ci.pushConstantRangeCount = 1;
    layout_ci.pPushConstantRanges = &push;
    vkCreatePipelineLayout(device, &layout_ci, nullptr, &layout_);

    // primbatcher.fx's CommandPS is the beam's: texture × colour
    VkShaderModule vert = compile_glsl(device, shaders::beam_vert, "world_quad_vert", true);
    VkShaderModule frag = compile_glsl(device, shaders::beam_frag, "world_quad_frag", false);
    if (!vert || !frag) {
        spdlog::error("{}: shader compilation failed", name);
        if (vert) {
            vkDestroyShaderModule(device, vert, nullptr);
        }
        if (frag) {
            vkDestroyShaderModule(device, frag, nullptr);
        }
        return;
    }

    VkVertexInputBindingDescription bind{};
    bind.stride = sizeof(Vertex);
    bind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    const std::array<VkVertexInputAttributeDescription, 3> attrs = {{
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, pos)},
        {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv)},
        {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, color)},
    }};
    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &bind;
    vi.vertexAttributeDescriptionCount = static_cast<u32>(attrs.size());
    vi.pVertexAttributeDescriptions = attrs.data();
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
    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    // Depth_TCommand: no depth test, so no ground or unit hides a quad
    VkPipelineDepthStencilStateCreateInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth.depthTestEnable = VK_FALSE;
    depth.depthWriteEnable = VK_FALSE;
    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";
    // SrcAlpha / InvSrcAlpha, RGB written alone
    VkPipelineColorBlendAttachmentState att{};
    att.blendEnable = VK_TRUE;
    att.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    att.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    att.colorBlendOp = VK_BLEND_OP_ADD;
    att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    att.alphaBlendOp = VK_BLEND_OP_ADD;
    att.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
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
    ci.layout = layout_;
    ci.renderPass = render_pass;
    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline_) !=
        VK_SUCCESS) {
        spdlog::error("{}: pipeline creation failed", name);
    }
    vkDestroyShaderModule(device, vert, nullptr);
    vkDestroyShaderModule(device, frag, nullptr);
}

void WorldQuadBatch::upload(const std::vector<Quad>& quads, u32 fi) {
    groups_.clear();
    auto* out = static_cast<Vertex*>(vertex_mapped_[fi]);
    if (!out) {
        return;
    }
    u32 n = 0;
    for (const Quad& q : quads) {
        if (n / 6 >= max_quads_) {
            break;
        }
        if (groups_.empty() || groups_.back().ds != q.ds) {
            groups_.push_back({q.ds, n, 0});
        }
        std::memcpy(out + n, q.v.data(), sizeof(Vertex) * q.v.size());
        n += static_cast<u32>(q.v.size());
        groups_.back().vertex_count += static_cast<u32>(q.v.size());
    }
}

void WorldQuadBatch::render(VkCommandBuffer cmd, u32 viewport_w, u32 viewport_h,
                            const f32* view_proj, u32 fi) const {
    if (groups_.empty() || !pipeline_ || !vertex_buf_[fi].buffer) {
        return;
    }
    VkViewport viewport{};
    viewport.width = static_cast<f32>(viewport_w);
    viewport.height = static_cast<f32>(viewport_h);
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    VkRect2D scissor{};
    scissor.extent = {viewport_w, viewport_h};
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertex_buf_[fi].buffer, &offset);
    vkc::bind_pipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkc::push_constants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(f32) * 16, view_proj);
    for (const Group& g : groups_) {
        vkc::bind_descriptor_sets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &g.ds, 0,
                                  nullptr);
        vkc::draw(cmd, g.vertex_count, 1, g.first_vertex, 0);
    }
}

void WorldQuadBatch::destroy(VkDevice device, VmaAllocator allocator) {
    if (pipeline_) {
        vkDestroyPipeline(device, pipeline_, nullptr);
    }
    pipeline_ = VK_NULL_HANDLE;
    if (layout_) {
        vkDestroyPipelineLayout(device, layout_, nullptr);
    }
    layout_ = VK_NULL_HANDLE;
    for (u32 i = 0; i < FRAMES_IN_FLIGHT; ++i) {
        if (vertex_buf_[i].buffer) {
            vmaDestroyBuffer(allocator, vertex_buf_[i].buffer, vertex_buf_[i].allocation);
        }
        vertex_buf_[i] = {};
        vertex_mapped_[i] = nullptr;
    }
}

} // namespace osc::renderer
