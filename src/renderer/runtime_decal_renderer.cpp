#include "renderer/runtime_decal_renderer.hpp"

#include "map/terrain.hpp"
#include "renderer/decal_math.hpp"
#include "renderer/frustum.hpp"
#include "renderer/pipeline_builder.hpp"
#include "renderer/shader_utils.hpp"
#include "renderer/terrain_mesh.hpp"
#include "renderer/texture_cache.hpp"
#include "sim/world_snapshot.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace osc::renderer {

namespace {

/// A splat's vertex: its corner on the terrain, the terrain's normal
/// there, its UV and its alpha (CWldSplat::SplatVertex, with the normal
/// Moho reads from its normal buffer).
struct SplatVertex {
    f32 position[3];
    f32 normal[3];
    f32 uv[2];
    f32 alpha;
};
static_assert(sizeof(SplatVertex) == 36);

/// Two triangles a quad (cull none: their winding doesn't matter).
constexpr u32 kVerticesPerSplat = 6;

/// The decals' push block (decal_lit's and the splats'), 128 bytes.
struct DecalPush {
    f32 view_proj[16];
    f32 u[4];
    f32 v[4];
    f32 map_alpha[4];
    f32 eye[4];
};
static_assert(sizeof(DecalPush) == 128);

AllocatedBuffer make_mapped(VmaAllocator allocator, VkDeviceSize size, VkBufferUsageFlags usage,
                            void*& mapped) {
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
        VK_SUCCESS) {
        spdlog::error("Runtime decals: a buffer of {} bytes failed", size);
        mapped = nullptr;
        return {};
    }
    mapped = info.pMappedData;
    return buf;
}

} // namespace

void RuntimeDecalRenderer::init(VkDevice device, VmaAllocator allocator, VkRenderPass render_pass,
                                VkDescriptorSetLayout terrain_layout,
                                VkDescriptorSetLayout shadow_layout,
                                VkDescriptorSetLayout texture_layout) {
    device_ = device;
    allocator_ = allocator;
    for (u32 i = 0; i < FRAMES_IN_FLIGHT; ++i)
        splat_buf_[i] = make_mapped(allocator, sizeof(SplatVertex) * kVerticesPerSplat * kMaxSplats,
                                    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, splat_mapped_[i]);

    VkShaderModule vert = compile_glsl(device, shaders::splat_vert, "splat.vert", true);
    VkShaderModule frag = compile_glsl(device, shaders::splat_frag(), "splat.frag", false);
    if (vert && frag) {
        VkVertexInputBindingDescription binding{};
        binding.binding = 0;
        binding.stride = sizeof(SplatVertex);
        binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        const std::array<VkVertexInputAttributeDescription, 4> attrs = {{
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(SplatVertex, position)},
            {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(SplatVertex, normal)},
            {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(SplatVertex, uv)},
            {3, 0, VK_FORMAT_R32_SFLOAT, offsetof(SplatVertex, alpha)},
        }};
        // TSplats: SrcAlpha / InvSrcAlpha into RGB (the glow in alpha
        // stays), depth LessEqual unwritten, no culling, the bias in the
        // shader.
        splat_pipeline_ =
            PipelineBuilder()
                .set_shaders(vert, frag)
                .set_vertex_input(&binding, 1, attrs.data(), static_cast<u32>(attrs.size()))
                .set_depth_test(true, false)
                .set_blend(true)
                .set_cull_mode(VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE)
                .set_push_constant(sizeof(DecalPush),
                                   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)
                .set_descriptor_set_layout(terrain_layout)
                .add_descriptor_set_layout(shadow_layout)
                .add_descriptor_set_layout(texture_layout)
                .set_color_write_mask(VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                      VK_COLOR_COMPONENT_B_BIT)
                .build(device, render_pass, &splat_layout_);
    } else {
        spdlog::error("Runtime decals: the splat shaders failed to compile");
    }
    if (vert) vkDestroyShaderModule(device, vert, nullptr);
    if (frag) vkDestroyShaderModule(device, frag, nullptr);
}

void RuntimeDecalRenderer::clear() {
    decals_.clear();
    gathered_.clear();
    indices_.clear();
    gathered_generation_ = ~0u;
    for (u32& g : index_generation_) g = ~0u;
    decal_draws_.clear();
    runs_.clear();
    splat_count_ = 0;
}

void RuntimeDecalRenderer::update(const sim::WorldSnapshot* snap, i32 focus_army,
                                  const map::Terrain& terrain, const TerrainMesh& mesh,
                                  const std::array<f32, 16>& view, const std::array<f32, 3>& eye,
                                  f32 aspect, const Frustum& frustum, TextureCache& textures,
                                  u32 fi) {
    decal_draws_.clear();
    runs_.clear();
    splat_count_ = 0;
    if (!snap) return;
    decals_.update(*snap, focus_army);
    if (decals_.generation() != gathered_generation_) gather(mesh, textures);
    upload_indices(fi);

    // The decals: DrawDecalPass's LOD fade at their middle, times their own.
    const auto& decals = decals_.decals();
    for (size_t i = 0; i < decals.size() && i < gathered_.size(); ++i) {
        const RuntimeDecals::Decal& d = decals[i];
        const Gathered& g = gathered_[i];
        if (g.index_count == 0 || !index_buf_[fi].buffer) continue;
        const f32 ground = terrain.get_terrain_height(g.mid_x, g.mid_z);
        if (!frustum.is_sphere_visible(g.mid_x, ground, g.mid_z, g.radius + 64.0f)) continue;
        const f32 alpha =
            decal_lod_alpha(d.info.cut_off_lod, d.info.near_cut_off_lod,
                            decal_lod_metric(view, eye, aspect, g.mid_x, ground, g.mid_z)) *
            d.alpha;
        if (alpha < 1.0f / 255.0f) continue;
        DecalDraw draw;
        draw.decal = &d;
        draw.technique = *decal_technique(d.info.type);
        std::memcpy(draw.u, g.u, sizeof(draw.u));
        std::memcpy(draw.v, g.v, sizeof(draw.v));
        draw.alpha = alpha;
        draw.first_index = g.first_index;
        draw.index_count = g.index_count;
        decal_draws_.push_back(draw);
    }

    build_splats(terrain, view, eye, aspect, frustum, textures, fi);
}

void RuntimeDecalRenderer::gather(const TerrainMesh& mesh, TextureCache& textures) {
    gathered_.clear();
    indices_.clear();
    for (const RuntimeDecals::Decal& d : decals_.decals()) {
        Gathered& g = gathered_.emplace_back();
        // Those drawn over the terrain's colour (normals and water apart).
        if (!decal_technique(d.info.type)) continue;
        if (d.info.scale_x == 0.0f || d.info.scale_z == 0.0f) continue;
        decal_texture_matrix(d.info, g.u, g.v);
        f32 min_x = 0;
        f32 min_z = 0;
        f32 max_x = 0;
        f32 max_z = 0;
        decal_bounds(d.info, min_x, min_z, max_x, max_z);
        g.mid_x = (min_x + max_x) * 0.5f;
        g.mid_z = (min_z + max_z) * 0.5f;
        g.radius = 0.5f * std::hypot(max_x - min_x, max_z - min_z);
        // Moho loads a decal's textures as it is added (SetName).
        if (!textures.get_blocking(d.info.texture_path)) continue;
        if (!d.info.texture2_path.empty()) (void)textures.get_blocking(d.info.texture2_path);
        g.first_index = static_cast<u32>(indices_.size());
        mesh.collect_indices(min_x, min_z, max_x, max_z, indices_);
        g.index_count = static_cast<u32>(indices_.size()) - g.first_index;
    }
    gathered_generation_ = decals_.generation();
}

void RuntimeDecalRenderer::upload_indices(u32 fi) {
    if (index_generation_[fi] == gathered_generation_) return;
    if (indices_.empty()) {
        index_generation_[fi] = gathered_generation_;
        return;
    }
    const u32 need = static_cast<u32>(indices_.size());
    if (need > index_capacity_[fi]) {
        // This frame's fence has been waited on: its buffer is free.
        if (index_buf_[fi].buffer)
            vmaDestroyBuffer(allocator_, index_buf_[fi].buffer, index_buf_[fi].allocation);
        const u32 capacity = std::max(need, index_capacity_[fi] * 2);
        index_buf_[fi] = make_mapped(allocator_, sizeof(u32) * static_cast<VkDeviceSize>(capacity),
                                     VK_BUFFER_USAGE_INDEX_BUFFER_BIT, index_mapped_[fi]);
        index_capacity_[fi] = index_buf_[fi].buffer ? capacity : 0;
    }
    if (!index_mapped_[fi]) return;
    std::memcpy(index_mapped_[fi], indices_.data(), sizeof(u32) * indices_.size());
    index_generation_[fi] = gathered_generation_;
}

void RuntimeDecalRenderer::build_splats(const map::Terrain& terrain,
                                        const std::array<f32, 16>& view,
                                        const std::array<f32, 3>& eye, f32 aspect,
                                        const Frustum& frustum, TextureCache& textures, u32 fi) {
    auto* out = static_cast<SplatVertex*>(splat_mapped_[fi]);
    if (!out || !splat_pipeline_) return;
    u32 budget = 0;
    u32 written = 0;
    for (const RuntimeDecals::Decal& s : decals_.splats()) {
        // CWldSplat::UpdateVertices: its corners on the terrain.
        constexpr std::array<std::array<f32, 2>, 4> kLocal = {{{0, 0}, {1, 0}, {1, 1}, {0, 1}}};
        std::array<std::array<f32, 3>, 4> corners{};
        for (size_t c = 0; c < 4; ++c) {
            const auto xz = decal_corner(s.info, kLocal[c][0], kLocal[c][1]);
            corners[c] = {xz[0], terrain.get_terrain_height(xz[0], xz[1]), xz[1]};
        }
        const f32 mid_x = (corners[0][0] + corners[2][0]) * 0.5f;
        const f32 mid_z = (corners[0][2] + corners[2][2]) * 0.5f;
        const f32 radius = 0.5f * std::hypot(s.info.scale_x, s.info.scale_z);
        if (!frustum.is_sphere_visible(mid_x, corners[0][1], mid_z, radius + 64.0f)) continue;
        // The LOD metric at its first corner; past its cutoff it isn't drawn.
        const f32 distance =
            decal_lod_metric(view, eye, aspect, corners[0][0], corners[0][1], corners[0][2]);
        if (distance > s.info.cut_off_lod) continue;
        if (++budget >= kMaxSplats) break;
        const f32 alpha =
            decal_lod_alpha(s.info.cut_off_lod, s.info.near_cut_off_lod, distance) * s.alpha;
        const GPUTexture* texture = textures.get_blocking(s.info.texture_path);
        if (!texture) continue;

        std::array<SplatVertex, 4> quad{};
        for (size_t c = 0; c < 4; ++c) {
            const auto n = terrain_normal_at(terrain, corners[c][0], corners[c][2]);
            quad[c] = {{corners[c][0], corners[c][1], corners[c][2]},
                       {n[0], n[1], n[2]},
                       {kLocal[c][0], kLocal[c][1]},
                       alpha};
        }
        for (const size_t c : {0u, 1u, 2u, 0u, 2u, 3u}) out[written++] = quad[c];
        if (runs_.empty() || runs_.back().texture != texture->descriptor_set)
            runs_.push_back({texture->descriptor_set, written - kVerticesPerSplat, 0});
        runs_.back().vertex_count += kVerticesPerSplat;
        ++splat_count_;
    }
}

void RuntimeDecalRenderer::draw_splats(VkCommandBuffer cmd, u32 fi,
                                       const std::array<f32, 16>& view_proj,
                                       const std::array<f32, 3>& eye, f32 map_width, f32 map_height,
                                       VkDescriptorSet terrain_set,
                                       VkDescriptorSet shadow_set) const {
    if (runs_.empty() || !splat_pipeline_ || !terrain_set || !shadow_set) return;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, splat_pipeline_);
    const std::array<VkDescriptorSet, 2> shared = {terrain_set, shadow_set};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, splat_layout_, 0,
                            static_cast<u32>(shared.size()), shared.data(), 0, nullptr);
    const VkDeviceSize no_offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &splat_buf_[fi].buffer, &no_offset);
    DecalPush pc{};
    std::memcpy(pc.view_proj, view_proj.data(), sizeof(pc.view_proj));
    pc.map_alpha[0] = map_width;
    pc.map_alpha[1] = map_height;
    pc.eye[0] = eye[0];
    pc.eye[1] = eye[1];
    pc.eye[2] = eye[2];
    vkCmdPushConstants(cmd, splat_layout_,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc),
                       &pc);
    for (const Run& run : runs_) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, splat_layout_, 2, 1,
                                &run.texture, 0, nullptr);
        vkCmdDraw(cmd, run.vertex_count, 1, run.first_vertex, 0);
    }
}

void RuntimeDecalRenderer::destroy(VkDevice device, VmaAllocator allocator) {
    for (u32 i = 0; i < FRAMES_IN_FLIGHT; ++i) {
        if (index_buf_[i].buffer)
            vmaDestroyBuffer(allocator, index_buf_[i].buffer, index_buf_[i].allocation);
        if (splat_buf_[i].buffer)
            vmaDestroyBuffer(allocator, splat_buf_[i].buffer, splat_buf_[i].allocation);
        index_buf_[i] = {};
        splat_buf_[i] = {};
        index_mapped_[i] = nullptr;
        splat_mapped_[i] = nullptr;
        index_capacity_[i] = 0;
    }
    if (splat_pipeline_) vkDestroyPipeline(device, splat_pipeline_, nullptr);
    if (splat_layout_) vkDestroyPipelineLayout(device, splat_layout_, nullptr);
    splat_pipeline_ = VK_NULL_HANDLE;
    splat_layout_ = VK_NULL_HANDLE;
    clear();
}

} // namespace osc::renderer
