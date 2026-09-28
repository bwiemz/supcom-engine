#include "renderer/trail_renderer.hpp"

#include "renderer/camera.hpp"
#include "renderer/frustum.hpp"
#include "renderer/recon_view.hpp"
#include "renderer/shader_utils.hpp"
#include "renderer/texture_cache.hpp"
#include "renderer/trail_blueprint.hpp"
#include "sim/world_snapshot.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstring>

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

/// Points a trail keeps: its newest, and the ones before it that a
/// catch-up can reach back to.
constexpr size_t kPointsKept = TrailRenderer::MAX_CATCHUP + 2;
/// Moho's look at a trail's intel: every fifth tick from its first.
constexpr u32 kLookEvery = 5;
/// CanSeeCam's sphere about the trail's point.
constexpr f32 kViewRadius = 5.0f;

/// particle.fx's TPolyTrail blends: all write RGB only.
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

void TrailRenderer::init(VkDevice device, VmaAllocator allocator, VkRenderPass render_pass,
                         VkDescriptorSetLayout texture_ds_layout) {
    for (u32 i = 0; i < FRAMES_IN_FLIGHT; ++i) {
        VkBufferCreateInfo buf_ci{};
        buf_ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buf_ci.size = sizeof(Vertex) * 6 * MAX_SEGMENTS;
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

    // Set 0 the ramp texture, set 1 the repeat texture.
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

    VkShaderModule vert = compile_glsl(device, shaders::trail_vert, "trail_vert", true);
    VkShaderModule frag = compile_glsl(device, shaders::trail_frag, "trail_frag", false);
    if (!vert || !frag) {
        spdlog::error("TrailRenderer: shader compilation failed");
        if (vert) vkDestroyShaderModule(device, vert, nullptr);
        if (frag) vkDestroyShaderModule(device, frag, nullptr);
        return;
    }

    VkVertexInputBindingDescription bind{};
    bind.stride = sizeof(Vertex);
    bind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    const std::array<VkVertexInputAttributeDescription, 2> attrs = {{
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, pos)},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, tvu)},
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
        if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &ci, nullptr, &pipelines_[i]) !=
            VK_SUCCESS)
            spdlog::error("TrailRenderer: pipeline {} creation failed", i);
    }
    vkDestroyShaderModule(device, vert, nullptr);
    vkDestroyShaderModule(device, frag, nullptr);
}

bool TrailRenderer::can_emit(Emitter& e, const sim::FrameView& view, u32 tick, const Vector3& eye,
                             const Frustum* frustum, const ReconView* recon) const {
    if (!e.bp->emit_if_visible) return true;
    const Vector3& p = e.points.back().pos;
    // Past its LODCutoff, or out of view: no look at its intel either.
    if ((e.bp->lod_cutoff > 0 && length(sub(p, eye)) > e.bp->lod_cutoff) ||
        (frustum && !frustum->is_sphere_visible(p.x, p.y, p.z, kViewRadius))) {
        e.first_look.reset();
        return false;
    }
    if (!recon) return true;
    // The player's army's LOS there, whoever made it: looked at on the
    // first tick it's in view and every fifth after.
    if (e.first_look && (tick - *e.first_look) % kLookEvery != 0) return e.seen;
    if (!e.first_look) e.first_look = tick;
    e.seen = recon->sees_at(view, -1, p.x, p.z);
    return e.seen;
}

void TrailRenderer::emit(u32 id, Emitter& e, const Point& from, const Point& to) {
    Vector3 dir = sub(to.pos, from.pos);
    const f32 len = length(dir);
    dir = len > 1e-6f ? scale(dir, 1.0f / len) : Vector3{};
    Segment s;
    s.effect_id = id;
    s.bp = e.bp;
    s.blueprint = e.blueprint;
    s.start = from.pos;
    s.end = to.pos;
    // The start carries the last segment's direction, so the two meet.
    s.start_tangent = e.created ? e.last_dir : dir;
    s.end_tangent = dir;
    s.start_born = from.tick + 1;
    s.end_born = to.tick + 1;
    s.u_start = e.bp->texture_repeat_rate * e.travelled;
    e.travelled += len;
    s.u_end = e.bp->texture_repeat_rate * e.travelled;
    e.last_dir = dir;
    e.created = true;
    if (segments_.size() < MAX_SEGMENTS) segments_.push_back(s);
}

void TrailRenderer::advance(const sim::FrameView& view, const Vector3& eye, const Frustum* frustum,
                            TrailBlueprintCache& blueprints, lua_State* L, const ReconView* recon) {
    const sim::WorldSnapshot& snap = *view.cur();
    const u32 tick = snap.tick;
    const u32 steps = last_tick_ ? tick - *last_tick_ : 1;
    std::unordered_set<u32> live;
    for (const sim::EffectRecord& fx : snap.effects) {
        if (fx.type != sim::EffectType::TRAIL_EMITTER) continue;
        live.insert(fx.id);
        if (!fx.anchored || unknown_.count(fx.id)) continue;
        auto it = emitters_.find(fx.id);
        if (it == emitters_.end()) {
            const TrailBlueprintData* bp = blueprints.get(fx.blueprint_path, L);
            if (!bp) {
                unknown_.insert(fx.id);
                continue;
            }
            Emitter e;
            e.bp = bp;
            e.blueprint = &*names_.insert(fx.blueprint_path).first;
            e.first_tick = tick;
            it = emitters_.emplace(fx.id, std::move(e)).first;
        }
        Emitter& e = it->second;
        e.points.push_back({fx.anchor, tick});
        if (e.points.size() > kPointsKept) e.points.pop_front();
        if (e.points.size() < 2) continue; // where it was, next tick
        // A Lifetime ≥ 0 ends it that many ticks on (ProcessLifetime).
        if (e.bp->lifetime >= 0 && static_cast<f32>(tick - e.first_tick) >= e.bp->lifetime)
            continue;
        if (!can_emit(e, view, tick, eye, frustum, recon)) {
            e.missed += steps;
            continue;
        }
        // Catch up the ticks it missed, at most TrailLength (and 24) of
        // them; dropping some, it starts afresh (OnTick).
        u32 catchup = e.missed;
        const auto most =
            std::min(static_cast<u32>(std::max(e.bp->trail_length, 0.0f)), MAX_CATCHUP);
        if (catchup > most) {
            catchup = most;
            e.created = false;
        }
        e.missed = 0;
        const size_t n = e.points.size();
        catchup = std::min(catchup, static_cast<u32>(n - 2));
        for (u32 i = catchup; i > 0; --i) emit(fx.id, e, e.points[n - 2 - i], e.points[n - 1 - i]);
        emit(fx.id, e, e.points[n - 2], e.points[n - 1]);
    }
    // Trails gone from the world stop; their segments stay theirs.
    for (auto it = emitters_.begin(); it != emitters_.end();)
        it = live.count(it->first) ? std::next(it) : emitters_.erase(it);
    for (auto it = unknown_.begin(); it != unknown_.end();)
        it = live.count(*it) ? std::next(it) : unknown_.erase(it);
    last_tick_ = tick;
}

void TrailRenderer::update(const sim::FrameView& view, const Camera& camera, const Frustum* frustum,
                           TrailBlueprintCache& blueprints, TextureCache& tex_cache, lua_State* L,
                           const ReconView* recon, u32 fi) {
    groups_.clear();
    drawn_.clear();
    const sim::WorldSnapshot* cur = view.cur();
    if (!cur) return;

    f32 ex = 0;
    f32 ey = 0;
    f32 ez = 0;
    camera.eye_position(ex, ey, ez);
    const Vector3 eye{ex, ey, ez};
    Vector3 forward = sub({camera.focus_x(), camera.focus_y(), camera.focus_z()}, eye);
    if (const f32 len = length(forward); len > 1e-6f) forward = scale(forward, 1.0f / len);

    if (last_tick_ && cur->tick < *last_tick_) clear(); // a new game
    if (!last_tick_ || cur->tick != *last_tick_) advance(view, eye, frustum, blueprints, L, recon);

    // The render clock: the entity is drawn between its last two ticks.
    const f64 now = static_cast<f64>(cur->tick) + static_cast<f64>(view.alpha());
    // Segments spent (both ends a TrailLength old) go.
    std::erase_if(segments_, [now](const Segment& s) {
        return now >= static_cast<f64>(std::max(s.start_born, s.end_born)) +
                          static_cast<f64>(s.bp->trail_length);
    });
    if (!vertex_mapped_[fi]) return;

    struct Ribbon {
        bool under_water;
        f32 sort_order;
        i32 blendmode;
        VkDescriptorSet ramp, repeat;
        std::array<Vertex, 6> v;
    };
    std::vector<Ribbon> ribbons;
    ribbons.reserve(segments_.size());
    // Each blueprint's textures, looked up once a frame.
    std::unordered_map<const TrailBlueprintData*, std::pair<const GPUTexture*, const GPUTexture*>>
        textures;
    for (const Segment& s : segments_) {
        const TrailBlueprintData& bp = *s.bp;
        if (bp.trail_length <= 0) continue;
        const f32 t0 = static_cast<f32>((now - s.start_born) / bp.trail_length);
        const f32 t1 = static_cast<f32>((now - s.end_born) / bp.trail_length);
        if ((t0 <= 0 && t1 <= 0) || (t0 >= 1 && t1 >= 1)) continue; // unborn or spent
        auto tex = textures.find(s.bp);
        if (tex == textures.end()) {
            const auto get = [&tex_cache](const std::string& path) {
                return path.empty() ? nullptr : tex_cache.get(path);
            };
            tex = textures.emplace(s.bp, std::pair{get(bp.ramp_texture), get(bp.repeat_texture)})
                      .first;
        }
        const auto [ramp, repeat] = tex->second;
        if (!ramp || !repeat) continue; // loading, or not there

        // PackTrailSegmentQuadVertices / TrailVS: each end pushed Size along
        // ±cross(view axis, its tangent); the negated side has V 1.
        const auto side = [&](const Vector3& tangent) {
            const Vector3 c = cross(forward, tangent);
            const f32 len = length(c);
            return len > 1e-6f ? scale(c, bp.size / len) : Vector3{};
        };
        const Vector3 s0 = side(s.start_tangent);
        const Vector3 s1 = side(s.end_tangent);
        const auto vertex = [](const Vector3& p, f32 t, f32 v, f32 u) {
            return Vertex{{p.x, p.y, p.z}, {t, v, u}};
        };
        const Vertex a = vertex(sub(s.start, s0), t0, 1.0f, s.u_start);
        const Vertex b = vertex(sub(s.end, s1), t1, 1.0f, s.u_end);
        const Vertex c = vertex(add(s.end, s1), t1, 0.0f, s.u_end);
        const Vertex d = vertex(add(s.start, s0), t0, 0.0f, s.u_start);
        const bool under = bp.sort_order < 0;
        ribbons.push_back({under,
                           bp.sort_order,
                           bp.blendmode,
                           ramp->descriptor_set,
                           repeat->descriptor_set,
                           {a, b, c, a, c, d}});
        drawn_.push_back({s.effect_id, *s.blueprint, s.start, s.end, s.start_tangent, t0, t1,
                          s.u_start, s.u_end, bp.size, bp.blendmode, under});
    }

    // Runs by pass, then Moho's trail buckets: SortOrder, blend, textures.
    std::stable_sort(ribbons.begin(), ribbons.end(), [](const Ribbon& x, const Ribbon& y) {
        if (x.under_water != y.under_water) return x.under_water;
        if (x.sort_order != y.sort_order) return x.sort_order < y.sort_order;
        if (x.blendmode != y.blendmode) return x.blendmode < y.blendmode;
        if (x.ramp != y.ramp) return x.ramp < y.ramp;
        return x.repeat < y.repeat;
    });
    auto* out = static_cast<Vertex*>(vertex_mapped_[fi]);
    u32 n = 0;
    for (const Ribbon& r : ribbons) {
        const bool same = !groups_.empty() && groups_.back().under_water == r.under_water &&
                          groups_.back().blendmode == r.blendmode &&
                          groups_.back().ramp == r.ramp && groups_.back().repeat == r.repeat;
        if (!same) groups_.push_back({r.under_water, r.blendmode, r.ramp, r.repeat, n, 0});
        std::memcpy(out + n, r.v.data(), sizeof(Vertex) * r.v.size());
        n += static_cast<u32>(r.v.size());
        groups_.back().vertex_count += static_cast<u32>(r.v.size());
    }
}

void TrailRenderer::render(VkCommandBuffer cmd, u32 viewport_w, u32 viewport_h,
                           const f32* view_proj, bool under_water, u32 fi) const {
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
        if (g.under_water != under_water) continue;
        if (!pipelines_[static_cast<size_t>(g.blendmode)]) continue;
        if (g.blendmode != bound) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              pipelines_[static_cast<size_t>(g.blendmode)]);
            vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(f32) * 16,
                               view_proj);
            bound = g.blendmode;
        }
        const std::array<VkDescriptorSet, 2> sets = {g.ramp, g.repeat};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0,
                                static_cast<u32>(sets.size()), sets.data(), 0, nullptr);
        vkCmdDraw(cmd, g.vertex_count, 1, g.first_vertex, 0);
    }
}

void TrailRenderer::clear() {
    emitters_.clear();
    unknown_.clear();
    segments_.clear();
    names_.clear();
    last_tick_.reset();
    groups_.clear();
    drawn_.clear();
}

void TrailRenderer::destroy(VkDevice device, VmaAllocator allocator) {
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
