#include "renderer/particle_renderer.hpp"

#include "renderer/shader_utils.hpp"
#include "renderer/texture_cache.hpp"

#include <cstring>
#include <spdlog/spdlog.h>

namespace osc::renderer {

// Push constants: mat4 viewProj (64B) + vec3 camRight + pad + vec3 camUp + pad = 96B
struct ParticlePushConstants {
    f32 view_proj[16];
    f32 cam_right[3];
    f32 _pad0;
    f32 cam_up[3];
    f32 _pad1;
};
static_assert(sizeof(ParticlePushConstants) == 96);

void ParticleRenderer::init(VkDevice device, VmaAllocator allocator,
                            VkRenderPass render_pass,
                            VkDescriptorSetLayout texture_ds_layout,
                            VkSampler /*sampler*/) {
    device_ = device;

    // Instance buffer (per-frame, persistently mapped)
    for (u32 i = 0; i < FRAMES_IN_FLIGHT; ++i) {
        VkBufferCreateInfo buf_ci{};
        buf_ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buf_ci.size = sizeof(ParticleInstance) * MAX_PARTICLES;
        buf_ci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;

        VmaAllocationCreateInfo alloc_ci{};
        alloc_ci.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
        alloc_ci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
        alloc_ci.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

        VmaAllocationInfo info{};
        vmaCreateBuffer(allocator, &buf_ci, &alloc_ci,
                        &instance_buf_[i].buffer,
                        &instance_buf_[i].allocation, &info);
        instance_mapped_[i] = info.pMappedData;
    }

    // Pipeline layout (push constants + 1 texture descriptor set)
    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    push_range.offset = 0;
    push_range.size = sizeof(ParticlePushConstants);

    VkPipelineLayoutCreateInfo layout_ci{};
    layout_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_ci.setLayoutCount = 1;
    layout_ci.pSetLayouts = &texture_ds_layout;
    layout_ci.pushConstantRangeCount = 1;
    layout_ci.pPushConstantRanges = &push_range;
    vkCreatePipelineLayout(device, &layout_ci, nullptr, &layout_);

    // Compile shaders
    VkShaderModule vert_mod = compile_glsl(device, shaders::particle_vert,
                                           "particle_vert", true);
    VkShaderModule frag_mod = compile_glsl(device, shaders::particle_frag,
                                           "particle_frag", false);
    if (!vert_mod || !frag_mod) {
        spdlog::error("ParticleRenderer: shader compilation failed");
        if (vert_mod) vkDestroyShaderModule(device, vert_mod, nullptr);
        if (frag_mod) vkDestroyShaderModule(device, frag_mod, nullptr);
        return;
    }

    // Vertex input: no per-vertex data, all per-instance
    // ParticleInstance layout: pos(3f) + size(1f) + rotation(1f) + alpha(1f)
    //                        + uvOffset(2f) + uvSize(2f) + color(3f) = 13 floats = 52B
    VkVertexInputBindingDescription bind{};
    bind.binding = 0;
    bind.stride = sizeof(ParticleInstance);
    bind.inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;

    VkVertexInputAttributeDescription attrs[7] = {};
    // location 0: pos (vec3)
    attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(ParticleInstance, pos_x)};
    // location 1: size (float)
    attrs[1] = {1, 0, VK_FORMAT_R32_SFLOAT, offsetof(ParticleInstance, size)};
    // location 2: rotation (float)
    attrs[2] = {2, 0, VK_FORMAT_R32_SFLOAT, offsetof(ParticleInstance, rotation)};
    // location 3: alpha (float)
    attrs[3] = {3, 0, VK_FORMAT_R32_SFLOAT, offsetof(ParticleInstance, alpha)};
    // location 4: uvOffset (vec2)
    attrs[4] = {4, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(ParticleInstance, uv_x)};
    // location 5: uvSize (vec2)
    attrs[5] = {5, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(ParticleInstance, uv_w)};
    // location 6: color (vec3)
    attrs[6] = {6, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(ParticleInstance, r)};

    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount = 1;
    vertex_input.pVertexBindingDescriptions = &bind;
    vertex_input.vertexAttributeDescriptionCount = 7;
    vertex_input.pVertexAttributeDescriptions = attrs;

    // Input assembly
    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    // Viewport/scissor (dynamic)
    VkPipelineViewportStateCreateInfo vp_state{};
    vp_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp_state.viewportCount = 1;
    vp_state.scissorCount = 1;

    VkDynamicState dyn_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dyn_states;

    // Rasterizer
    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE; // billboards are double-sided
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;

    // Multisampling
    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Depth — read but don't write (particles behind terrain are culled,
    // but particles don't occlude each other)
    VkPipelineDepthStencilStateCreateInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth.depthTestEnable = VK_TRUE;
    depth.depthWriteEnable = VK_FALSE;
    depth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    // Shader stages
    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert_mod;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag_mod;
    stages[1].pName = "main";

    // Alpha blend pipeline
    {
        VkPipelineColorBlendAttachmentState blend_att{};
        blend_att.blendEnable = VK_TRUE;
        blend_att.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blend_att.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend_att.colorBlendOp = VK_BLEND_OP_ADD;
        blend_att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend_att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend_att.alphaBlendOp = VK_BLEND_OP_ADD;
        blend_att.colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
                                   VK_COLOR_COMPONENT_G_BIT |
                                   VK_COLOR_COMPONENT_B_BIT |
                                   VK_COLOR_COMPONENT_A_BIT;

        VkPipelineColorBlendStateCreateInfo blend{};
        blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        blend.attachmentCount = 1;
        blend.pAttachments = &blend_att;

        VkGraphicsPipelineCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        ci.stageCount = 2;
        ci.pStages = stages;
        ci.pVertexInputState = &vertex_input;
        ci.pInputAssemblyState = &ia;
        ci.pViewportState = &vp_state;
        ci.pRasterizationState = &raster;
        ci.pMultisampleState = &ms;
        ci.pDepthStencilState = &depth;
        ci.pColorBlendState = &blend;
        ci.pDynamicState = &dyn;
        ci.layout = layout_;
        ci.renderPass = render_pass;
        ci.subpass = 0;

        VkResult res = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1,
                                                   &ci, nullptr, &alpha_pipeline_);
        if (res != VK_SUCCESS) {
            spdlog::error("ParticleRenderer: alpha pipeline creation failed");
        }
    }

    // Additive blend pipeline (same but dst += src * alpha)
    {
        VkPipelineColorBlendAttachmentState blend_att{};
        blend_att.blendEnable = VK_TRUE;
        blend_att.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blend_att.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blend_att.colorBlendOp = VK_BLEND_OP_ADD;
        blend_att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend_att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend_att.alphaBlendOp = VK_BLEND_OP_ADD;
        blend_att.colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
                                   VK_COLOR_COMPONENT_G_BIT |
                                   VK_COLOR_COMPONENT_B_BIT |
                                   VK_COLOR_COMPONENT_A_BIT;

        VkPipelineColorBlendStateCreateInfo blend{};
        blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        blend.attachmentCount = 1;
        blend.pAttachments = &blend_att;

        VkGraphicsPipelineCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        ci.stageCount = 2;
        ci.pStages = stages;
        ci.pVertexInputState = &vertex_input;
        ci.pInputAssemblyState = &ia;
        ci.pViewportState = &vp_state;
        ci.pRasterizationState = &raster;
        ci.pMultisampleState = &ms;
        ci.pDepthStencilState = &depth;
        ci.pColorBlendState = &blend;
        ci.pDynamicState = &dyn;
        ci.layout = layout_;
        ci.renderPass = render_pass;
        ci.subpass = 0;

        VkResult res = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1,
                                                   &ci, nullptr, &additive_pipeline_);
        if (res != VK_SUCCESS) {
            spdlog::error("ParticleRenderer: additive pipeline creation failed");
        }
    }

    vkDestroyShaderModule(device, vert_mod, nullptr);
    vkDestroyShaderModule(device, frag_mod, nullptr);

    spdlog::debug("ParticleRenderer: initialized (max {} particles)",
                  MAX_PARTICLES);
}

void ParticleRenderer::update(const std::vector<ParticleInstance>& instances,
                              const ParticleSystem& /*psys*/,
                              TextureCache& tex_cache, u32 fi) {
    instance_count_ = static_cast<u32>(instances.size());
    if (instance_count_ == 0) {
        groups_.clear();
        draw_count_ = 0;
        return;
    }

    // Cap to MAX_PARTICLES
    if (instance_count_ > MAX_PARTICLES) {
        instance_count_ = MAX_PARTICLES;
    }

    // Upload instance data
    if (instance_mapped_[fi]) {
        std::memcpy(instance_mapped_[fi], instances.data(),
                    instance_count_ * sizeof(ParticleInstance));
    }

    // For now, single draw group using fallback texture
    // (full per-emitter texture grouping in a future pass)
    groups_.clear();
    DrawGroup group;
    group.texture_ds = tex_cache.fallback_descriptor();
    group.instance_offset = 0;
    group.instance_count = instance_count_;
    group.additive = false;
    groups_.push_back(group);
    draw_count_ = instance_count_;
}

void ParticleRenderer::render(VkCommandBuffer cmd,
                              u32 /*viewport_w*/, u32 /*viewport_h*/,
                              const f32* view_proj,
                              const f32* cam_right, const f32* cam_up,
                              u32 fi) {
    if (instance_count_ == 0 || groups_.empty()) return;
    if (!alpha_pipeline_) return;

    // Push constants
    ParticlePushConstants pc{};
    std::memcpy(pc.view_proj, view_proj, 64);
    std::memcpy(pc.cam_right, cam_right, 12);
    std::memcpy(pc.cam_up, cam_up, 12);

    VkBuffer vbufs[] = {instance_buf_[fi].buffer};
    VkDeviceSize offsets[] = {0};

    for (const auto& group : groups_) {
        // Bind appropriate pipeline
        VkPipeline pipeline = group.additive ? additive_pipeline_ : alpha_pipeline_;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

        vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT,
                           0, sizeof(ParticlePushConstants), &pc);

        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                layout_, 0, 1, &group.texture_ds,
                                0, nullptr);

        vkCmdBindVertexBuffers(cmd, 0, 1, vbufs, offsets);

        // 6 vertices per quad (generated in shader), instanced
        vkCmdDraw(cmd, 6, group.instance_count, 0, group.instance_offset);
    }
}

void ParticleRenderer::destroy(VkDevice device, VmaAllocator allocator) {
    for (u32 i = 0; i < FRAMES_IN_FLIGHT; ++i) {
        if (instance_buf_[i].buffer) {
            vmaDestroyBuffer(allocator, instance_buf_[i].buffer,
                             instance_buf_[i].allocation);
        }
    }

    if (alpha_pipeline_) vkDestroyPipeline(device, alpha_pipeline_, nullptr);
    if (additive_pipeline_) vkDestroyPipeline(device, additive_pipeline_, nullptr);
    if (layout_) vkDestroyPipelineLayout(device, layout_, nullptr);
}

} // namespace osc::renderer
