#include "renderer/profile_overlay.hpp"
#include "renderer/font_cache.hpp"
#include "renderer/texture_cache.hpp"
#include "core/profiler.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace osc::renderer {

void ProfileOverlay::init(VkDevice /*device*/, VmaAllocator allocator) {
    VkBufferCreateInfo buf_info{};
    buf_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buf_info.size = MAX_PROFILE_QUADS * sizeof(UIInstance);
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

void ProfileOverlay::destroy(VkDevice /*device*/, VmaAllocator allocator) {
    for (u32 i = 0; i < FRAMES_IN_FLIGHT; i++) {
        if (instance_buf_[i].buffer)
            vmaDestroyBuffer(allocator, instance_buf_[i].buffer,
                             instance_buf_[i].allocation);
        instance_buf_[i] = {};
        instance_mapped_[i] = nullptr;
    }
}

void ProfileOverlay::emit_quad(f32 x, f32 y, f32 w, f32 h,
                                f32 u0, f32 v0, f32 u1, f32 v1,
                                f32 r, f32 g, f32 b, f32 a) {
    if (quad_count_ >= MAX_PROFILE_QUADS) return;
    UIInstance inst{};
    inst.rect[0] = x; inst.rect[1] = y; inst.rect[2] = w; inst.rect[3] = h;
    inst.uv[0] = u0; inst.uv[1] = v0; inst.uv[2] = u1; inst.uv[3] = v1;
    inst.color[0] = r; inst.color[1] = g; inst.color[2] = b; inst.color[3] = a;
    quads_.push_back(inst);
    quad_count_++;
}

void ProfileOverlay::emit_solid(f32 x, f32 y, f32 w, f32 h,
                                 f32 r, f32 g, f32 b, f32 a) {
    emit_quad(x, y, w, h, 0.0f, 0.0f, 1.0f, 1.0f, r, g, b, a);
}

void ProfileOverlay::begin_group(VkDescriptorSet ds) {
    current_ds_ = ds;
    current_group_start_ = quad_count_;
}

void ProfileOverlay::end_group() {
    u32 count = quad_count_ - current_group_start_;
    if (count > 0) {
        groups_.push_back({current_ds_, current_group_start_, count});
    }
}

f32 ProfileOverlay::emit_text(const std::string& text, f32 x, f32 y,
                               f32 r, f32 g, f32 b, f32 a,
                               FontCache& font_cache,
                               const std::string& font_family, i32 font_size) {
    auto* atlas = font_cache.get(font_family, font_size);
    if (!atlas) return 0.0f;

    f32 cursor_x = x;
    f32 baseline_y = y + atlas->metrics.ascent;

    for (char c : text) {
        u32 cp = static_cast<u32>(static_cast<u8>(c));
        auto it = atlas->glyphs.find(cp);
        if (it == atlas->glyphs.end()) {
            auto sp = atlas->glyphs.find(32);
            if (sp != atlas->glyphs.end())
                cursor_x += sp->second.x_advance;
            continue;
        }

        auto& gi = it->second;
        f32 gx = cursor_x + gi.x_offset;
        f32 gy = baseline_y - gi.y_offset;

        if (gi.width > 0 && gi.height > 0) {
            emit_quad(gx, gy, gi.width, gi.height,
                      gi.u0, gi.v0, gi.u1, gi.v1,
                      r, g, b, a);
        }
        cursor_x += gi.x_advance;
    }
    return cursor_x - x;
}

void ProfileOverlay::update(FontCache& font_cache, TextureCache& tex_cache,
                             u32 viewport_w, u32 /*viewport_h*/) {
    quads_.clear();
    groups_.clear();
    quad_count_ = 0;

    auto& profiler = Profiler::instance();
    if (!profiler.enabled() || profiler.zone_count() == 0) return;

    VkDescriptorSet white_ds = tex_cache.fallback_descriptor();
    auto* font_atlas = font_cache.get("Arial", FONT_SIZE);
    VkDescriptorSet font_ds = font_atlas ? font_atlas->descriptor_set : VK_NULL_HANDLE;

    f32 sw = static_cast<f32>(viewport_w);
    f32 panel_x = sw - PANEL_WIDTH - MARGIN;
    f32 panel_y = MARGIN;

    u32 zone_count = profiler.zone_count();
    f32 panel_h = MARGIN * 2.0f +
                  ROW_HEIGHT +               // header
                  ROW_HEIGHT * zone_count +   // zone rows
                  MARGIN +
                  GRAPH_HEIGHT +              // sparkline
                  MARGIN;

    // --- Solid background + bars ---
    begin_group(white_ds);

    // Panel background
    emit_solid(panel_x, panel_y, PANEL_WIDTH, panel_h,
               0.02f, 0.02f, 0.06f, 0.85f);

    // Frame time sparkline graph
    f32 graph_x = panel_x + MARGIN;
    f32 graph_y = panel_y + MARGIN + ROW_HEIGHT + ROW_HEIGHT * zone_count + MARGIN;
    f32 graph_w = PANEL_WIDTH - MARGIN * 2.0f;

    // Graph background
    emit_solid(graph_x, graph_y, graph_w, GRAPH_HEIGHT,
               0.05f, 0.05f, 0.1f, 0.8f);

    // 16.67ms target line (60 FPS)
    f32 target_60_y = graph_y + GRAPH_HEIGHT * (1.0f - 16667.0f / 33333.0f);
    emit_solid(graph_x, target_60_y, graph_w, 1.0f,
               0.2f, 0.6f, 0.2f, 0.5f);

    // Sparkline bars
    u32 window = std::min(profiler.frame_count(), Profiler::HISTORY_SIZE);
    const f64* hist = profiler.frame_time_history();
    u32 hi = profiler.history_index();
    f32 bar_w = graph_w / static_cast<f32>(Profiler::HISTORY_SIZE);
    f32 max_us = 33333.0f; // scale: 0..33.3ms (30 FPS)

    for (u32 i = 0; i < window && i < Profiler::HISTORY_SIZE; ++i) {
        u32 idx = (hi + Profiler::HISTORY_SIZE - 1 - i) % Profiler::HISTORY_SIZE;
        f32 val = static_cast<f32>(hist[idx]);
        if (val < 0) continue;

        f32 h = std::min(val / max_us, 1.0f) * GRAPH_HEIGHT;
        f32 bx = graph_x + graph_w - (i + 1) * bar_w;

        // Color: green < 16.67ms, yellow < 33ms, red >= 33ms
        f32 cr, cg, cb;
        if (val < 16667.0f) {
            cr = 0.2f; cg = 0.8f; cb = 0.3f;
        } else if (val < 33333.0f) {
            cr = 0.9f; cg = 0.8f; cb = 0.2f;
        } else {
            cr = 0.9f; cg = 0.2f; cb = 0.2f;
        }

        emit_solid(bx, graph_y + GRAPH_HEIGHT - h, bar_w, h,
                   cr, cg, cb, 0.8f);
    }

    // Zone timing bars
    const auto* stats = profiler.zone_stats();
    f32 row_y = panel_y + MARGIN + ROW_HEIGHT;

    for (u32 i = 0; i < zone_count; ++i) {
        auto& z = stats[i];
        f32 ry = row_y + i * ROW_HEIGHT;

        // Timing bar (proportional to avg_us, max BAR_MAX_WIDTH at 16.67ms)
        f32 bar_frac = static_cast<f32>(z.avg_us) / 16667.0f;
        bar_frac = std::min(bar_frac, 1.0f);
        f32 bw = bar_frac * BAR_MAX_WIDTH;
        f32 bx = panel_x + PANEL_WIDTH - MARGIN - BAR_MAX_WIDTH;

        // Color by percentage of frame budget
        f32 br, bg, bb;
        if (bar_frac < 0.3f) {
            br = 0.2f; bg = 0.7f; bb = 0.3f;
        } else if (bar_frac < 0.6f) {
            br = 0.8f; bg = 0.7f; bb = 0.2f;
        } else {
            br = 0.9f; bg = 0.3f; bb = 0.2f;
        }

        if (bw > 0.5f) {
            emit_solid(bx, ry + 2.0f, bw, ROW_HEIGHT - 4.0f,
                       br, bg, bb, 0.7f);
        }
    }

    end_group();

    // --- Text labels (font atlas) ---
    if (font_ds) {
        begin_group(font_ds);

        // Header: avg FPS
        f64 avg_us = profiler.avg_frame_time_us();
        char header[64];
        std::snprintf(header, sizeof(header), "PROFILE  %.1f FPS  (%.2f ms)",
                      avg_us > 0 ? 1e6 / avg_us : 0, avg_us / 1000.0);
        emit_text(header, panel_x + MARGIN, panel_y + MARGIN,
                  0.9f, 0.9f, 0.3f, 1.0f,
                  font_cache, "Arial", FONT_SIZE);

        // Zone rows
        for (u32 i = 0; i < zone_count; ++i) {
            auto& z = stats[i];
            f32 ry = row_y + i * ROW_HEIGHT;

            // Indent + name
            f32 indent = z.depth * 8.0f;
            emit_text(z.name, panel_x + MARGIN + indent, ry,
                      0.8f, 0.85f, 0.9f, 1.0f,
                      font_cache, "Arial", FONT_SIZE);

            // Timing text (right-aligned before bar)
            char tbuf[32];
            std::snprintf(tbuf, sizeof(tbuf), "%.1fus", z.avg_us);
            f32 text_x = panel_x + PANEL_WIDTH - MARGIN - BAR_MAX_WIDTH - 4.0f;
            // Right-align: estimate text width and shift left
            f32 tw = static_cast<f32>(std::strlen(tbuf)) * 6.5f;
            emit_text(tbuf, text_x - tw, ry,
                      0.7f, 0.75f, 0.8f, 1.0f,
                      font_cache, "Arial", FONT_SIZE);
        }

        // Graph labels
        emit_text("0", graph_x + 2.0f, graph_y + GRAPH_HEIGHT - 12.0f,
                  0.5f, 0.5f, 0.5f, 0.8f,
                  font_cache, "Arial", FONT_SIZE);
        emit_text("33ms", graph_x + 2.0f, graph_y,
                  0.5f, 0.5f, 0.5f, 0.8f,
                  font_cache, "Arial", FONT_SIZE);
        emit_text("60fps", graph_x + graph_w - 36.0f, target_60_y - 12.0f,
                  0.2f, 0.6f, 0.2f, 0.6f,
                  font_cache, "Arial", FONT_SIZE);

        end_group();
    }

    // Upload to GPU
    if (quad_count_ > 0 && instance_mapped_[fi_]) {
        u32 bytes = quad_count_ * sizeof(UIInstance);
        std::memcpy(instance_mapped_[fi_], quads_.data(), bytes);
    }
}

void ProfileOverlay::render(VkCommandBuffer cmd, VkPipelineLayout layout,
                              u32 viewport_w, u32 viewport_h) {
    if (quad_count_ == 0 || groups_.empty()) return;

    f32 vp[2] = {static_cast<f32>(viewport_w),
                 static_cast<f32>(viewport_h)};
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                       sizeof(f32) * 2, vp);

    VkRect2D scissor{};
    scissor.extent = {viewport_w, viewport_h};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    VkBuffer buf = instance_buf_[fi_].buffer;
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &buf, &offset);

    for (auto& group : groups_) {
        if (group.count == 0) continue;
        if (group.ds) {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    layout, 0, 1, &group.ds, 0, nullptr);
        }
        vkCmdDraw(cmd, 6, group.count, 0, group.offset);
    }
}

} // namespace osc::renderer
