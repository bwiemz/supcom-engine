#include "renderer/sky_renderer.hpp"

#include "map/terrain.hpp"
#include "renderer/camera.hpp"
#include "renderer/shader_utils.hpp"
#include "renderer/sky_dome.hpp"
#include "renderer/texture_cache.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace osc::renderer {

namespace {

/// The descriptor bindings, as sky.fx names its textures.
enum Binding : u32 {
    kUniforms,
    kHorizonLookup, ///< point, clamped
    kCirrus,        ///< linear, wrapped
    kDecalAlbedo,   ///< linear, clamped
    kDecalGlow,
    kBindingCount
};

/// Moho's (SkyDome's constructor): the lookup no map names.
constexpr const char* kHorizonLookupPath = "/textures/environment/horizonLookup.dds";

/// The billboard's corners (kDecalBillboardQuadVertices) and its two
/// triangles (kDecalQuadIndices).
constexpr std::array<f32, 8> kQuadCorners = {-1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f, -1.0f};
constexpr std::array<u16, 6> kQuadIndices = {0, 1, 2, 2, 1, 3};
constexpr VkDeviceSize kQuadIndexOffset = sizeof(kQuadCorners);

VkSampler make_sampler(VkDevice device, VkFilter filter, VkSamplerAddressMode mode) {
    VkSamplerCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    ci.magFilter = filter;
    ci.minFilter = filter;
    // sky.fx's point sampler has no mips (MipFilter = NONE)
    ci.mipmapMode =
        filter == VK_FILTER_LINEAR ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.addressModeU = mode;
    ci.addressModeV = mode;
    ci.addressModeW = mode;
    ci.maxLod = filter == VK_FILTER_LINEAR ? VK_LOD_CLAMP_NONE : 0.0f;
    VkSampler s = VK_NULL_HANDLE;
    vkCreateSampler(device, &ci, nullptr, &s);
    return s;
}

AllocatedBuffer mapped_buffer(VmaAllocator allocator, VkDeviceSize size, VkBufferUsageFlags usage,
                              void** mapped) {
    VkBufferCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    ci.size = size;
    ci.usage = usage;
    VmaAllocationCreateInfo alloc_ci{};
    alloc_ci.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
    alloc_ci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
    alloc_ci.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    AllocatedBuffer buf{};
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(allocator, &ci, &alloc_ci, &buf.buffer, &buf.allocation, &info) !=
        VK_SUCCESS)
        return {};
    *mapped = info.pMappedData;
    return buf;
}

/// A buffer holding `bytes`, filled.
AllocatedBuffer filled_buffer(VmaAllocator allocator, const void* bytes, VkDeviceSize size,
                              VkBufferUsageFlags usage) {
    void* mapped = nullptr;
    AllocatedBuffer buf = mapped_buffer(allocator, size, usage, &mapped);
    if (mapped) std::memcpy(mapped, bytes, size);
    return buf;
}

} // namespace

void SkyRenderer::init(VkDevice device, VmaAllocator allocator, VkRenderPass scene_pass) {
    device_ = device;
    allocator_ = allocator;
    point_ = make_sampler(device, VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    wrap_ = make_sampler(device, VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_REPEAT);
    clamp_ = make_sampler(device, VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    for (u32 i = 0; i < FRAMES_IN_FLIGHT; ++i)
        uniform_buf_[i] = mapped_buffer(allocator, sizeof(Uniforms),
                                        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &uniform_mapped_[i]);
    // The quad's corners, then its indices, in one buffer
    std::array<u8, sizeof(kQuadCorners) + sizeof(kQuadIndices)> quad{};
    std::memcpy(quad.data(), kQuadCorners.data(), sizeof(kQuadCorners));
    std::memcpy(quad.data() + kQuadIndexOffset, kQuadIndices.data(), sizeof(kQuadIndices));
    quad_buf_ = filled_buffer(allocator, quad.data(), quad.size(),
                              VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT);

    std::array<VkDescriptorSetLayoutBinding, kBindingCount> bindings{};
    for (u32 b = 0; b < kBindingCount; ++b) {
        bindings[b].binding = b;
        bindings[b].descriptorCount = 1;
        bindings[b].descriptorType = b == kUniforms ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                                    : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[b].stageFlags = b == kUniforms
                                     ? VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT
                                     : VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo set_ci{};
    set_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    set_ci.bindingCount = static_cast<u32>(bindings.size());
    set_ci.pBindings = bindings.data();
    vkCreateDescriptorSetLayout(device, &set_ci, nullptr, &set_layout_);

    const std::array<VkDescriptorPoolSize, 2> sizes = {{
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, FRAMES_IN_FLIGHT},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, FRAMES_IN_FLIGHT * (kBindingCount - 1)},
    }};
    VkDescriptorPoolCreateInfo pool_ci{};
    pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_ci.maxSets = FRAMES_IN_FLIGHT;
    pool_ci.poolSizeCount = static_cast<u32>(sizes.size());
    pool_ci.pPoolSizes = sizes.data();
    vkCreateDescriptorPool(device, &pool_ci, nullptr, &pool_);
    std::array<VkDescriptorSetLayout, FRAMES_IN_FLIGHT> layouts{};
    layouts.fill(set_layout_);
    VkDescriptorSetAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool = pool_;
    alloc.descriptorSetCount = FRAMES_IN_FLIGHT;
    alloc.pSetLayouts = layouts.data();
    vkAllocateDescriptorSets(device, &alloc, sets_.data());

    VkPipelineLayoutCreateInfo layout_ci{};
    layout_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_ci.setLayoutCount = 1;
    layout_ci.pSetLayouts = &set_layout_;
    vkCreatePipelineLayout(device, &layout_ci, nullptr, &layout_);

    VkShaderModule dome_vert =
        compile_glsl(device, shaders::sky_dome_vert(), "sky_dome_vert", true);
    VkShaderModule decal_vert =
        compile_glsl(device, shaders::sky_decal_vert(), "sky_decal_vert", true);
    VkShaderModule atmosphere =
        compile_glsl(device, shaders::sky_atmosphere_frag(), "sky_atmosphere_frag", false);
    VkShaderModule cirrus =
        compile_glsl(device, shaders::sky_cirrus_frag(), "sky_cirrus_frag", false);
    VkShaderModule albedo =
        compile_glsl(device, shaders::sky_decal_albedo_frag, "sky_decal_albedo_frag", false);
    VkShaderModule glow =
        compile_glsl(device, shaders::sky_decal_glow_frag(), "sky_decal_glow_frag", false);
    const std::array<VkShaderModule, 6> modules = {dome_vert, decal_vert, atmosphere,
                                                   cirrus,    albedo,     glow};
    if (std::any_of(modules.begin(), modules.end(), [](VkShaderModule m) { return !m; })) {
        spdlog::error("SkyRenderer: shader compilation failed");
        for (VkShaderModule m : modules)
            if (m) vkDestroyShaderModule(device, m, nullptr);
        return;
    }

    // The dome's vertex: its position and azimuth
    const VkVertexInputBindingDescription dome_bind{0, sizeof(SkyDomeVertex),
                                                    VK_VERTEX_INPUT_RATE_VERTEX};
    const std::array<VkVertexInputAttributeDescription, 2> dome_attrs = {{
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(SkyDomeVertex, pos)},
        {1, 0, VK_FORMAT_R32_SFLOAT, offsetof(SkyDomeVertex, theta)},
    }};
    // The decal's: the quad's corner, then its instance
    const std::array<VkVertexInputBindingDescription, 2> decal_binds = {{
        {0, sizeof(f32) * 2, VK_VERTEX_INPUT_RATE_VERTEX},
        {1, sizeof(DecalInstance), VK_VERTEX_INPUT_RATE_INSTANCE},
    }};
    const std::array<VkVertexInputAttributeDescription, 4> decal_attrs = {{
        {0, 0, VK_FORMAT_R32G32_SFLOAT, 0},
        {1, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(DecalInstance, position)},
        {2, 1, VK_FORMAT_R32G32_SFLOAT, offsetof(DecalInstance, size)},
        {3, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(DecalInstance, uv)},
    }};

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
    // CullMode = CW: clockwise triangles on screen are culled
    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_BACK_BIT;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    // ZEnable = false, ZWriteEnable = false
    VkPipelineDepthStencilStateCreateInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth.depthCompareOp = VK_COMPARE_OP_ALWAYS;

    constexpr VkColorComponentFlags kRGB =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
    const auto build = [&](VkShaderModule vert, VkShaderModule frag, bool decal, bool blended,
                           VkColorComponentFlags write, VkPipeline& out) {
        std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vert;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = frag;
        stages[1].pName = "main";
        VkPipelineVertexInputStateCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        if (decal) {
            vi.vertexBindingDescriptionCount = static_cast<u32>(decal_binds.size());
            vi.pVertexBindingDescriptions = decal_binds.data();
            vi.vertexAttributeDescriptionCount = static_cast<u32>(decal_attrs.size());
            vi.pVertexAttributeDescriptions = decal_attrs.data();
        } else {
            vi.vertexBindingDescriptionCount = 1;
            vi.pVertexBindingDescriptions = &dome_bind;
            vi.vertexAttributeDescriptionCount = static_cast<u32>(dome_attrs.size());
            vi.pVertexAttributeDescriptions = dome_attrs.data();
        }
        // AlphaBlend_SrcAlpha_InvSrcAlpha, or AlphaBlend_Disable
        VkPipelineColorBlendAttachmentState att{};
        att.blendEnable = blended ? VK_TRUE : VK_FALSE;
        att.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        att.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        att.colorBlendOp = VK_BLEND_OP_ADD;
        att.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        att.alphaBlendOp = VK_BLEND_OP_ADD;
        att.colorWriteMask = write;
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
        ci.renderPass = scene_pass;
        if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &ci, nullptr, &out) != VK_SUCCESS)
            spdlog::error("SkyRenderer: pipeline creation failed");
    };
    // Atmosphere: AlphaBlend_Disable_Write_RGB
    build(dome_vert, atmosphere, false, false, kRGB, atmosphere_pipeline_);
    // Cirrus: AlphaBlend_SrcAlpha_InvSrcAlpha_Write_RGB
    build(dome_vert, cirrus, false, true, kRGB, cirrus_pipeline_);
    // Decal P0 (albedo), as Cirrus; P1 (glow): AlphaBlend_Disable_Write_A
    build(decal_vert, albedo, true, true, kRGB, decal_albedo_pipeline_);
    build(decal_vert, glow, true, false, VK_COLOR_COMPONENT_A_BIT, decal_glow_pipeline_);
    for (VkShaderModule m : modules) vkDestroyShaderModule(device, m, nullptr);
}

void SkyRenderer::build(const map::Terrain& terrain, TextureCache& textures) {
    if (!device_) return;
    vkDeviceWaitIdle(device_);
    clear();
    const map::ScmapSky& sky = terrain.sky();

    // The dome
    const SkyDomeMesh dome = build_sky_dome(sky);
    if (dome.indices.empty()) {
        spdlog::warn("SkyRenderer: the map's dome ({} x {}) can't be drawn", sky.width, sky.height);
        return;
    }
    dome_vertices_ = filled_buffer(allocator_, dome.vertices.data(),
                                   sizeof(SkyDomeVertex) * dome.vertices.size(),
                                   VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    dome_indices_ =
        filled_buffer(allocator_, dome.indices.data(), sizeof(u16) * dome.indices.size(),
                      VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    if (!dome_vertices_.buffer || !dome_indices_.buffer) {
        free_buffers();
        return;
    }

    // The textures: the decals draw only with both of theirs
    const auto load = [&](const std::string& path) -> VkImageView {
        const GPUTexture* tex = path.empty() ? nullptr : textures.get_blocking(path);
        return tex ? tex->image.view : VK_NULL_HANDLE;
    };
    horizon_lookup_ = load(kHorizonLookupPath);
    if (!horizon_lookup_) horizon_lookup_ = textures.fallback_view();
    cirrus_ = load(sky.cirrus_texture);
    if (!cirrus_) cirrus_ = textures.zero_fallback_view();
    decal_albedo_ = load(sky.decal_albedo);
    decal_glow_ = load(sky.decal_glow_texture);
    const u32 decals = std::min<u32>(static_cast<u32>(sky.decals.size()), MAX_DECALS);
    if (decals > 0 && decal_albedo_ && decal_glow_) {
        std::vector<DecalInstance> instances;
        instances.reserve(decals);
        for (u32 i = 0; i < decals; ++i) {
            const map::ScmapSkyDecal& d = sky.decals[i];
            instances.push_back({{d.position[0], d.position[1], d.position[2], d.rotation},
                                 {d.size[0], d.size[1]},
                                 {d.uv[0], d.uv[1], d.uv[2], d.uv[3]}});
        }
        decal_buf_ =
            filled_buffer(allocator_, instances.data(), sizeof(DecalInstance) * instances.size(),
                          VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        if (decal_buf_.buffer) decal_count_ = decals;
    }
    if (!decal_albedo_) decal_albedo_ = textures.zero_fallback_view();
    if (!decal_glow_) decal_glow_ = textures.zero_fallback_view();

    // The parameters (RenderAtmosphere, RenderCirrus, RenderDecals)
    params_.view_up[3] = sky.decal_glow;
    params_.horizon[0] = sky.elevation;
    params_.horizon[1] = sky.elevation + sky.horizon_size;
    params_.horizon[2] = sky.cirrus_multiplier;
    std::copy(std::begin(sky.horizon_color), std::end(sky.horizon_color), params_.horizon_color);
    std::copy(std::begin(sky.sky_color), std::end(sky.sky_color), params_.sky_color);
    std::copy(std::begin(sky.cirrus_color), std::end(sky.cirrus_color), params_.cirrus_color);
    for (size_t i = 0; i < std::size(sky.cirrus); ++i) {
        const map::ScmapCirrusLayer& layer = sky.cirrus[i];
        f32* out = params_.cirrus_layer[i];
        out[0] = layer.frequency[0];
        out[1] = layer.frequency[1];
        out[2] = layer.direction[0];
        out[3] = layer.direction[1];
        params_.cirrus_speed[i] = layer.speed;
    }
    index_count_ = static_cast<u32>(dome.indices.size());
    write_sets();
}

void SkyRenderer::write_sets() {
    if (!pool_) return;
    const std::array<std::pair<VkImageView, VkSampler>, kBindingCount> images = {{
        {VK_NULL_HANDLE, VK_NULL_HANDLE},
        {horizon_lookup_, point_},
        {cirrus_, wrap_},
        {decal_albedo_, clamp_},
        {decal_glow_, clamp_},
    }};
    for (u32 f = 0; f < FRAMES_IN_FLIGHT; ++f) {
        VkDescriptorBufferInfo buffer{uniform_buf_[f].buffer, 0, sizeof(Uniforms)};
        std::array<VkDescriptorImageInfo, kBindingCount> infos{};
        std::array<VkWriteDescriptorSet, kBindingCount> writes{};
        for (u32 b = 0; b < kBindingCount; ++b) {
            writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[b].dstSet = sets_[f];
            writes[b].dstBinding = b;
            writes[b].descriptorCount = 1;
            if (b == kUniforms) {
                writes[b].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                writes[b].pBufferInfo = &buffer;
            } else {
                infos[b] = {images[b].second, images[b].first,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                writes[b].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                writes[b].pImageInfo = &infos[b];
            }
        }
        vkUpdateDescriptorSets(device_, static_cast<u32>(writes.size()), writes.data(), 0, nullptr);
    }
}

void SkyRenderer::update(const Camera& camera, const std::array<f32, 16>& view_proj, u32 tick,
                         f32 interpolant, u32 fi) {
    if (!has_sky() || !uniform_mapped_[fi]) return;
    std::copy(view_proj.begin(), view_proj.end(), params_.view_proj);
    // The view's right and up (its matrix's first two rows), for the billboards
    const std::array<f32, 16> view = camera.view();
    for (int c = 0; c < 3; ++c) {
        params_.view_right[c] = view[static_cast<size_t>(c) * 4];
        params_.view_up[c] = view[static_cast<size_t>(c) * 4 + 1];
    }
    // sky.fx's time: the tick plus the interpolant
    params_.view_right[3] = static_cast<f32>(static_cast<f64>(tick) + interpolant);
    std::memcpy(uniform_mapped_[fi], &params_, sizeof(Uniforms));
}

void SkyRenderer::record(VkCommandBuffer cmd, u32 fi) const {
    if (!has_sky() || !atmosphere_pipeline_) return;
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &sets_[fi], 0,
                            nullptr);
    const VkDeviceSize zero = 0;
    const auto draw_dome = [&](VkPipeline pipeline) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdBindVertexBuffers(cmd, 0, 1, &dome_vertices_.buffer, &zero);
        vkCmdBindIndexBuffer(cmd, dome_indices_.buffer, 0, VK_INDEX_TYPE_UINT16);
        vkCmdDrawIndexed(cmd, index_count_, 1, 0, 0, 0);
    };
    // RenderAtmosphere, RenderDecals (albedo, then glow), RenderCirrus
    draw_dome(atmosphere_pipeline_);
    if (decal_count_ > 0) {
        const std::array<VkBuffer, 2> buffers = {quad_buf_.buffer, decal_buf_.buffer};
        const std::array<VkDeviceSize, 2> offsets = {0, 0};
        for (VkPipeline pipeline : {decal_albedo_pipeline_, decal_glow_pipeline_}) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            vkCmdBindVertexBuffers(cmd, 0, 2, buffers.data(), offsets.data());
            vkCmdBindIndexBuffer(cmd, quad_buf_.buffer, kQuadIndexOffset, VK_INDEX_TYPE_UINT16);
            vkCmdDrawIndexed(cmd, static_cast<u32>(kQuadIndices.size()), decal_count_, 0, 0, 0);
        }
    }
    draw_dome(cirrus_pipeline_);
}

void SkyRenderer::free_buffers() {
    for (AllocatedBuffer* b : {&dome_vertices_, &dome_indices_, &decal_buf_}) {
        if (b->buffer) vmaDestroyBuffer(allocator_, b->buffer, b->allocation);
        *b = {};
    }
}

void SkyRenderer::clear() {
    free_buffers();
    index_count_ = 0;
    decal_count_ = 0;
    params_ = {};
    horizon_lookup_ = cirrus_ = decal_albedo_ = decal_glow_ = VK_NULL_HANDLE;
}

void SkyRenderer::destroy(VkDevice device, VmaAllocator allocator) {
    clear();
    for (VkPipeline* p : {&atmosphere_pipeline_, &cirrus_pipeline_, &decal_albedo_pipeline_,
                          &decal_glow_pipeline_}) {
        if (*p) vkDestroyPipeline(device, *p, nullptr);
        *p = VK_NULL_HANDLE;
    }
    if (layout_) vkDestroyPipelineLayout(device, layout_, nullptr);
    layout_ = VK_NULL_HANDLE;
    if (pool_) vkDestroyDescriptorPool(device, pool_, nullptr);
    pool_ = VK_NULL_HANDLE;
    if (set_layout_) vkDestroyDescriptorSetLayout(device, set_layout_, nullptr);
    set_layout_ = VK_NULL_HANDLE;
    for (VkSampler* s : {&point_, &wrap_, &clamp_}) {
        if (*s) vkDestroySampler(device, *s, nullptr);
        *s = VK_NULL_HANDLE;
    }
    for (u32 i = 0; i < FRAMES_IN_FLIGHT; ++i) {
        if (uniform_buf_[i].buffer)
            vmaDestroyBuffer(allocator, uniform_buf_[i].buffer, uniform_buf_[i].allocation);
        uniform_buf_[i] = {};
        uniform_mapped_[i] = nullptr;
    }
    if (quad_buf_.buffer) vmaDestroyBuffer(allocator, quad_buf_.buffer, quad_buf_.allocation);
    quad_buf_ = {};
}

} // namespace osc::renderer
