#include "renderer/beam_renderer.hpp"
#include "renderer/vk_cmd.hpp"

#include "renderer/beam_blueprint.hpp"
#include "renderer/camera.hpp"
#include "renderer/effect_blueprint_file.hpp"
#include "renderer/recon_view.hpp"
#include "renderer/shader_utils.hpp"
#include "renderer/texture_cache.hpp"
#include "sim/world_snapshot.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace osc::renderer {

namespace {

using sim::Vector3;

Vector3 add(const Vector3& a, const Vector3& b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
Vector3 sub(const Vector3& a, const Vector3& b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
Vector3 scale(const Vector3& a, f32 s) {
    return {a.x * s, a.y * s, a.z * s};
}
Vector3 cross(const Vector3& a, const Vector3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
f32 length(const Vector3& a) {
    return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
}
Vector3 lerp(const Vector3& a, const Vector3& b, f32 t) {
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
}

/// A beam effect's ends in a record, given its blueprint's length.
bool ends_of(const sim::EffectRecord& fx, f32 bp_length, Vector3& start, Vector3& end) {
    switch (fx.beam) {
    case sim::EffectRecord::BeamReach::Ends:
        start = fx.beam_start;
        end = fx.beam_end;
        return true;
    case sim::EffectRecord::BeamReach::Along:
        start = fx.beam_start;
        end = add(fx.beam_start, scale(fx.beam_dir, bp_length));
        return true;
    case sim::EffectRecord::BeamReach::None: break;
    }
    return false;
}

/// particle.fx's TBeam blends (D3D9 blends alpha by the colour's factors;
/// the three that write RGBA add to the frame's glow).
struct Blend {
    VkBlendFactor src, dst;
    bool write_alpha;
};
constexpr std::array<Blend, 5> kBlends = {{
    {VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, true}, // ALPHABLEND
    {VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR, false},     // MODULATEINVERSE
    {VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
     false},                                                          // MODULATE2XINVERSE
    {VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE, true},                 // ADD
    {VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, true}, // PREMODALPHA
}};

/// For the alpha channel, a colour factor's alpha counterpart.
VkBlendFactor alpha_factor(VkBlendFactor f) {
    switch (f) {
    case VK_BLEND_FACTOR_SRC_COLOR: return VK_BLEND_FACTOR_SRC_ALPHA;
    case VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case VK_BLEND_FACTOR_DST_COLOR: return VK_BLEND_FACTOR_DST_ALPHA;
    case VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    default: return f;
    }
}

} // namespace

void BeamRenderer::init(VkDevice device, VmaAllocator allocator, VkRenderPass render_pass,
                        VkDescriptorSetLayout texture_ds_layout) {
    for (u32 i = 0; i < FRAMES_IN_FLIGHT; ++i) {
        VkBufferCreateInfo buf_ci{};
        buf_ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buf_ci.size = sizeof(Vertex) * 6 * MAX_BEAMS;
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

    VkShaderModule vert = compile_glsl(device, shaders::beam_vert, "beam_vert", true);
    VkShaderModule frag = compile_glsl(device, shaders::beam_frag, "beam_frag", false);
    if (!vert || !frag) {
        spdlog::error("BeamRenderer: shader compilation failed");
        if (vert) vkDestroyShaderModule(device, vert, nullptr);
        if (frag) vkDestroyShaderModule(device, frag, nullptr);
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
    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";

    for (size_t i = 0; i < kBlends.size(); ++i) {
        VkPipelineColorBlendAttachmentState att{};
        att.blendEnable = VK_TRUE;
        att.srcColorBlendFactor = kBlends[i].src;
        att.dstColorBlendFactor = kBlends[i].dst;
        att.colorBlendOp = VK_BLEND_OP_ADD;
        att.srcAlphaBlendFactor = alpha_factor(kBlends[i].src);
        att.dstAlphaBlendFactor = alpha_factor(kBlends[i].dst);
        att.alphaBlendOp = VK_BLEND_OP_ADD;
        att.colorWriteMask =
            VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
        if (kBlends[i].write_alpha) att.colorWriteMask |= VK_COLOR_COMPONENT_A_BIT;
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
        if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &ci, nullptr, &pipelines_[i]) !=
            VK_SUCCESS)
            spdlog::error("BeamRenderer: pipeline {} creation failed", i);
    }
    vkDestroyShaderModule(device, vert, nullptr);
    vkDestroyShaderModule(device, frag, nullptr);
}

void BeamRenderer::update(const sim::FrameView& view, const Camera& camera,
                          BeamBlueprintCache& blueprints, TextureCache& tex_cache, lua_State* L,
                          const ReconView* recon, f32 time, u32 fi) {
    groups_.clear();
    drawn_.clear();
    drawn_effects_.clear();
    drawn_entities_.clear();
    const sim::WorldSnapshot* cur = view.cur();
    if (!cur || !vertex_mapped_[fi]) return;

    // Last tick's beams, to draw each between its two ticks' ends.
    std::unordered_map<u32, const sim::EffectRecord*> before;
    if (const sim::WorldSnapshot* prev = view.prev())
        for (const sim::EffectRecord& fx : prev->effects)
            if (fx.beam != sim::EffectRecord::BeamReach::None) before[fx.id] = &fx;

    f32 ex = 0;
    f32 ey = 0;
    f32 ez = 0;
    camera.eye_position(ex, ey, ez);
    const Vector3 eye{ex, ey, ez};
    Vector3 forward = sub({camera.focus_x(), camera.focus_y(), camera.focus_z()}, eye);
    if (const f32 len = length(forward); len > 1e-6f) forward = scale(forward, 1.0f / len);

    struct Strip {
        i32 blendmode;
        VkDescriptorSet ds;
        std::array<Vertex, 6> v;
    };
    std::vector<Strip> strips;
    for (const sim::EffectRecord& fx : cur->effects) {
        if (fx.beam == sim::EffectRecord::BeamReach::None || strips.size() >= MAX_BEAMS) continue;
        const BeamBlueprintData* bp = blueprints.get(fx.blueprint_path, L);
        if (!bp) continue; // an emitter's, not a beam's
        if (!fidelity_allows(bp->fidelity, fidelity_)) { // Moho destroys it on making
            drawn_effects_.insert(fx.id);
            if (fx.entity_id) drawn_entities_.insert(fx.entity_id);
            continue;
        }
        Vector3 start;
        Vector3 end;
        if (!ends_of(fx, bp->length, start, end)) continue;
        if (auto it = before.find(fx.id); it != before.end()) {
            Vector3 s0;
            Vector3 e0;
            if (ends_of(*it->second, bp->length, s0, e0)) {
                start = lerp(s0, start, view.alpha());
                end = lerp(e0, end, view.alpha());
            }
        }
        if (recon && !recon->sees_beam(view, start, end)) continue;
        // Past its LODCutoff (its nearer end), not drawn.
        if (bp->lod_cutoff > 0 &&
            std::min(length(sub(start, eye)), length(sub(end, eye))) > bp->lod_cutoff) {
            drawn_effects_.insert(fx.id);
            if (fx.entity_id) {
                drawn_entities_.insert(fx.entity_id);
            }
            continue;
        }
        const GPUTexture* tex = bp->texture.empty() ? nullptr : tex_cache.get(bp->texture);
        if (!tex) continue; // loading, or not there

        // EmitInterpolatedBeamQuadVertices / BeamVS: the axis from end to
        // start, the strip pushed Thickness each way across it and the view;
        // V runs 0 to length × RepeatRate (1 without), both scrolled.
        const Vector3 axis = sub(start, end);
        const f32 len = length(axis);
        if (len < 1e-4f) continue;
        const f32 repeat = bp->repeat_rate != 0.0f ? len * bp->repeat_rate : 1.0f;
        Vector3 side = cross(forward, scale(axis, 1.0f / len));
        const f32 side_len = length(side);
        if (side_len < 1e-6f) continue; // seen end-on
        side = scale(side, bp->thickness / side_len);
        const f32 us = bp->ushift * time;
        const f32 vs = bp->vshift * time;
        const auto vertex = [](const Vector3& p, f32 u, f32 v, const std::array<f32, 4>& c) {
            return Vertex{{p.x, p.y, p.z}, {u, v}, {c[0], c[1], c[2], c[3]}};
        };
        const Vertex a = vertex(add(start, side), 1.0f + us, vs, bp->start_color);
        const Vertex b = vertex(add(end, side), 1.0f + us, repeat + vs, bp->end_color);
        const Vertex c = vertex(sub(end, side), us, repeat + vs, bp->end_color);
        const Vertex d = vertex(sub(start, side), us, vs, bp->start_color);
        strips.push_back({bp->blendmode, tex->descriptor_set, {a, b, c, a, c, d}});

        drawn_.push_back({fx.id, fx.blueprint_path, start, end, bp->thickness, bp->start_color,
                          bp->end_color, bp->blendmode, us, vs, repeat + vs});
        drawn_effects_.insert(fx.id);
        if (fx.entity_id) drawn_entities_.insert(fx.entity_id);
    }

    // Runs by blend, then texture (Moho's beam buckets).
    std::stable_sort(strips.begin(), strips.end(), [](const Strip& x, const Strip& y) {
        return x.blendmode != y.blendmode ? x.blendmode < y.blendmode : x.ds < y.ds;
    });
    auto* out = static_cast<Vertex*>(vertex_mapped_[fi]);
    u32 n = 0;
    for (const Strip& s : strips) {
        if (groups_.empty() || groups_.back().blendmode != s.blendmode || groups_.back().ds != s.ds)
            groups_.push_back({s.blendmode, s.ds, n, 0});
        std::memcpy(out + n, s.v.data(), sizeof(Vertex) * s.v.size());
        n += static_cast<u32>(s.v.size());
        groups_.back().vertex_count += static_cast<u32>(s.v.size());
    }
}

void BeamRenderer::render(VkCommandBuffer cmd, u32 viewport_w, u32 viewport_h, const f32* view_proj,
                          u32 fi) const {
    if (groups_.empty() || !vertex_buf_[fi].buffer) return;
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
    i32 bound = -1;
    for (const Group& g : groups_) {
        if (!pipelines_[static_cast<size_t>(g.blendmode)]) continue;
        if (g.blendmode != bound) {
            vkc::bind_pipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                               pipelines_[static_cast<size_t>(g.blendmode)]);
            vkc::push_constants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(f32) * 16,
                                view_proj);
            bound = g.blendmode;
        }
        vkc::bind_descriptor_sets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &g.ds, 0,
                                  nullptr);
        vkc::draw(cmd, g.vertex_count, 1, g.first_vertex, 0);
    }
}

void BeamRenderer::destroy(VkDevice device, VmaAllocator allocator) {
    for (VkPipeline& p : pipelines_) {
        if (p) vkDestroyPipeline(device, p, nullptr);
        p = VK_NULL_HANDLE;
    }
    if (layout_) vkDestroyPipelineLayout(device, layout_, nullptr);
    layout_ = VK_NULL_HANDLE;
    for (u32 i = 0; i < FRAMES_IN_FLIGHT; ++i) {
        if (vertex_buf_[i].buffer)
            vmaDestroyBuffer(allocator, vertex_buf_[i].buffer, vertex_buf_[i].allocation);
        vertex_buf_[i] = {};
        vertex_mapped_[i] = nullptr;
    }
}

} // namespace osc::renderer
