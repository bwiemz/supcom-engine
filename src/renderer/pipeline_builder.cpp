#include "renderer/pipeline_builder.hpp"

#include <spdlog/spdlog.h>

#include <array>

namespace osc::renderer {

PipelineBuilder& PipelineBuilder::set_shaders(VkShaderModule vert,
                                              VkShaderModule frag) {
    vert_ = vert;
    frag_ = frag;
    return *this;
}

PipelineBuilder& PipelineBuilder::set_vertex_input(
    const VkVertexInputBindingDescription* bindings, uint32_t binding_count,
    const VkVertexInputAttributeDescription* attrs, uint32_t attr_count) {
    bindings_ = bindings;
    binding_count_ = binding_count;
    attrs_ = attrs;
    attr_count_ = attr_count;
    return *this;
}

PipelineBuilder& PipelineBuilder::set_topology(VkPrimitiveTopology topo) {
    topology_ = topo;
    return *this;
}

PipelineBuilder& PipelineBuilder::set_cull_mode(VkCullModeFlags cull,
                                                VkFrontFace front) {
    cull_mode_ = cull;
    front_face_ = front;
    return *this;
}

PipelineBuilder& PipelineBuilder::set_depth_test(bool test, bool write) {
    depth_test_ = test;
    depth_write_ = write;
    return *this;
}

PipelineBuilder& PipelineBuilder::set_blend(bool enable) {
    blend_ = enable;
    return *this;
}

PipelineBuilder& PipelineBuilder::set_push_constant(
    uint32_t size, VkShaderStageFlags stages) {
    push_constant_size_ = size;
    push_constant_stages_ = stages;
    return *this;
}

PipelineBuilder& PipelineBuilder::set_descriptor_set_layout(
    VkDescriptorSetLayout layout) {
    ds_layouts_.clear();
    ds_layouts_.push_back(layout);
    return *this;
}

PipelineBuilder& PipelineBuilder::add_descriptor_set_layout(
    VkDescriptorSetLayout layout) {
    ds_layouts_.push_back(layout);
    return *this;
}

VkPipeline PipelineBuilder::build(VkDevice device, VkRenderPass render_pass,
                                  VkPipelineLayout* out_layout) {
    // Shader stages
    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert_;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag_;
    stages[1].pName = "main";

    // Vertex input
    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType =
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount = binding_count_;
    vertex_input.pVertexBindingDescriptions = bindings_;
    vertex_input.vertexAttributeDescriptionCount = attr_count_;
    vertex_input.pVertexAttributeDescriptions = attrs_;

    // Input assembly
    VkPipelineInputAssemblyStateCreateInfo input_asm{};
    input_asm.sType =
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_asm.topology = topology_;

    // Dynamic viewport + scissor
    VkPipelineViewportStateCreateInfo viewport_state{};
    viewport_state.sType =
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;

    std::array<VkDynamicState, 2> dyn_states = {VK_DYNAMIC_STATE_VIEWPORT,
                                                 VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn_state{};
    dyn_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn_state.dynamicStateCount = static_cast<uint32_t>(dyn_states.size());
    dyn_state.pDynamicStates = dyn_states.data();

    // Rasterizer
    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.lineWidth = 1.0f;
    raster.cullMode = cull_mode_;
    raster.frontFace = front_face_;

    // Multisampling (off)
    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Depth stencil
    VkPipelineDepthStencilStateCreateInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth.depthTestEnable = depth_test_ ? VK_TRUE : VK_FALSE;
    depth.depthWriteEnable = depth_write_ ? VK_TRUE : VK_FALSE;
    depth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    // Color blend
    VkPipelineColorBlendAttachmentState blend_att{};
    blend_att.colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
                               VK_COLOR_COMPONENT_G_BIT |
                               VK_COLOR_COMPONENT_B_BIT |
                               VK_COLOR_COMPONENT_A_BIT;
    if (blend_) {
        blend_att.blendEnable = VK_TRUE;
        blend_att.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blend_att.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend_att.colorBlendOp = VK_BLEND_OP_ADD;
        blend_att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend_att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        blend_att.alphaBlendOp = VK_BLEND_OP_ADD;
    }

    VkPipelineColorBlendStateCreateInfo blend_state{};
    blend_state.sType =
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend_state.attachmentCount = 1;
    blend_state.pAttachments = &blend_att;

    // Pipeline layout
    VkPipelineLayoutCreateInfo layout_ci{};
    layout_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;

    if (!ds_layouts_.empty()) {
        layout_ci.setLayoutCount = static_cast<uint32_t>(ds_layouts_.size());
        layout_ci.pSetLayouts = ds_layouts_.data();
    }

    VkPushConstantRange push_range{};
    if (push_constant_size_ > 0) {
        push_range.stageFlags = push_constant_stages_;
        push_range.offset = 0;
        push_range.size = push_constant_size_;
        layout_ci.pushConstantRangeCount = 1;
        layout_ci.pPushConstantRanges = &push_range;
    }

    VkPipelineLayout layout = VK_NULL_HANDLE;
    if (vkCreatePipelineLayout(device, &layout_ci, nullptr, &layout) !=
        VK_SUCCESS) {
        spdlog::error("Failed to create pipeline layout");
        return VK_NULL_HANDLE;
    }

    // Create pipeline
    VkGraphicsPipelineCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    ci.stageCount = static_cast<uint32_t>(stages.size());
    ci.pStages = stages.data();
    ci.pVertexInputState = &vertex_input;
    ci.pInputAssemblyState = &input_asm;
    ci.pViewportState = &viewport_state;
    ci.pRasterizationState = &raster;
    ci.pMultisampleState = &ms;
    ci.pDepthStencilState = &depth;
    ci.pColorBlendState = &blend_state;
    ci.pDynamicState = &dyn_state;
    ci.layout = layout;
    ci.renderPass = render_pass;
    ci.subpass = 0;

    VkPipeline pipeline = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &ci, nullptr,
                                  &pipeline) != VK_SUCCESS) {
        spdlog::error("Failed to create graphics pipeline");
        vkDestroyPipelineLayout(device, layout, nullptr);
        return VK_NULL_HANDLE;
    }

    *out_layout = layout;
    return pipeline;
}

} // namespace osc::renderer
