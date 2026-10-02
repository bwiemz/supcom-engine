#include "renderer/water_renderer.hpp"
#include "renderer/vk_cmd.hpp"

#include "map/terrain.hpp"
#include "renderer/camera.hpp"
#include "renderer/shader_utils.hpp"
#include "renderer/texture_cache.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace osc::renderer {

namespace {

/// The descriptor bindings, as water2.fx names its textures.
enum Binding : u32 {
    kUniforms,
    kSky,
    kNormal0,
    kNormal1,
    kNormal2,
    kNormal3,
    kRefraction,
    kReflection,
    kFresnel,
    kWaterMap,     ///< UtilitySamplerC: linear
    kWaterMapMask, ///< MaskSampler: point
    kBindingCount
};

/// Where the water map is missing, the map's default masks (CWldMap):
/// foam 0, flatness 255.
constexpr u8 kNoFoam = 0;
constexpr u8 kFullFlatness = 255;

VkSampler make_sampler(VkDevice device, VkFilter filter, VkSamplerAddressMode mode) {
    VkSamplerCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    ci.magFilter = filter;
    ci.minFilter = filter;
    ci.mipmapMode =
        filter == VK_FILTER_LINEAR ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.addressModeU = mode;
    ci.addressModeV = mode;
    ci.addressModeW = mode;
    ci.maxLod = VK_LOD_CLAMP_NONE;
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
    vmaCreateBuffer(allocator, &ci, &alloc_ci, &buf.buffer, &buf.allocation, &info);
    *mapped = info.pMappedData;
    return buf;
}

} // namespace

void WaterRenderer::init(VkDevice device, VmaAllocator allocator, VkRenderPass scene_pass) {
    device_ = device;
    allocator_ = allocator;
    wrap_ = make_sampler(device, VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_REPEAT);
    clamp_ = make_sampler(device, VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    point_ = make_sampler(device, VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    for (u32 i = 0; i < FRAMES_IN_FLIGHT; ++i)
        uniform_buf_[i] = mapped_buffer(allocator, sizeof(Uniforms),
                                        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &uniform_mapped_[i]);
    vertex_buf_ = mapped_buffer(allocator, sizeof(Vertex) * 4, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                &vertex_mapped_);
    index_buf_ =
        mapped_buffer(allocator, sizeof(u16) * 6, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, &index_mapped_);

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

    VkShaderModule vert = compile_glsl(device, shaders::water_vert, "water_vert", true);
    VkShaderModule surface = compile_glsl(device, shaders::water_frag, "water_frag", false);
    VkShaderModule mask = compile_glsl(device, shaders::water_mask_frag, "water_mask_frag", false);
    VkShaderModule low0 = compile_glsl(device, shaders::water_low_frag0, "water_low_frag0", false);
    VkShaderModule low1 = compile_glsl(device, shaders::water_low_frag1, "water_low_frag1", false);
    if (!vert || !surface || !mask || !low0 || !low1) {
        spdlog::error("WaterRenderer: shader compilation failed");
        for (VkShaderModule m : {vert, surface, mask, low0, low1})
            if (m) vkDestroyShaderModule(device, m, nullptr);
        return;
    }

    VkVertexInputBindingDescription bind{};
    bind.stride = sizeof(Vertex);
    bind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    const std::array<VkVertexInputAttributeDescription, 2> attrs = {{
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, pos)},
        {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv)},
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

    const auto build = [&](VkShaderModule frag, VkCompareOp compare, VkColorComponentFlags write,
                           VkPipeline& out, bool blended = false) {
        std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vert;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = frag;
        stages[1].pName = "main";
        VkPipelineDepthStencilStateCreateInfo depth{};
        depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depth.depthTestEnable = VK_TRUE;
        depth.depthWriteEnable = VK_FALSE;
        depth.depthCompareOp = compare;
        VkPipelineColorBlendAttachmentState att{};
        att.blendEnable = blended ? VK_TRUE : VK_FALSE;
        att.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        att.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        att.colorBlendOp = VK_BLEND_OP_ADD;
        att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
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
            spdlog::error("WaterRenderer: pipeline creation failed");
    };
    // TWaterLayAlphaMask: alpha only, Depth_Enable_Less_Write_None.
    build(mask, VK_COMPARE_OP_LESS, VK_COLOR_COMPONENT_A_BIT, mask_pipeline_);
    // Water_HighFidelity: RGB, no blend, Depth_Enable_LessEqual_Write_None.
    build(surface, VK_COMPARE_OP_LESS_OR_EQUAL,
          VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT,
          surface_pipeline_);
    // Water_LowFidelity: both passes SrcAlpha / InvSrcAlpha into RGB
    // (ColorWriteEnable 0x07), Depth LessEqual, unwritten.
    const VkColorComponentFlags rgb =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
    build(low0, VK_COMPARE_OP_LESS_OR_EQUAL, rgb, low_pipelines_[0], true);
    build(low1, VK_COMPARE_OP_LESS_OR_EQUAL, rgb, low_pipelines_[1], true);
    for (VkShaderModule m : {vert, surface, mask, low0, low1})
        vkDestroyShaderModule(device, m, nullptr);
}

void WaterRenderer::build(const map::Terrain& terrain, TextureCache& textures) {
    clear();
    if (!terrain.has_water() || !vertex_mapped_) return;
    has_water_ = true;
    water_elevation_ = terrain.water_elevation();
    const map::ScmapWater& w = terrain.water();
    const map::Heightmap& hm = terrain.heightmap();
    const u32 width = hm.map_width();
    const u32 height = hm.map_height();

    // InitVerts: a quad over the map, UV 0-1 across it.
    const auto fw = static_cast<f32>(width);
    const auto fh = static_cast<f32>(height);
    const std::array<Vertex, 4> verts = {{
        {{0, water_elevation_, 0}, {0, 0}},
        {{fw, water_elevation_, 0}, {1, 0}},
        {{0, water_elevation_, fh}, {0, 1}},
        {{fw, water_elevation_, fh}, {1, 1}},
    }};
    std::memcpy(vertex_mapped_, verts.data(), sizeof(verts));
    const std::array<u16, 6> indices = {0, 2, 1, 1, 2, 3};
    std::memcpy(index_mapped_, indices.data(), sizeof(indices));

    // The water map (RebuildWaterMapRect): half the map's size, each texel
    // two heightmap units across.
    water_map_w_ = width / 2;
    water_map_h_ = height / 2;
    water_map_.assign(static_cast<size_t>(water_map_w_) * water_map_h_ * 4, 0);
    const map::ScmapWaterMasks& masks = terrain.water_masks();
    const bool has_masks = masks.width == water_map_w_ && masks.height == water_map_h_;
    const f32 abyss = terrain.water_abyss_elevation();
    const f32 span = water_elevation_ - abyss;
    const auto sample = [&](u32 x, u32 z) {
        return hm.get_height_at_grid(std::min(x, width), std::min(z, height));
    };
    for (u32 hz = 0; hz < water_map_h_; ++hz) {
        for (u32 hx = 0; hx < water_map_w_; ++hx) {
            const size_t i = static_cast<size_t>(hz) * water_map_w_ + hx;
            const u32 x = hx * 2;
            const u32 z = hz * 2;
            const std::array<f32, 4> corners = {sample(x, z), sample(x + 2, z), sample(x, z + 2),
                                                sample(x + 2, z + 2)};
            const bool under = std::any_of(corners.begin(), corners.end(),
                                           [&](f32 h) { return water_elevation_ > h; });
            const bool over = std::any_of(corners.begin(), corners.end(),
                                          [&](f32 h) { return water_elevation_ < h; });
            f32 depth = span > 0 ? (water_elevation_ - corners[0]) / span * 255.0f : 255.0f;
            depth = std::clamp(depth, 0.0f, 255.0f);
            u8* texel = &water_map_[i * 4];
            texel[0] = has_masks ? masks.flatness[i] : kFullFlatness;
            texel[1] = under ? static_cast<u8>(depth) : 0;
            texel[2] = over ? 255 : 0;
            texel[3] = has_masks ? masks.foam[i] : kNoFoam;
        }
    }

    // The Fresnel table (BuildFresnelLookupTexture): row the incidence,
    // column the depth.
    fresnel_.assign(static_cast<size_t>(FRESNEL_SIZE) * FRESNEL_SIZE * 4, 0);
    const f32 step = 1.0f / static_cast<f32>(FRESNEL_SIZE - 1);
    for (u32 row = 0; row < FRESNEL_SIZE; ++row) {
        const f32 incidence = static_cast<f32>(row) * step;
        for (u32 col = 0; col < FRESNEL_SIZE; ++col) {
            const f32 blend = static_cast<f32>(col) * step * w.fresnel_bias;
            const f32 fresnel = std::clamp(
                blend + (1.0f - blend) * std::pow(1.0f - incidence, w.fresnel_power), 0.0f, 1.0f);
            const f32 sun = fresnel * std::pow(incidence, w.sun_shininess) * w.sun_reflection;
            u8* texel = &fresnel_[(static_cast<size_t>(row) * FRESNEL_SIZE + col) * 4];
            texel[0] = static_cast<u8>(std::lround(fresnel * 255.0f));
            texel[1] = static_cast<u8>(std::lround(std::clamp(sun, 0.0f, 1.0f) * 255.0f));
            texel[3] = 255;
        }
    }

    const auto upload = [&](const char* key, const std::vector<u8>& pixels, u32 pw, u32 ph) {
        const GPUTexture* tex = textures.upload_rgba(key, pixels.data(), pw, ph);
        return tex ? tex->image.view : textures.fallback_view();
    };
    water_map_view_ = upload("__water_map", water_map_, water_map_w_, water_map_h_);
    fresnel_view_ = upload("__water_fresnel", fresnel_, FRESNEL_SIZE, FRESNEL_SIZE);
    for (size_t k = 0; k < normals_.size(); ++k) {
        const GPUTexture* tex =
            w.normal_texture[k].empty() ? nullptr : textures.get_blocking(w.normal_texture[k]);
        normals_[k] = tex ? tex->image.view : textures.fallback_view();
    }
    // The water ramp: the terrain's tint by depth; transparent without one.
    const GPUTexture* ramp = w.ramp.empty() ? nullptr : textures.get_blocking(w.ramp);
    ramp_view_ = ramp ? ramp->image.view : textures.zero_fallback_view();
    cube_ = w.cubemap.empty() ? VK_NULL_HANDLE : textures.get_cube_blocking(w.cubemap);
    if (!cube_) cube_ = textures.cube_fallback_view();
    // Until the renderer gives it its targets: transparent, so the sky shows.
    if (!reflection_view_) reflection_view_ = textures.zero_fallback_view();
    if (!refraction_view_) refraction_view_ = textures.zero_fallback_view();

    // The shader's parameters (RenderWaterSurface).
    params_.view_pos[3] = water_elevation_;
    params_.params[1] = w.refraction_scale;
    params_.params[2] = w.unit_reflection;
    params_.params[3] = w.sky_reflection;
    std::copy(std::begin(w.surface_color), std::end(w.surface_color), params_.water_color);
    params_.water_color[3] = w.sun_shininess;
    params_.lerp[0] = w.color_lerp[0];
    params_.lerp[1] = w.color_lerp[1];
    std::copy(std::begin(w.normal_repeat), std::end(w.normal_repeat), params_.repeat);
    params_.move01[0] = w.normal_movement[0][0];
    params_.move01[1] = w.normal_movement[0][1];
    params_.move01[2] = w.normal_movement[1][0];
    params_.move01[3] = w.normal_movement[1][1];
    params_.move23[0] = w.normal_movement[2][0];
    params_.move23[1] = w.normal_movement[2][1];
    params_.move23[2] = w.normal_movement[3][0];
    params_.move23[3] = w.normal_movement[3][1];
    std::copy(std::begin(w.sun_direction), std::end(w.sun_direction), params_.sun_dir);
    for (int c = 0; c < 3; ++c) params_.sun_color[c] = w.sun_color[c] * w.sun_reflection;
    write_sets();
}

void WaterRenderer::set_frame_images(const FrameImages& images) {
    refraction_view_ = images.refraction;
    reflection_view_ = images.reflection;
    if (has_water_) {
        write_sets();
    }
}

void WaterRenderer::write_sets() {
    if (!pool_) return;
    const std::array<std::pair<VkImageView, VkSampler>, kBindingCount> images = {{
        {VK_NULL_HANDLE, VK_NULL_HANDLE},
        {cube_, wrap_},
        {normals_[0], wrap_},
        {normals_[1], wrap_},
        {normals_[2], wrap_},
        {normals_[3], wrap_},
        {refraction_view_, clamp_},
        {reflection_view_, clamp_},
        {fresnel_view_, clamp_},
        {water_map_view_, clamp_},
        {water_map_view_, point_},
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
        vkc::update_descriptor_sets(device_, static_cast<u32>(writes.size()), writes.data(), 0,
                                    nullptr);
    }
}

void WaterRenderer::update(const Camera& camera, const std::array<f32, 16>& view_proj, f32 time,
                           u32 fi) {
    if (!has_water_ || !uniform_mapped_[fi]) return;
    std::copy(view_proj.begin(), view_proj.end(), params_.view_proj);
    camera.eye_position(params_.view_pos[0], params_.view_pos[1], params_.view_pos[2]);
    params_.params[0] = time;
    std::memcpy(uniform_mapped_[fi], &params_, sizeof(Uniforms));
}

void WaterRenderer::render_mask(VkCommandBuffer cmd, u32 viewport_w, u32 viewport_h, u32 fi) const {
    if (!has_water_ || !mask_pipeline_) return;
    VkViewport viewport{};
    viewport.width = static_cast<f32>(viewport_w);
    viewport.height = static_cast<f32>(viewport_h);
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    VkRect2D scissor{};
    scissor.extent = {viewport_w, viewport_h};
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkc::bind_pipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mask_pipeline_);
    vkc::bind_descriptor_sets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &sets_[fi], 0,
                              nullptr);
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertex_buf_.buffer, &offset);
    vkCmdBindIndexBuffer(cmd, index_buf_.buffer, 0, VK_INDEX_TYPE_UINT16);
    vkc::draw_indexed(cmd, 6, 1, 0, 0, 0);
}

void WaterRenderer::render_surface_low(VkCommandBuffer cmd, u32 viewport_w, u32 viewport_h,
                                       u32 fi) const {
    if (!has_water_ || !low_pipelines_[0] || !low_pipelines_[1]) return;
    for (VkPipeline pass : low_pipelines_) {
        bind_quad(cmd, pass, viewport_w, viewport_h, fi);
        vkc::draw_indexed(cmd, 6, 1, 0, 0, 0);
    }
}

void WaterRenderer::bind_quad(VkCommandBuffer cmd, VkPipeline pipeline, u32 viewport_w,
                              u32 viewport_h, u32 fi) const {
    VkViewport viewport{};
    viewport.width = static_cast<f32>(viewport_w);
    viewport.height = static_cast<f32>(viewport_h);
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    VkRect2D scissor{};
    scissor.extent = {viewport_w, viewport_h};
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkc::bind_pipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkc::bind_descriptor_sets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &sets_[fi], 0,
                              nullptr);
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertex_buf_.buffer, &offset);
    vkCmdBindIndexBuffer(cmd, index_buf_.buffer, 0, VK_INDEX_TYPE_UINT16);
}

void WaterRenderer::render_surface(VkCommandBuffer cmd, u32 viewport_w, u32 viewport_h,
                                   u32 fi) const {
    if (!has_water_ || !surface_pipeline_) return;
    bind_quad(cmd, surface_pipeline_, viewport_w, viewport_h, fi);
    vkc::draw_indexed(cmd, 6, 1, 0, 0, 0);
}

void WaterRenderer::clear() {
    has_water_ = false;
    water_map_.clear();
    fresnel_.clear();
    water_map_w_ = water_map_h_ = 0;
    normals_.fill(VK_NULL_HANDLE);
    cube_ = water_map_view_ = fresnel_view_ = ramp_view_ = VK_NULL_HANDLE;
}

void WaterRenderer::destroy(VkDevice device, VmaAllocator allocator) {
    clear();
    for (VkPipeline* p :
         {&mask_pipeline_, &surface_pipeline_, &low_pipelines_[0], &low_pipelines_[1]}) {
        if (*p) vkDestroyPipeline(device, *p, nullptr);
        *p = VK_NULL_HANDLE;
    }
    if (layout_) vkDestroyPipelineLayout(device, layout_, nullptr);
    layout_ = VK_NULL_HANDLE;
    if (pool_) vkDestroyDescriptorPool(device, pool_, nullptr);
    pool_ = VK_NULL_HANDLE;
    if (set_layout_) vkDestroyDescriptorSetLayout(device, set_layout_, nullptr);
    set_layout_ = VK_NULL_HANDLE;
    for (VkSampler* s : {&wrap_, &clamp_, &point_}) {
        if (*s) vkDestroySampler(device, *s, nullptr);
        *s = VK_NULL_HANDLE;
    }
    for (u32 i = 0; i < FRAMES_IN_FLIGHT; ++i) {
        if (uniform_buf_[i].buffer)
            vmaDestroyBuffer(allocator, uniform_buf_[i].buffer, uniform_buf_[i].allocation);
        uniform_buf_[i] = {};
        uniform_mapped_[i] = nullptr;
    }
    for (AllocatedBuffer* b : {&vertex_buf_, &index_buf_}) {
        if (b->buffer) vmaDestroyBuffer(allocator, b->buffer, b->allocation);
        *b = {};
    }
    vertex_mapped_ = index_mapped_ = nullptr;
    refraction_view_ = VK_NULL_HANDLE;
}

} // namespace osc::renderer
