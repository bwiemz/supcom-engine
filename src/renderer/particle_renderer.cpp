#include "renderer/particle_renderer.hpp"

#include "renderer/shader_utils.hpp"
#include "renderer/texture_cache.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>

namespace osc::renderer {

namespace {

/// particle.fx's TRamp blends (REFRACT is drawn apart): all write RGB only,
/// as the trails' do.
struct Blend {
    VkBlendFactor src, dst;
};
constexpr std::array<Blend, 5> kBlends = {{
    {VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA},           // ALPHABLEND
    {VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR},                // MODULATEINVERSE
    {VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR}, // MODULATE2XINVERSE
    {VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE},                           // ADD
    {VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA},                 // PREMODALPHA
}};

} // namespace

void ParticleRenderer::init(VkDevice device, VmaAllocator allocator, VkRenderPass render_pass,
                            VkDescriptorSetLayout texture_ds_layout) {
    device_ = device;
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
        vmaCreateBuffer(allocator, &buf_ci, &alloc_ci, &instance_buf_[i].buffer,
                        &instance_buf_[i].allocation, &info);
        instance_mapped_[i] = info.pMappedData;
    }

    // Set 0 the particle texture, set 1 its ramp.
    const std::array<VkDescriptorSetLayout, 2> set_layouts = {texture_ds_layout, texture_ds_layout};
    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    push.size = sizeof(f32) * 16;
    VkPipelineLayoutCreateInfo layout_ci{};
    layout_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_ci.setLayoutCount = static_cast<u32>(set_layouts.size());
    layout_ci.pSetLayouts = set_layouts.data();
    layout_ci.pushConstantRangeCount = 1;
    layout_ci.pPushConstantRanges = &push;
    vkCreatePipelineLayout(device, &layout_ci, nullptr, &layout_);
    // The refracting ones' (M214d): set 2 the frame behind them.
    const std::array<VkDescriptorSetLayout, 3> refract_sets = {texture_ds_layout, texture_ds_layout,
                                                               texture_ds_layout};
    layout_ci.setLayoutCount = static_cast<u32>(refract_sets.size());
    layout_ci.pSetLayouts = refract_sets.data();
    vkCreatePipelineLayout(device, &layout_ci, nullptr, &refract_layout_);

    // BackgroundSampler: linear, clamped.
    VkSamplerCreateInfo sampler_ci{};
    sampler_ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_ci.magFilter = VK_FILTER_LINEAR;
    sampler_ci.minFilter = VK_FILTER_LINEAR;
    sampler_ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(device, &sampler_ci, nullptr, &background_sampler_);
    const VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo pool_ci{};
    pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_ci.maxSets = 1;
    pool_ci.poolSizeCount = 1;
    pool_ci.pPoolSizes = &pool_size;
    vkCreateDescriptorPool(device, &pool_ci, nullptr, &background_pool_);
    VkDescriptorSetAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool = background_pool_;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &texture_ds_layout;
    vkAllocateDescriptorSets(device, &alloc, &background_set_);

    VkShaderModule vert = compile_glsl(device, shaders::particle_vert, "particle_vert", true);
    VkShaderModule frag = compile_glsl(device, shaders::particle_frag, "particle_frag", false);
    VkShaderModule refract =
        compile_glsl(device, shaders::particle_refract_frag, "particle_refract_frag", false);
    if (!vert || !frag || !refract) {
        spdlog::error("ParticleRenderer: shader compilation failed");
        for (VkShaderModule m : {vert, frag, refract})
            if (m) vkDestroyShaderModule(device, m, nullptr);
        return;
    }

    // All per instance: the quad's corners come from gl_VertexIndex.
    VkVertexInputBindingDescription bind{};
    bind.stride = sizeof(ParticleInstance);
    bind.inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
    const std::array<VkVertexInputAttributeDescription, 5> attrs = {{
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(ParticleInstance, center)},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(ParticleInstance, axis_x)},
        {2, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(ParticleInstance, axis_y)},
        {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(ParticleInstance, uv)},
        {4, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(ParticleInstance, ramp)},
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
    raster.cullMode = VK_CULL_MODE_NONE; // Rasterizer_Cull_None
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    // Depth_Enable_Less_Write_None
    VkPipelineDepthStencilStateCreateInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth.depthTestEnable = VK_TRUE;
    depth.depthWriteEnable = VK_FALSE;
    depth.depthCompareOp = VK_COMPARE_OP_LESS;
    // A TRamp technique's pipeline: `frag` blended src/dst, colour only.
    const auto make = [&](VkShaderModule fragment, VkPipelineLayout layout, const Blend& b) {
        std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vert;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragment;
        stages[1].pName = "main";
        VkPipelineColorBlendAttachmentState att{};
        att.blendEnable = VK_TRUE;
        att.srcColorBlendFactor = b.src;
        att.dstColorBlendFactor = b.dst;
        att.colorBlendOp = VK_BLEND_OP_ADD;
        att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        att.alphaBlendOp = VK_BLEND_OP_ADD;
        // The frame's alpha is its glow (M211e): particles leave it be.
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
        ci.layout = layout;
        ci.renderPass = render_pass;
        VkPipeline pipeline = VK_NULL_HANDLE;
        if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline) !=
            VK_SUCCESS)
            spdlog::error("ParticleRenderer: pipeline creation failed");
        return pipeline;
    };
    for (size_t i = 0; i < kBlends.size(); ++i) pipelines_[i] = make(frag, layout_, kBlends[i]);
    // TRamp_REFRACT and its kin: AlphaBlend_SrcAlpha_InvSrcAlpha_Write_RGB.
    refract_pipeline_ = make(refract, refract_layout_,
                             {VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA});
    for (VkShaderModule m : {vert, frag, refract}) vkDestroyShaderModule(device, m, nullptr);
}

void ParticleRenderer::update(const ParticleSystem& particles, TextureCache& tex_cache, u32 fi) {
    groups_.clear();
    draw_count_ = 0;
    const auto& instances = particles.instances();
    const auto count = static_cast<u32>(std::min<size_t>(instances.size(), MAX_PARTICLES));
    if (count == 0 || !instance_mapped_[fi]) return;
    std::memcpy(instance_mapped_[fi], instances.data(), sizeof(ParticleInstance) * count);

    // A texture still loading skips its run this frame; an emitter that
    // names none draws with white.
    const auto set_of = [&](const std::string& path) -> VkDescriptorSet {
        if (path.empty()) return tex_cache.fallback_descriptor();
        const GPUTexture* tex = tex_cache.get(path);
        return tex ? tex->descriptor_set : VK_NULL_HANDLE;
    };
    for (const ParticleSystem::Group& run : particles.groups()) {
        if (run.offset >= count) break;
        VkDescriptorSet texture = set_of(run.texture);
        VkDescriptorSet ramp = set_of(run.ramp);
        if (!texture || !ramp) continue;
        const u32 n = std::min(run.count, count - run.offset);
        groups_.push_back({run.under_water, run.blendmode, texture, ramp, run.offset, n});
        draw_count_ += n;
    }
}

void ParticleRenderer::render(VkCommandBuffer cmd, u32 viewport_w, u32 viewport_h,
                              const f32* view_proj, bool under_water, u32 fi) const {
    if (groups_.empty() || !instance_buf_[fi].buffer) return;
    VkViewport viewport{};
    viewport.width = static_cast<f32>(viewport_w);
    viewport.height = static_cast<f32>(viewport_h);
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    VkRect2D scissor{};
    scissor.extent = {viewport_w, viewport_h};
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &instance_buf_[fi].buffer, &offset);
    i32 bound = -1;
    for (const Group& g : groups_) {
        if (g.under_water != under_water || g.blendmode == kBlendRefract) continue;
        const auto blend = static_cast<size_t>(g.blendmode);
        if (blend >= pipelines_.size() || !pipelines_[blend]) continue;
        if (g.blendmode != bound) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines_[blend]);
            vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(f32) * 16,
                               view_proj);
            bound = g.blendmode;
        }
        const std::array<VkDescriptorSet, 2> sets = {g.texture, g.ramp};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0,
                                static_cast<u32>(sets.size()), sets.data(), 0, nullptr);
        vkCmdDraw(cmd, 6, g.count, 0, g.offset);
    }
}

void ParticleRenderer::set_background(VkImageView view) {
    if (!background_set_ || !view) return;
    VkDescriptorImageInfo info{};
    info.sampler = background_sampler_;
    info.imageView = view;
    info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = background_set_;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &info;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    background_ready_ = true;
}

bool ParticleRenderer::refracting() const {
    return background_ready_ && refract_pipeline_ &&
           std::any_of(groups_.begin(), groups_.end(),
                       [](const Group& g) { return g.blendmode == kBlendRefract; });
}

void ParticleRenderer::render_refracting(VkCommandBuffer cmd, u32 viewport_w, u32 viewport_h,
                                         const f32* view_proj, u32 fi) const {
    if (!refracting() || !instance_buf_[fi].buffer) return;
    VkViewport viewport{};
    viewport.width = static_cast<f32>(viewport_w);
    viewport.height = static_cast<f32>(viewport_h);
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    VkRect2D scissor{};
    scissor.extent = {viewport_w, viewport_h};
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &instance_buf_[fi].buffer, &offset);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, refract_pipeline_);
    vkCmdPushConstants(cmd, refract_layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(f32) * 16,
                       view_proj);
    for (const Group& g : groups_) {
        if (g.blendmode != kBlendRefract) continue;
        const std::array<VkDescriptorSet, 3> sets = {g.texture, g.ramp, background_set_};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, refract_layout_, 0,
                                static_cast<u32>(sets.size()), sets.data(), 0, nullptr);
        vkCmdDraw(cmd, 6, g.count, 0, g.offset);
    }
}

void ParticleRenderer::destroy(VkDevice device, VmaAllocator allocator) {
    if (refract_pipeline_) vkDestroyPipeline(device, refract_pipeline_, nullptr);
    refract_pipeline_ = VK_NULL_HANDLE;
    if (refract_layout_) vkDestroyPipelineLayout(device, refract_layout_, nullptr);
    refract_layout_ = VK_NULL_HANDLE;
    if (background_pool_) vkDestroyDescriptorPool(device, background_pool_, nullptr);
    background_pool_ = VK_NULL_HANDLE;
    background_set_ = VK_NULL_HANDLE;
    background_ready_ = false;
    if (background_sampler_) vkDestroySampler(device, background_sampler_, nullptr);
    background_sampler_ = VK_NULL_HANDLE;
    for (VkPipeline& p : pipelines_) {
        if (p) vkDestroyPipeline(device, p, nullptr);
        p = VK_NULL_HANDLE;
    }
    if (layout_) vkDestroyPipelineLayout(device, layout_, nullptr);
    layout_ = VK_NULL_HANDLE;
    for (u32 i = 0; i < FRAMES_IN_FLIGHT; ++i) {
        if (instance_buf_[i].buffer)
            vmaDestroyBuffer(allocator, instance_buf_[i].buffer, instance_buf_[i].allocation);
        instance_buf_[i] = {};
        instance_mapped_[i] = nullptr;
    }
}

} // namespace osc::renderer
