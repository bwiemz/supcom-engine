#include "renderer/minimap_renderer.hpp"
#include "renderer/army_colors.hpp"
#include "renderer/camera.hpp"
#include "renderer/texture_cache.hpp"
#include "map/terrain.hpp"
#include "map/heightmap.hpp"
#include "sim/sim_state.hpp"
#include "sim/entity.hpp"
#include "sim/world_snapshot.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace osc::renderer {

static void get_army_color_simple(const sim::Entity& entity,
                                   const sim::SimState& sim,
                                   f32& r, f32& g, f32& b) {
    i32 army = entity.army();
    if (army >= 0 && army < static_cast<i32>(sim.army_count())) {
        auto* brain = sim.army_at(static_cast<size_t>(army));
        if (brain && brain->has_color()) {
            r = brain->color_r() / 255.0f;
            g = brain->color_g() / 255.0f;
            b = brain->color_b() / 255.0f;
        } else if (army < 8) {
            r = ARMY_COLORS[army][0];
            g = ARMY_COLORS[army][1];
            b = ARMY_COLORS[army][2];
        } else {
            r = g = b = 0.7f;
        }
    } else {
        r = g = b = 0.5f; // neutral
    }
}

void MinimapRenderer::init(VkDevice device, VmaAllocator allocator) {
    VkBufferCreateInfo buf_info{};
    buf_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buf_info.size = MAX_MINIMAP_QUADS * sizeof(UIInstance);
    buf_info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;

    VmaAllocationCreateInfo alloc_info{};
    alloc_info.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
    alloc_info.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
    alloc_info.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

    for (u32 i = 0; i < FRAMES_IN_FLIGHT; i++) {
        VmaAllocationInfo info{};
        vmaCreateBuffer(allocator, &buf_info, &alloc_info,
                        &instance_buf_[i].buffer, &instance_buf_[i].allocation, &info);
        instance_mapped_[i] = info.pMappedData;
    }
}

void MinimapRenderer::destroy(VkDevice /*device*/, VmaAllocator allocator) {
    for (u32 i = 0; i < FRAMES_IN_FLIGHT; i++) {
        if (instance_buf_[i].buffer)
            vmaDestroyBuffer(allocator, instance_buf_[i].buffer,
                             instance_buf_[i].allocation);
        instance_buf_[i] = {};
        instance_mapped_[i] = nullptr;
    }
}

void MinimapRenderer::build_terrain_texture(
    const map::Terrain& terrain, TextureCache& tex_cache) {

    map_w_ = static_cast<f32>(terrain.map_width());
    map_h_ = static_cast<f32>(terrain.map_height());

    const auto& hm = terrain.heightmap();
    constexpr u32 TEX = MINIMAP_TEX_SIZE;

    // Generate RGBA pixels from heightmap
    std::vector<u8> pixels(TEX * TEX * 4);

    // Find height range for normalization
    f32 min_h = 1e9f, max_h = -1e9f;
    for (u32 gz = 0; gz < hm.grid_height(); gz += 4) {
        for (u32 gx = 0; gx < hm.grid_width(); gx += 4) {
            f32 h = hm.get_height_at_grid(gx, gz);
            min_h = std::min(min_h, h);
            max_h = std::max(max_h, h);
        }
    }
    if (max_h - min_h < 1.0f) max_h = min_h + 1.0f;

    f32 water_h = terrain.water_elevation();

    for (u32 ty = 0; ty < TEX; ty++) {
        for (u32 tx = 0; tx < TEX; tx++) {
            // Map texture pixel to world position
            f32 wx = (static_cast<f32>(tx) + 0.5f) / TEX * map_w_;
            f32 wz = (static_cast<f32>(ty) + 0.5f) / TEX * map_h_;
            f32 h = terrain.get_terrain_height(wx, wz);

            u32 idx = (ty * TEX + tx) * 4;

            if (terrain.has_water() && h < water_h) {
                // Water: blue tones, darker for deeper
                f32 depth_frac = std::clamp(
                    (water_h - h) / (water_h - min_h + 1.0f), 0.0f, 1.0f);
                pixels[idx + 0] = static_cast<u8>(20 + (1.0f - depth_frac) * 40);
                pixels[idx + 1] = static_cast<u8>(40 + (1.0f - depth_frac) * 60);
                pixels[idx + 2] = static_cast<u8>(80 + (1.0f - depth_frac) * 80);
            } else {
                // Land: green-brown gradient by height
                f32 t = std::clamp((h - min_h) / (max_h - min_h), 0.0f, 1.0f);
                // Low = dark green, mid = green, high = brown/grey
                if (t < 0.4f) {
                    f32 s = t / 0.4f;
                    pixels[idx + 0] = static_cast<u8>(30 + s * 30);
                    pixels[idx + 1] = static_cast<u8>(50 + s * 50);
                    pixels[idx + 2] = static_cast<u8>(20 + s * 10);
                } else if (t < 0.7f) {
                    f32 s = (t - 0.4f) / 0.3f;
                    pixels[idx + 0] = static_cast<u8>(60 + s * 60);
                    pixels[idx + 1] = static_cast<u8>(100 - s * 30);
                    pixels[idx + 2] = static_cast<u8>(30 + s * 20);
                } else {
                    f32 s = (t - 0.7f) / 0.3f;
                    pixels[idx + 0] = static_cast<u8>(120 + s * 40);
                    pixels[idx + 1] = static_cast<u8>(70 + s * 50);
                    pixels[idx + 2] = static_cast<u8>(50 + s * 40);
                }
            }
            pixels[idx + 3] = 255; // fully opaque
        }
    }

    // Upload via TextureCache::upload_rgba
    auto* gpu_tex = tex_cache.upload_rgba("__osc_minimap_terrain",
                                           pixels.data(), TEX, TEX);
    if (gpu_tex) {
        terrain_ds_ = gpu_tex->descriptor_set;
    }
}

MapArea fit_map_area(f32 x, f32 y, f32 w, f32 h, f32 map_w, f32 map_h) {
    if (w <= 0 || h <= 0 || map_w <= 0 || map_h <= 0) return {};
    const f32 scale = std::min(w / map_w, h / map_h);
    const f32 aw = map_w * scale;
    const f32 ah = map_h * scale;
    return {x + (w - aw) * 0.5f, y + (h - ah) * 0.5f, aw, ah};
}

bool minimap_to_world(const MapArea& view, const MapArea& area, f32 mx, f32 my,
                      f32 map_w, f32 map_h, f32& out_wx, f32& out_wz) {
    if (view.w <= 0 || view.h <= 0 || area.w <= 0 || area.h <= 0) return false;
    if (mx < view.x || mx > view.x + view.w || my < view.y || my > view.y + view.h)
        return false;
    out_wx = std::clamp((mx - area.x) / area.w, 0.0f, 1.0f) * map_w;
    out_wz = std::clamp((my - area.y) / area.h, 0.0f, 1.0f) * map_h;
    return true;
}

void MinimapRenderer::emit_quad(f32 x, f32 y, f32 w, f32 h,
                                 f32 r, f32 g, f32 b, f32 a,
                                 VkDescriptorSet ds) {
    if (quads_.size() >= MAX_MINIMAP_QUADS) return;
    UIQuad q{};
    q.inst.rect[0] = x; q.inst.rect[1] = y; q.inst.rect[2] = w; q.inst.rect[3] = h;
    q.inst.uv[0] = 0; q.inst.uv[1] = 0; q.inst.uv[2] = 1; q.inst.uv[3] = 1;
    q.inst.color[0] = r; q.inst.color[1] = g; q.inst.color[2] = b; q.inst.color[3] = a;
    q.texture_ds = ds;
    quads_.push_back(q);
}

void MinimapRenderer::update(const sim::SimState& sim, const sim::FrameView& view,
                             const Camera& camera,
                              TextureCache& tex_cache,
                              const std::unordered_set<u32>* /*selected_ids*/,
                              u32 viewport_w, u32 viewport_h) {
    quads_.clear();
    draw_groups_.clear();
    quad_count_ = 0;
    if (map_w_ <= 0 || map_h_ <= 0) return;

    // Bottom-left corner
    const f32 size = static_cast<f32>(MINIMAP_SIZE);
    const f32 margin = static_cast<f32>(MINIMAP_MARGIN);
    view_ = {margin, static_cast<f32>(viewport_h) - size - margin, size, size};
    area_ = fit_map_area(view_.x, view_.y, view_.w, view_.h, map_w_, map_h_);
    build(sim, view, camera, tex_cache, viewport_w, viewport_h, /*framed=*/true);

    // Batch consecutive quads by texture and upload
    for (u32 i = 0; i < quads_.size(); ++i) {
        if (draw_groups_.empty() || draw_groups_.back().ds != quads_[i].texture_ds)
            draw_groups_.push_back({quads_[i].texture_ds, i, 1});
        else
            draw_groups_.back().count++;
    }
    quad_count_ = static_cast<u32>(quads_.size());
    if (quad_count_ > 0 && instance_mapped_[fi_]) {
        auto* dst = static_cast<UIInstance*>(instance_mapped_[fi_]);
        for (u32 i = 0; i < quad_count_; ++i) dst[i] = quads_[i].inst;
    }
}

void MinimapRenderer::paint(const sim::SimState& sim, const sim::FrameView& view,
                            const Camera& camera,
                             TextureCache& tex_cache, f32 x, f32 y, f32 w, f32 h,
                             u32 viewport_w, u32 viewport_h, std::vector<UIQuad>& out) {
    quads_.clear();
    if (map_w_ <= 0 || map_h_ <= 0) return;
    view_ = {x, y, w, h};
    area_ = fit_map_area(x, y, w, h, map_w_, map_h_);
    if (area_.w <= 0 || area_.h <= 0) return;
    build(sim, view, camera, tex_cache, viewport_w, viewport_h, /*framed=*/false);
    out.insert(out.end(), quads_.begin(), quads_.end());
}

void MinimapRenderer::build(const sim::SimState& sim, const sim::FrameView& view,
                            const Camera& camera,
                             TextureCache& tex_cache, u32 viewport_w, u32 viewport_h,
                             bool framed) {
    white_ds_ = tex_cache.fallback_descriptor();
    const f32 sw = static_cast<f32>(viewport_w);
    const f32 sh = static_cast<f32>(viewport_h);
    const auto [ax, ay, aw, ah] = area_;

    // --- Background border (the C++ HUD's own frame, round the whole view) ---
    if (framed)
        emit_quad(view_.x - 2, view_.y - 2, view_.w + 4, view_.h + 4, 0.3f, 0.3f, 0.35f, 0.9f,
                  white_ds_);

    // --- Terrain background texture ---
    VkDescriptorSet bg_ds = terrain_ds_ ? terrain_ds_ : white_ds_;
    emit_quad(ax, ay, aw, ah, 1.0f, 1.0f, 1.0f, 1.0f, bg_ds);

    // --- Unit dots ---
    auto& registry = sim.entity_registry();
    registry.for_each([&](const sim::Entity& entity) {
        if (entity.destroyed()) return;
        if (!entity.is_unit()) return;

        auto pos = view.position(entity);
        // Map world position to minimap pixel position
        f32 nx = pos.x / map_w_; // normalized [0,1]
        f32 nz = pos.z / map_h_;
        if (nx < 0 || nx > 1 || nz < 0 || nz > 1) return;

        f32 dot_x = ax + nx * aw;
        f32 dot_y = ay + nz * ah;

        f32 r, g, b;
        get_army_color_simple(entity, sim, r, g, b);

        constexpr f32 DOT_SIZE = 3.0f;
        emit_quad(dot_x - DOT_SIZE * 0.5f, dot_y - DOT_SIZE * 0.5f,
                  DOT_SIZE, DOT_SIZE, r, g, b, 1.0f, white_ds_);
    });

    // --- Camera frustum box ---
    // Unproject the 4 screen corners to world XZ to get the camera view area
    f32 corners_x[4], corners_z[4];
    bool all_valid = true;
    f32 screen_corners[4][2] = {
        {0, 0},           // top-left
        {sw, 0},          // top-right
        {sw, sh},         // bottom-right
        {0, sh},          // bottom-left
    };

    for (int i = 0; i < 4; i++) {
        if (!camera.screen_to_world(screen_corners[i][0], screen_corners[i][1],
                                     sw, sh, 0.0f,
                                     corners_x[i], corners_z[i])) {
            all_valid = false;
            break;
        }
    }

    if (all_valid) {
        // Draw 4 line segments connecting the frustum corners on the minimap
        constexpr f32 LINE_THICK = 1.5f;
        for (int i = 0; i < 4; i++) {
            int j = (i + 1) % 4;

            f32 x0 = ax + std::clamp(corners_x[i] / map_w_, 0.0f, 1.0f) * aw;
            f32 y0 = ay + std::clamp(corners_z[i] / map_h_, 0.0f, 1.0f) * ah;
            f32 x1 = ax + std::clamp(corners_x[j] / map_w_, 0.0f, 1.0f) * aw;
            f32 y1 = ay + std::clamp(corners_z[j] / map_h_, 0.0f, 1.0f) * ah;

            // AABB of the line segment
            f32 min_x = std::min(x0, x1) - LINE_THICK * 0.5f;
            f32 min_y = std::min(y0, y1) - LINE_THICK * 0.5f;
            f32 max_x = std::max(x0, x1) + LINE_THICK * 0.5f;
            f32 max_y = std::max(y0, y1) + LINE_THICK * 0.5f;

            // Ensure minimum size
            if (max_x - min_x < LINE_THICK) max_x = min_x + LINE_THICK;
            if (max_y - min_y < LINE_THICK) max_y = min_y + LINE_THICK;

            emit_quad(min_x, min_y, max_x - min_x, max_y - min_y,
                      1.0f, 1.0f, 1.0f, 0.8f, white_ds_);
        }
    }
}

void MinimapRenderer::render(VkCommandBuffer cmd, VkPipelineLayout layout,
                              u32 viewport_w, u32 viewport_h) {
    if (quad_count_ == 0) return;

    // Push viewport size
    f32 vp[2] = {static_cast<f32>(viewport_w),
                 static_cast<f32>(viewport_h)};
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                       sizeof(f32) * 2, vp);

    // Bind instance buffer
    VkBuffer buf = instance_buf_[fi_].buffer;
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &buf, &offset);

    // Draw each group with its descriptor set
    for (auto& group : draw_groups_) {
        if (group.count == 0 || !group.ds) continue;

        // Set scissor to minimap area (with small border margin, clamped to >= 0)
        VkRect2D scissor{};
        scissor.offset.x = std::max(0, static_cast<i32>(view_.x) - 2);
        scissor.offset.y = std::max(0, static_cast<i32>(view_.y) - 2);
        scissor.extent.width = static_cast<u32>(view_.w) + 4;
        scissor.extent.height = static_cast<u32>(view_.h) + 4;
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                layout, 0, 1, &group.ds, 0, nullptr);

        vkCmdDraw(cmd, 6, group.count, 0, group.offset);
    }

    // Restore full-screen scissor
    VkRect2D full_scissor{};
    full_scissor.extent = {viewport_w, viewport_h};
    vkCmdSetScissor(cmd, 0, 1, &full_scissor);
}

bool MinimapRenderer::hit_test(f32 mx, f32 my, u32 /*viewport_w*/, u32 /*viewport_h*/,
                                f32 map_w, f32 map_h,
                                f32& out_wx, f32& out_wz) const {
    return minimap_to_world(view_, area_, mx, my, map_w, map_h, out_wx, out_wz);
}

} // namespace osc::renderer
