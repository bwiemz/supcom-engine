#include "renderer/cartographic_renderer.hpp"

#include "map/terrain.hpp"
#include "renderer/shader_utils.hpp"
#include "renderer/texture_cache.hpp"
#include "renderer/vk_cmd.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace osc::renderer {

namespace {

/// The cache's names for the map's three lookups.
constexpr const char* kElevationKey = "__osc_cartographic_elevation";
constexpr const char* kHypsometricKey = "__osc_cartographic_hypsometric";
constexpr const char* kTopographicKey = "__osc_cartographic_topographic";

/// Terrain_Stage0's constants: viewMatrix x projMatrix, terrainSizeCoeff,
/// and the ground quad (its far corner, its height word, terrainHeightScale).
struct TerrainPush {
    f32 view_proj[16];
    f32 terrain_size_coeff[4];
    f32 quad[4];
};

// TerrainVS: Moho's ground quad, (0, z1) (0, 0) (x1, z1) (x1, 0) as a strip,
// its height a word times terrainHeightScale.
const char* kTerrainVert = R"(#version 450
layout(push_constant) uniform Push {
    mat4 view_proj;
    vec4 terrain_size_coeff;
    vec4 quad;
} pc;
layout(location = 0) out vec3 world;
void main() {
    vec4 p = vec4(gl_VertexIndex >= 2 ? pc.quad.x : 0.0, pc.quad.z,
                  (gl_VertexIndex & 1) == 0 ? pc.quad.y : 0.0, 1.0);
    p.y *= pc.quad.w;
    world = p.xyz;
    gl_Position = pc.view_proj * p;
}
)";

// TerrainPS0: the elevation at the point, bilinear over the half-resolution
// grid's four nearest texels (POINT taps; lerp as HLSL's a + t(b - a)),
// indexes the colour ramp, and the contour ramp for alpha. A sampler1D's
// point tap at h is texel floor(h * 256), clamped.
const char* kTerrainFrag = R"(#version 450
layout(push_constant) uniform Push {
    mat4 view_proj;
    vec4 terrain_size_coeff;
    vec4 quad;
} pc;
layout(set = 0, binding = 0) uniform sampler2D elevation;
layout(set = 0, binding = 1) uniform sampler2D hypsometric;
layout(set = 0, binding = 2) uniform sampler2D topographic;
layout(location = 0) in vec3 world;
layout(location = 0) out vec4 color;
float lerp(float a, float b, float t) { return a + t * (b - a); }
float tap(ivec2 at) {
    return texelFetch(elevation, clamp(at, ivec2(0), textureSize(elevation, 0) - 1), 0).r;
}
void main() {
    vec2 v0 = pc.terrain_size_coeff.xy * world.xz;
    vec2 t = v0 - floor(v0);
    ivec2 base = ivec2(floor(v0));
    float h0 = tap(base);
    float h1 = tap(base + ivec2(1, 0));
    float h2 = tap(base + ivec2(0, 1));
    float h3 = tap(base + ivec2(1, 1));
    float h = clamp(lerp(lerp(h0, h1, t.x), lerp(h2, h3, t.x), t.y) - 0.0001, 0.0, 1.0);
    int i = min(int(floor(h * 256.0)), 255);
    color = vec4(texelFetch(hypsometric, ivec2(i, 0), 0).rgb,
                 texelFetch(topographic, ivec2(i, 0), 0).r);
}
)";

// A triangle over the whole target.
const char* kEdgeVert = R"(#version 450
void main() {
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

// TerrainPS1: the frame's colour less saturate(16 (c - mean of the eight
// neighbours' alpha)), alpha 0; neighbours summed in Moho's order (bl, l,
// tl, t, ur, r, br, b), POINT and CLAMP. Clamped at 0 as Moho's 8-bit head
// clamps it, which the scene's float target wouldn't.
const char* kEdgeFrag = R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D frame;
layout(location = 0) out vec4 color;
float alpha_at(ivec2 at) {
    return texelFetch(frame, clamp(at, ivec2(0), textureSize(frame, 0) - 1), 0).a;
}
void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec4 c = texelFetch(frame, p, 0);
    float sum = alpha_at(p + ivec2(-1, -1)) + alpha_at(p + ivec2(-1, 0)) +
                alpha_at(p + ivec2(-1, 1)) + alpha_at(p + ivec2(0, 1)) +
                alpha_at(p + ivec2(1, 1)) + alpha_at(p + ivec2(1, 0)) +
                alpha_at(p + ivec2(1, -1)) + alpha_at(p + ivec2(0, -1));
    float topo = clamp(16.0 * (c.a - 0.125 * sum), 0.0, 1.0);
    color = vec4(clamp(c.rgb - vec3(topo), 0.0, 1.0), 0.0);
}
)";

AllocatedImage make_image(VkDevice device, VmaAllocator allocator, VkFormat format, u32 w, u32 h,
                          VkImageUsageFlags usage, VkImageAspectFlags aspect) {
    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = {w, h, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    VmaAllocationCreateInfo alloc{};
    alloc.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    AllocatedImage img{};
    if (vmaCreateImage(allocator, &ci, &alloc, &img.image, &img.allocation, nullptr) != VK_SUCCESS)
        return {};
    VkImageViewCreateInfo view{};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = img.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = format;
    view.subresourceRange = {aspect, 0, 1, 0, 1};
    if (vkCreateImageView(device, &view, nullptr, &img.view) != VK_SUCCESS) {
        vmaDestroyImage(allocator, img.image, img.allocation);
        return {};
    }
    return img;
}

void destroy_image(VkDevice device, VmaAllocator allocator, AllocatedImage& img) {
    if (img.view) vkDestroyImageView(device, img.view, nullptr);
    if (img.image) vmaDestroyImage(allocator, img.image, img.allocation);
    img = {};
}

/// A one-subpass pass drawing an 8-bit colour target (and, with `depth`,
/// a depth one), cleared, ending shader-readable for the next stage, the
/// minimap, or a capture's copy.
VkRenderPass make_pass(VkDevice device, VkFormat depth_format, bool depth) {
    std::array<VkAttachmentDescription, 2> att{};
    att[0].format = CartographicRenderer::kTargetFormat;
    att[0].samples = VK_SAMPLE_COUNT_1_BIT;
    att[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att[0].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    att[1].format = depth_format;
    att[1].samples = VK_SAMPLE_COUNT_1_BIT;
    att[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    const VkAttachmentReference color_ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    const VkAttachmentReference depth_ref{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &color_ref;
    sub.pDepthStencilAttachment = depth ? &depth_ref : nullptr;

    constexpr VkPipelineStageFlags kAttachments = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                                  VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                                  VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    std::array<VkSubpassDependency, 2> deps{};
    // In: what read or wrote the targets before (the frame's last use of
    // them, a capture's copy) is done first.
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask =
        kAttachments | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    deps[0].dstStageMask = kAttachments;
    deps[0].srcAccessMask =
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    // Out: the colour, readable by a shader or a copy.
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;

    VkRenderPassCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    ci.attachmentCount = depth ? 2u : 1u;
    ci.pAttachments = att.data();
    ci.subpassCount = 1;
    ci.pSubpasses = &sub;
    ci.dependencyCount = static_cast<u32>(deps.size());
    ci.pDependencies = deps.data();
    VkRenderPass pass = VK_NULL_HANDLE;
    if (vkCreateRenderPass(device, &ci, nullptr, &pass) != VK_SUCCESS) return VK_NULL_HANDLE;
    return pass;
}

VkDescriptorSetLayout sampler_layout(VkDevice device, u32 count) {
    std::vector<VkDescriptorSetLayoutBinding> bindings(count);
    for (u32 b = 0; b < count; ++b) {
        bindings[b].binding = b;
        bindings[b].descriptorCount = 1;
        bindings[b].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[b].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    ci.bindingCount = count;
    ci.pBindings = bindings.data();
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    vkCreateDescriptorSetLayout(device, &ci, nullptr, &layout);
    return layout;
}

void write_samplers(VkDevice device, VkDescriptorSet set, VkSampler sampler,
                    const std::vector<VkImageView>& views) {
    std::vector<VkDescriptorImageInfo> infos(views.size());
    std::vector<VkWriteDescriptorSet> writes(views.size());
    for (size_t i = 0; i < views.size(); ++i) {
        infos[i] = {sampler, views[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = static_cast<u32>(i);
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &infos[i];
    }
    vkc::update_descriptor_sets(device, static_cast<u32>(writes.size()), writes.data(), 0, nullptr);
}

} // namespace

void CartographicRenderer::init(VkDevice device, VmaAllocator allocator, VkFormat depth_format,
                                VkDescriptorSetLayout ui_layout, VkRenderPass scene_pass) {
    device_ = device;
    allocator_ = allocator;
    depth_format_ = depth_format;

    // cartographic.fx's samplers are all POINT, CLAMP; the shaders fetch
    // texels by index, which is that
    VkSamplerCreateInfo sampler{};
    sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter = VK_FILTER_NEAREST;
    sampler.minFilter = VK_FILTER_NEAREST;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(device, &sampler, nullptr, &point_);

    frame_pass_ = make_pass(device, depth_format, true);
    output_pass_ = make_pass(device, depth_format, true);
    lookup_layout_ = sampler_layout(device, 3);
    frame_layout_ = sampler_layout(device, 1);

    // The lookups' set, then each frame in flight's stage-1 input and UI
    // set, and the main view's stage-1 input
    const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                    3 + 3 * FRAMES_IN_FLIGHT};
    VkDescriptorPoolCreateInfo pool_ci{};
    pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_ci.maxSets = 1 + 3 * FRAMES_IN_FLIGHT;
    pool_ci.poolSizeCount = 1;
    pool_ci.pPoolSizes = &size;
    vkCreateDescriptorPool(device, &pool_ci, nullptr, &pool_);
    std::array<VkDescriptorSetLayout, 1 + 3 * FRAMES_IN_FLIGHT> layouts{};
    layouts[0] = lookup_layout_;
    for (u32 i = 0; i < FRAMES_IN_FLIGHT; ++i) {
        layouts[1 + i] = frame_layout_;
        layouts[1 + FRAMES_IN_FLIGHT + i] = ui_layout;
        layouts[1 + 2 * FRAMES_IN_FLIGHT + i] = frame_layout_;
    }
    std::array<VkDescriptorSet, 1 + 3 * FRAMES_IN_FLIGHT> sets{};
    VkDescriptorSetAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool = pool_;
    alloc.descriptorSetCount = static_cast<u32>(layouts.size());
    alloc.pSetLayouts = layouts.data();
    vkAllocateDescriptorSets(device, &alloc, sets.data());
    lookup_set_ = sets[0];
    for (u32 i = 0; i < FRAMES_IN_FLIGHT; ++i) {
        frame_sets_[i] = sets[1 + i];
        ui_sets_[i] = sets[1 + FRAMES_IN_FLIGHT + i];
        main_frame_sets_[i] = sets[1 + 2 * FRAMES_IN_FLIGHT + i];
    }

    const VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                   sizeof(TerrainPush)};
    VkPipelineLayoutCreateInfo layout_ci{};
    layout_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_ci.setLayoutCount = 1;
    layout_ci.pSetLayouts = &lookup_layout_;
    layout_ci.pushConstantRangeCount = 1;
    layout_ci.pPushConstantRanges = &push;
    vkCreatePipelineLayout(device, &layout_ci, nullptr, &terrain_layout_);
    layout_ci.pSetLayouts = &frame_layout_;
    layout_ci.pushConstantRangeCount = 0;
    layout_ci.pPushConstantRanges = nullptr;
    vkCreatePipelineLayout(device, &layout_ci, nullptr, &edge_layout_);

    VkShaderModule terrain_vert =
        compile_glsl(device, kTerrainVert, "cartographic_terrain_vert", true);
    VkShaderModule terrain_frag =
        compile_glsl(device, kTerrainFrag, "cartographic_terrain_frag", false);
    VkShaderModule edge_vert = compile_glsl(device, kEdgeVert, "cartographic_edge_vert", true);
    VkShaderModule edge_frag = compile_glsl(device, kEdgeFrag, "cartographic_edge_frag", false);
    const std::array<VkShaderModule, 4> modules = {terrain_vert, terrain_frag, edge_vert,
                                                   edge_frag};
    if (std::any_of(modules.begin(), modules.end(), [](VkShaderModule m) { return !m; })) {
        spdlog::error("CartographicRenderer: shader compilation failed");
        for (VkShaderModule m : modules)
            if (m) vkDestroyShaderModule(device, m, nullptr);
        return;
    }

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
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
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    // AlphaBlend_Disable_Write_RGBA
    VkPipelineColorBlendAttachmentState att{};
    att.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &att;

    const auto build = [&](VkShaderModule vert, VkShaderModule frag, VkPrimitiveTopology topology,
                           const VkPipelineDepthStencilStateCreateInfo* depth,
                           VkPipelineLayout layout, VkRenderPass pass, VkPipeline& out) {
        std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vert;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = frag;
        stages[1].pName = "main";
        VkPipelineInputAssemblyStateCreateInfo ia{};
        ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = topology;
        VkGraphicsPipelineCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        ci.stageCount = static_cast<u32>(stages.size());
        ci.pStages = stages.data();
        ci.pVertexInputState = &vi;
        ci.pInputAssemblyState = &ia;
        ci.pViewportState = &vp;
        ci.pRasterizationState = &raster;
        ci.pMultisampleState = &ms;
        ci.pDepthStencilState = depth;
        ci.pColorBlendState = &blend;
        ci.pDynamicState = &dyn;
        ci.layout = layout;
        ci.renderPass = pass;
        if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &ci, nullptr, &out) != VK_SUCCESS)
            spdlog::error("CartographicRenderer: pipeline creation failed");
    };
    // Terrain_Stage0: ZEnable, ZWriteEnable, ZFunc = always
    VkPipelineDepthStencilStateCreateInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth.depthTestEnable = VK_TRUE;
    depth.depthWriteEnable = VK_TRUE;
    depth.depthCompareOp = VK_COMPARE_OP_ALWAYS;
    build(terrain_vert, terrain_frag, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, &depth, terrain_layout_,
          frame_pass_, terrain_pipeline_);
    // Terrain_Stage1: Depth_Disable, over the output's depth (which the
    // meshes test after it); and into the scene, for a cartographic main
    // view, over the scene's
    VkPipelineDepthStencilStateCreateInfo no_depth{};
    no_depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    build(edge_vert, edge_frag, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, &no_depth, edge_layout_,
          output_pass_, edge_pipeline_);
    if (scene_pass)
        build(edge_vert, edge_frag, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, &no_depth, edge_layout_,
              scene_pass, scene_edge_pipeline_);
    for (VkShaderModule m : modules) vkDestroyShaderModule(device, m, nullptr);
}

void CartographicRenderer::build(const map::Terrain& terrain, TextureCache& textures) {
    if (!device_) return;
    vkDeviceWaitIdle(device_);
    clear(textures);
    params_ = cartographic_params(terrain);
    if (params_.grid_width < 1 || params_.grid_height < 1) return;
    const map::Heightmap& field = terrain.heightmap();
    span_x_ = field.grid_width() - 1;
    span_z_ = field.grid_height() - 1;
    surface_word_ = cartographic_surface_word(params_);

    const std::vector<u16> elevation = cartographic_elevation(terrain, params_);
    const auto hypsometric = hypsometric_ramp(params_);
    const auto topographic = topographic_ramp(params_);
    const GPUTexture* elev = textures.upload_pixels(
        kElevationKey, VK_FORMAT_R16_SFLOAT, elevation.data(), sizeof(u16),
        static_cast<u32>(params_.grid_width), static_cast<u32>(params_.grid_height));
    // ARGB words are B, G, R, A bytes
    const GPUTexture* hypso =
        textures.upload_pixels(kHypsometricKey, VK_FORMAT_B8G8R8A8_UNORM, hypsometric.data(),
                               sizeof(u32), kCartographicRampTexels, 1);
    const GPUTexture* topo = textures.upload_pixels(
        kTopographicKey, VK_FORMAT_R8_UNORM, topographic.data(), 1, kCartographicRampTexels, 1);
    if (!elev || !hypso || !topo) {
        spdlog::error("CartographicRenderer: the map's lookups couldn't be uploaded");
        return;
    }
    write_samplers(device_, lookup_set_, point_,
                   {elev->image.view, hypso->image.view, topo->image.view});
    hypsometric_ds_ = hypso->descriptor_set;
    built_ = true;
}

bool CartographicRenderer::ensure_target(Target& t, u32 width, u32 height, bool with_output,
                                         VkDescriptorSet frame_set, VkDescriptorSet ui_set) {
    if (t.width == width && t.height == height && t.frame_fb && (t.output_fb || !with_output))
        return true;
    vkDeviceWaitIdle(device_);
    destroy_target(t);
    constexpr VkImageUsageFlags kColor = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                         VK_IMAGE_USAGE_SAMPLED_BIT |
                                         VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    t.frame = make_image(device_, allocator_, kTargetFormat, width, height, kColor,
                         VK_IMAGE_ASPECT_COLOR_BIT);
    if (with_output)
        t.output = make_image(device_, allocator_, kTargetFormat, width, height, kColor,
                              VK_IMAGE_ASPECT_COLOR_BIT);
    // The view takes the stencil too where the format has one, as the
    // frame's depth does
    const VkImageAspectFlags depth_aspect =
        depth_format_ == VK_FORMAT_D32_SFLOAT
            ? VkImageAspectFlags{VK_IMAGE_ASPECT_DEPTH_BIT}
            : VkImageAspectFlags{VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT};
    t.depth = make_image(device_, allocator_, depth_format_, width, height,
                         VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, depth_aspect);
    // The UI's view of the output: alpha read as 1 (see ui_descriptor)
    if (t.output.image) {
        VkImageViewCreateInfo view{};
        view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view.image = t.output.image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = kTargetFormat;
        view.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                           VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_ONE};
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(device_, &view, nullptr, &t.ui_view) != VK_SUCCESS)
            t.ui_view = VK_NULL_HANDLE;
    }
    if (!t.frame.view || !t.depth.view || (with_output && (!t.output.view || !t.ui_view))) {
        destroy_target(t);
        return false;
    }
    VkFramebufferCreateInfo fb{};
    fb.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fb.width = width;
    fb.height = height;
    fb.layers = 1;
    const std::array<VkImageView, 2> frame_views = {t.frame.view, t.depth.view};
    fb.renderPass = frame_pass_;
    fb.attachmentCount = 2;
    fb.pAttachments = frame_views.data();
    vkCreateFramebuffer(device_, &fb, nullptr, &t.frame_fb);
    if (with_output) {
        // The frame's depth again, cleared: Stage 0 is done with it
        const std::array<VkImageView, 2> output_views = {t.output.view, t.depth.view};
        fb.renderPass = output_pass_;
        fb.attachmentCount = 2;
        fb.pAttachments = output_views.data();
        vkCreateFramebuffer(device_, &fb, nullptr, &t.output_fb);
    }
    if (!t.frame_fb || (with_output && !t.output_fb)) {
        destroy_target(t);
        return false;
    }
    write_samplers(device_, frame_set, point_, {t.frame.view});
    if (with_output) write_samplers(device_, ui_set, point_, {t.ui_view});
    t.width = width;
    t.height = height;
    return true;
}

bool CartographicRenderer::prepare(u32 fi, u32 width, u32 height) {
    if (!ready() || fi >= FRAMES_IN_FLIGHT || width == 0 || height == 0) return false;
    return ensure_target(targets_[fi], width, height, true, frame_sets_[fi], ui_sets_[fi]);
}

std::array<f32, 16> CartographicRenderer::view_proj(const View& view) const {
    // Depths over the heights and the quad, which lies at the water's
    // surface: a map flooded over its highest ground, or with its water
    // under its lowest, must not clip it away; nor the overlays' volumes
    const f32 surface = static_cast<f32>(surface_word_) / 128.0f;
    return cartographic_view_proj(view.x0, view.z0, view.x1, view.z1,
                                  std::min({params_.elev_min, surface, volume_bottom_}) - 1.0f,
                                  std::max({params_.elev_max, surface, volume_top_}) + 1.0f);
}

std::array<f32, 16> CartographicRenderer::mesh_view_proj(const View& view) const {
    const f32 surface = static_cast<f32>(surface_word_) / 128.0f;
    return cartographic_view_proj(view.x0, view.z0, view.x1, view.z1,
                                  std::min({params_.elev_min, surface, volume_bottom_}) - 1.0f,
                                  std::max({params_.elev_max, surface, volume_top_}) + 1.0f +
                                      kMeshHeadroom);
}

void CartographicRenderer::record_stage0(VkCommandBuffer cmd, const Target& t, const f32* view_proj,
                                         const Overlays& overlays) const {
    const VkViewport viewport{0.0f, 0.0f, static_cast<f32>(t.width), static_cast<f32>(t.height),
                              0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, {t.width, t.height}};
    // Into the frame: cleared to opaque black, depth 1, stencil 0
    std::array<VkClearValue, 2> clears{};
    clears[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    clears[1].depthStencil = {1.0f, 0};
    VkRenderPassBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    begin.renderPass = frame_pass_;
    begin.framebuffer = t.frame_fb;
    begin.renderArea = scissor;
    begin.clearValueCount = static_cast<u32>(clears.size());
    begin.pClearValues = clears.data();
    vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    TerrainPush push{};
    std::memcpy(push.view_proj, view_proj, sizeof(push.view_proj));
    std::memcpy(push.terrain_size_coeff, params_.terrain_size_coeff.data(),
                sizeof(push.terrain_size_coeff));
    push.quad[0] = static_cast<f32>(span_x_);
    push.quad[1] = static_cast<f32>(span_z_);
    push.quad[2] = static_cast<f32>(surface_word_);
    push.quad[3] = 1.0f / 128.0f; // terrainHeightScale
    vkc::bind_pipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, terrain_pipeline_);
    vkc::bind_descriptor_sets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, terrain_layout_, 0, 1,
                              &lookup_set_, 0, nullptr);
    vkc::push_constants(cmd, terrain_layout_,
                        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push),
                        &push);
    vkc::draw(cmd, 4, 1, 0, 0);
    // The overlays, on the quad's depth
    if (overlays) overlays(cmd, view_proj, t.width, t.height);
    vkCmdEndRenderPass(cmd);
}

void CartographicRenderer::record(VkCommandBuffer cmd, u32 fi, const View& view,
                                  const Overlays& overlays, const Meshes& meshes) {
    if (!prepare(fi, view.width, view.height)) return;
    const Target& t = targets_[fi];
    const auto vp = view_proj(view);
    record_stage0(cmd, t, vp.data(), overlays);

    // Stage 1, into the output: the contour lines
    const VkViewport viewport{0.0f, 0.0f, static_cast<f32>(t.width), static_cast<f32>(t.height),
                              0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, {t.width, t.height}};
    // Black, and a depth of 1 for the meshes (Moho clears the head's)
    std::array<VkClearValue, 2> clears{};
    clears[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    clears[1].depthStencil = {1.0f, 0};
    VkRenderPassBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    begin.renderPass = output_pass_;
    begin.framebuffer = t.output_fb;
    begin.renderArea = scissor;
    begin.clearValueCount = static_cast<u32>(clears.size());
    begin.pClearValues = clears.data();
    vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkc::bind_pipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, edge_pipeline_);
    vkc::bind_descriptor_sets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, edge_layout_, 0, 1,
                              &frame_sets_[fi], 0, nullptr);
    vkc::draw(cmd, 3, 1, 0, 0);
    // RenderMeshes, after Stage 1
    if (meshes) {
        const auto mesh_vp = mesh_view_proj(view);
        meshes(cmd, mesh_vp.data());
    }
    vkCmdEndRenderPass(cmd);
}

bool CartographicRenderer::record_main_frame(VkCommandBuffer cmd, u32 fi, u32 width, u32 height,
                                             const f32* view_proj, const Overlays& overlays) {
    if (!ready() || !scene_edge_pipeline_ || fi >= FRAMES_IN_FLIGHT || width == 0 || height == 0 ||
        !ensure_target(main_targets_[fi], width, height, false, main_frame_sets_[fi],
                       VK_NULL_HANDLE))
        return false;
    record_stage0(cmd, main_targets_[fi], view_proj, overlays);
    return true;
}

void CartographicRenderer::record_main_edge(VkCommandBuffer cmd, u32 fi) const {
    if (!scene_edge_pipeline_ || fi >= FRAMES_IN_FLIGHT || !main_targets_[fi].frame_fb) return;
    vkc::bind_pipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, scene_edge_pipeline_);
    vkc::bind_descriptor_sets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, edge_layout_, 0, 1,
                              &main_frame_sets_[fi], 0, nullptr);
    vkc::draw(cmd, 3, 1, 0, 0);
}

void CartographicRenderer::destroy_target(Target& t) {
    if (t.frame_fb) vkDestroyFramebuffer(device_, t.frame_fb, nullptr);
    if (t.output_fb) vkDestroyFramebuffer(device_, t.output_fb, nullptr);
    if (t.ui_view) vkDestroyImageView(device_, t.ui_view, nullptr);
    destroy_image(device_, allocator_, t.frame);
    destroy_image(device_, allocator_, t.output);
    destroy_image(device_, allocator_, t.depth);
    t = Target{};
}

void CartographicRenderer::clear(TextureCache& textures) {
    if (built_ || params_.grid_width > 0) {
        textures.evict(kElevationKey);
        textures.evict(kHypsometricKey);
        textures.evict(kTopographicKey);
    }
    built_ = false;
    hypsometric_ds_ = VK_NULL_HANDLE;
    params_ = {};
}

void CartographicRenderer::destroy() {
    if (!device_) return;
    for (Target& t : targets_) destroy_target(t);
    for (Target& t : main_targets_) destroy_target(t);
    if (terrain_pipeline_) vkDestroyPipeline(device_, terrain_pipeline_, nullptr);
    if (edge_pipeline_) vkDestroyPipeline(device_, edge_pipeline_, nullptr);
    if (scene_edge_pipeline_) vkDestroyPipeline(device_, scene_edge_pipeline_, nullptr);
    if (terrain_layout_) vkDestroyPipelineLayout(device_, terrain_layout_, nullptr);
    if (edge_layout_) vkDestroyPipelineLayout(device_, edge_layout_, nullptr);
    if (pool_) vkDestroyDescriptorPool(device_, pool_, nullptr);
    if (lookup_layout_) vkDestroyDescriptorSetLayout(device_, lookup_layout_, nullptr);
    if (frame_layout_) vkDestroyDescriptorSetLayout(device_, frame_layout_, nullptr);
    if (frame_pass_) vkDestroyRenderPass(device_, frame_pass_, nullptr);
    if (output_pass_) vkDestroyRenderPass(device_, output_pass_, nullptr);
    if (point_) vkDestroySampler(device_, point_, nullptr);
    *this = CartographicRenderer{};
}

} // namespace osc::renderer
