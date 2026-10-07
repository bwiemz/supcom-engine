#include "renderer/range_renderer.hpp"

#include "renderer/frustum.hpp"
#include "renderer/shader_utils.hpp"
#include "renderer/vk_cmd.hpp"
#include "sim/world_snapshot.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstddef>
#include <cstring>

namespace osc::renderer {

namespace {

// range.fx's vertexShader: a circle's point scaled by the radius its
// weights pick, about the ring's centre; its height the volume's bottom or
// top (the template holds 0 or 1)
const char* kCastVert = R"glsl(
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

const char* kCastFrag = R"glsl(
#version 450

layout(location = 0) out vec4 outColor;

void main() {
    outColor = vec4(0.0);
}
)glsl";

// frame.fx's full-screen passes: one colour (RangePS)
const char* kScreenVert = R"glsl(
#version 450

void main() {
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)glsl";

const char* kScreenFrag = R"glsl(
#version 450

layout(push_constant) uniform PushConstants {
    vec4 color;
} pc;

layout(location = 0) out vec4 outColor;

void main() {
    outColor = pc.color;
}
)glsl";

struct CastPush {
    f32 view_proj[16];
    f32 heights[2];
};

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

struct PassState {
    bool cast = false; ///< the volumes (vertex input, depth test), else full screen
    VkStencilOpState front{}, back{};
    VkColorComponentFlags write = 0;
    bool blend = false; ///< SrcAlpha, InvSrcAlpha
};

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
    if (state.cast) {
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
    // Both faces: Cast's two passes, one per facing, in one
    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
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
        spdlog::error("RangeRenderer: pipeline creation failed");
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

/// A mapped host-visible buffer of `size` bytes, with `data` in it if given
void* make_buffer(VmaAllocator allocator, VkBufferUsageFlags usage, size_t size,
                  AllocatedBuffer& out, const void* data = nullptr) {
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

} // namespace

RangeScene collect_range_scene(const RangeOverlays& overlays, const sim::FrameView& view,
                               const Frustum& frustum, i32 focus_army,
                               const std::unordered_set<u32>* selected, u32 hovered,
                               const std::string* placing, f32 cursor_x, f32 cursor_z,
                               RangeBlueprints& blueprints, lua_State* L) {
    RangeScene scene;
    const RangeOverlays::Settings& settings = overlays.settings();
    if (!settings.enabled) return scene;
    const sim::WorldSnapshot* snap = view.cur();
    if (placing && settings.render_build) {
        scene.placing = blueprints.find(*placing, L);
        scene.cursor_x = cursor_x;
        scene.cursor_z = cursor_z;
    }
    if (!snap || focus_army < 0) return scene;

    // A unit of the focus army, as the extractors see it (nothing for
    // another's, or one whose blueprint the UI state hasn't)
    const auto own = [&](const sim::EntityRecord& e) -> std::optional<RangeUnit> {
        if (!e.is_unit || e.army != focus_army || e.is_dying) return std::nullopt;
        const RangeBlueprint* bp = blueprints.find(e.blueprint_id, L);
        if (!bp) return std::nullopt;
        const sim::Vector3 at = view.position(e);
        return range_unit(*snap, e, *bp, at.x, at.z);
    };
    // CameraImpl's units in view, for the active filters: the focus army's
    // whose bounds the view holds (here a sphere round the footprint)
    if (!overlays.filters().empty()) {
        for (const sim::EntityRecord& e : view.entities()) {
            if (e.is_being_built) continue;
            auto unit = own(e);
            if (!unit) continue;
            const sim::Vector3 at = view.position(e);
            const f32 reach = std::max({e.footprint_size_x, e.footprint_size_z, 1.0f});
            if (frustum.is_sphere_visible(at.x, at.y, at.z, reach)) scene.in_view.push_back(*unit);
        }
    }
    if (selected && settings.render_selected) {
        for (const sim::EntityRecord& e : view.entities())
            if (selected->count(e.id) != 0)
                if (auto unit = own(e)) scene.selected.push_back(*unit);
    }
    if (hovered != 0 && settings.render_highlighted)
        if (const sim::EntityRecord* e = view.find(hovered)) scene.hovered = own(*e);
    const sim::ArmyRecord* army = snap->army(focus_army);
    if (snap->no_rush_radius > 0.0f && army && army->valid)
        scene.no_rush = RangeRing{army->start_x, army->start_z, 0.0f, snap->no_rush_radius};
    return scene;
}

void RangeRenderer::init(VkDevice device, VmaAllocator allocator, VkRenderPass render_pass) {
    // The volume's template: its height 0 at the bottom, 1 at the top
    const std::vector<RingVertex> volume = ring_volume_vertices(0.0f, 1.0f);
    const std::vector<u16> indices = ring_volume_indices();
    index_count_ = static_cast<u32>(indices.size());
    make_buffer(allocator, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, sizeof(RingVertex) * volume.size(),
                volume_, volume.data());
    make_buffer(allocator, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, sizeof(u16) * indices.size(), indices_,
                indices.data());
    for (u32 i = 0; i < FRAMES_IN_FLIGHT; ++i)
        band_mapped_[i] = make_buffer(allocator, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                      sizeof(RangeRing) * MAX_BANDS, band_buf_[i]);

    cast_layout_ = make_layout(device, VK_SHADER_STAGE_VERTEX_BIT, sizeof(CastPush));
    screen_layout_ = make_layout(device, VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(f32) * 4);

    VkShaderModule cast_vert = compile_glsl(device, kCastVert, "range_cast.vert", true);
    VkShaderModule cast_frag = compile_glsl(device, kCastFrag, "range_cast.frag", false);
    VkShaderModule screen_vert = compile_glsl(device, kScreenVert, "range_screen.vert", true);
    VkShaderModule screen_frag = compile_glsl(device, kScreenFrag, "range_screen.frag", false);
    if (cast_vert && cast_frag && screen_vert && screen_frag) {
        constexpr VkColorComponentFlags kRgb =
            VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
        constexpr VkColorComponentFlags kRgba = kRgb | VK_COLOR_COMPONENT_A_BIT;
        constexpr auto keep = VK_STENCIL_OP_KEEP;
        // Cast: count where the depth so far lies in a volume (z-fail, the
        // front faces up and the back down, bits 0-6), outside a marked fill
        PassState cast{true};
        cast.front = stencil(VK_COMPARE_OP_NOT_EQUAL, 0x80, 0xFF, 0x7F, VK_STENCIL_OP_ZERO, keep,
                             VK_STENCIL_OP_INCREMENT_AND_WRAP);
        cast.back = cast.front;
        cast.back.depthFailOp = VK_STENCIL_OP_DECREMENT_AND_WRAP;
        cast_ = make_pipeline(device, render_pass, cast_layout_, cast_vert, cast_frag, cast);
        // RangeMask: mark (bit 7) where a fill counted
        PassState mask;
        mask.front = mask.back =
            stencil(VK_COMPARE_OP_NOT_EQUAL, 0x7F, 0x80, 0xFF, keep, VK_STENCIL_OP_REPLACE, keep);
        mask_ = make_pipeline(device, render_pass, screen_layout_, screen_vert, screen_frag, mask);
        // RangeFill: tint the marked fills (colour alone)
        PassState fill;
        fill.front = fill.back = stencil(VK_COMPARE_OP_EQUAL, 0x80, 0xFF, 0x00, keep, keep, keep);
        fill.write = kRgb;
        fill.blend = true;
        fill_ = make_pipeline(device, render_pass, screen_layout_, screen_vert, screen_frag, fill);
        // RangeBurn: the colour and its glow where a line counted
        PassState burn;
        burn.front = burn.back =
            stencil(VK_COMPARE_OP_NOT_EQUAL, 0x7F, 0x00, 0x00, keep, keep, keep);
        burn.write = kRgba;
        burn_ = make_pipeline(device, render_pass, screen_layout_, screen_vert, screen_frag, burn);
    } else {
        spdlog::error("RangeRenderer: shader compilation failed");
    }
    for (VkShaderModule m : {cast_vert, cast_frag, screen_vert, screen_frag})
        if (m) vkDestroyShaderModule(device, m, nullptr);
}

void RangeRenderer::update(const std::vector<RangeBatch>& batches,
                           const RangeOverlays::Settings& settings, f32 span, f32 zoom_ratio,
                           u32 fi) {
    drawn_.clear();
    bands_.clear();
    fill_on_ = settings.fill;
    std::vector<RangeRing> fills;
    std::vector<RangeRing> edges;
    for (const RangeBatch& batch : batches) {
        Drawn d;
        d.color = batch.color;
        d.inner_thickness =
            ring_thickness(batch.inner, settings.inner_thickness_coeff, span, zoom_ratio);
        d.outer_thickness =
            ring_thickness(batch.outer, settings.outer_thickness_coeff, span, zoom_ratio);
        fills.clear();
        edges.clear();
        ring_bands(batch.rings, d.inner_thickness, d.outer_thickness, fills, edges);
        if (bands_.size() + fills.size() + edges.size() > MAX_BANDS) {
            if (!warned_full_) spdlog::warn("RangeRenderer: over {} rings a frame", MAX_BANDS);
            warned_full_ = true;
            break;
        }
        d.fill_first = static_cast<u32>(bands_.size());
        d.fill_count = static_cast<u32>(fills.size());
        bands_.insert(bands_.end(), fills.begin(), fills.end());
        d.edge_first = static_cast<u32>(bands_.size());
        d.edge_count = static_cast<u32>(edges.size());
        bands_.insert(bands_.end(), edges.begin(), edges.end());
        drawn_.push_back(d);
    }
    if (band_mapped_[fi] && !bands_.empty())
        std::memcpy(band_mapped_[fi], bands_.data(), sizeof(RangeRing) * bands_.size());
}

void RangeRenderer::render(VkCommandBuffer cmd, u32 viewport_w, u32 viewport_h,
                           const f32* view_proj, u32 fi) const {
    if (drawn_.empty() || !cast_ || !mask_ || !fill_ || !burn_ || !band_buf_[fi].buffer) return;
    VkViewport viewport{};
    viewport.width = static_cast<f32>(viewport_w);
    viewport.height = static_cast<f32>(viewport_h);
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    VkRect2D scissor{};
    scissor.extent = {viewport_w, viewport_h};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    CastPush push{};
    std::memcpy(push.view_proj, view_proj, sizeof(push.view_proj));
    push.heights[0] = bottom_;
    push.heights[1] = top_;

    const auto cast = [&](u32 first, u32 count) {
        if (count == 0) return;
        vkc::bind_pipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, cast_);
        vkc::push_constants(cmd, cast_layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
        const std::array<VkBuffer, 2> buffers = {volume_.buffer, band_buf_[fi].buffer};
        const std::array<VkDeviceSize, 2> offsets = {0, 0};
        vkCmdBindVertexBuffers(cmd, 0, 2, buffers.data(), offsets.data());
        vkCmdBindIndexBuffer(cmd, indices_.buffer, 0, VK_INDEX_TYPE_UINT16);
        vkc::draw_indexed(cmd, index_count_, count, 0, 0, first);
    };
    const auto screen = [&](VkPipeline pipeline, const RangeColor& color) {
        vkc::bind_pipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkc::push_constants(cmd, screen_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(f32) * 4,
                            color.data());
        vkc::draw(cmd, 3, 1, 0, 0);
    };
    constexpr RangeColor kNone{};
    constexpr RangeColor kFillTint{1.0f, 1.0f, 1.0f, 0.125f};
    for (const Drawn& d : drawn_) {
        cast(d.fill_first, d.fill_count);
        screen(mask_, kNone);
        cast(d.edge_first, d.edge_count);
        if (fill_on_) screen(fill_, kFillTint);
        screen(burn_, d.color);
        // The stencil clear for the next batch
        VkClearAttachment clear{};
        clear.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
        clear.clearValue.depthStencil = {1.0f, 0};
        VkClearRect rect{};
        rect.rect = scissor;
        rect.layerCount = 1;
        vkCmdClearAttachments(cmd, 1, &clear, 1, &rect);
    }
}

void RangeRenderer::destroy(VkDevice device, VmaAllocator allocator) {
    for (VkPipeline* p : {&cast_, &mask_, &fill_, &burn_}) {
        if (*p) vkDestroyPipeline(device, *p, nullptr);
        *p = VK_NULL_HANDLE;
    }
    for (VkPipelineLayout* l : {&cast_layout_, &screen_layout_}) {
        if (*l) vkDestroyPipelineLayout(device, *l, nullptr);
        *l = VK_NULL_HANDLE;
    }
    const auto free = [allocator](AllocatedBuffer& b) {
        if (b.buffer) vmaDestroyBuffer(allocator, b.buffer, b.allocation);
        b = {};
    };
    free(volume_);
    free(indices_);
    for (u32 i = 0; i < FRAMES_IN_FLIGHT; ++i) {
        free(band_buf_[i]);
        band_mapped_[i] = nullptr;
    }
    drawn_.clear();
    bands_.clear();
}

} // namespace osc::renderer
