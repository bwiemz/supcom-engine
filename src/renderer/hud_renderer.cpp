#include "renderer/hud_renderer.hpp"
#include "renderer/font_cache.hpp"
#include "renderer/texture_cache.hpp"
#include "sim/sim_state.hpp"
#include "sim/army_brain.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace osc::renderer {

void HudRenderer::init(VkDevice /*device*/, VmaAllocator allocator) {
    VkBufferCreateInfo buf_info{};
    buf_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buf_info.size = MAX_HUD_QUADS * sizeof(UIInstance);
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

void HudRenderer::destroy(VkDevice /*device*/, VmaAllocator allocator) {
    for (u32 i = 0; i < FRAMES_IN_FLIGHT; i++) {
        if (instance_buf_[i].buffer)
            vmaDestroyBuffer(allocator, instance_buf_[i].buffer,
                             instance_buf_[i].allocation);
        instance_buf_[i] = {};
        instance_mapped_[i] = nullptr;
    }
}

void HudRenderer::emit_quad(f32 x, f32 y, f32 w, f32 h,
                              f32 u0, f32 v0, f32 u1, f32 v1,
                              f32 r, f32 g, f32 b, f32 a) {
    if (quad_count_ >= MAX_HUD_QUADS) return;
    UIInstance inst{};
    inst.rect[0] = x; inst.rect[1] = y; inst.rect[2] = w; inst.rect[3] = h;
    inst.uv[0] = u0; inst.uv[1] = v0; inst.uv[2] = u1; inst.uv[3] = v1;
    inst.color[0] = r; inst.color[1] = g; inst.color[2] = b; inst.color[3] = a;
    quads_.push_back(inst);
    quad_count_++;
}

void HudRenderer::emit_solid(f32 x, f32 y, f32 w, f32 h,
                               f32 r, f32 g, f32 b, f32 a) {
    emit_quad(x, y, w, h, 0.0f, 0.0f, 1.0f, 1.0f, r, g, b, a);
}

void HudRenderer::begin_group(VkDescriptorSet ds) {
    current_ds_ = ds;
    current_group_start_ = quad_count_;
}

void HudRenderer::end_group() {
    u32 count = quad_count_ - current_group_start_;
    if (count > 0) {
        groups_.push_back({current_ds_, current_group_start_, count});
    }
}

f32 HudRenderer::emit_text(const std::string& text, f32 x, f32 y,
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
            // Try space width for unknown glyphs
            auto sp = atlas->glyphs.find(32);
            if (sp != atlas->glyphs.end())
                cursor_x += sp->second.x_advance;
            continue;
        }

        auto& gi = it->second;

        f32 gx = cursor_x + gi.x_offset;
        f32 gy = baseline_y - gi.y_offset; // y_offset is from baseline upward

        if (gi.width > 0 && gi.height > 0) {
            emit_quad(gx, gy, gi.width, gi.height,
                      gi.u0, gi.v0, gi.u1, gi.v1,
                      r, g, b, a);
        }

        cursor_x += gi.x_advance;
    }

    return cursor_x - x;
}

/// Format a number compactly: 1234 -> "1234", 12345 -> "12.3k", 1234567 -> "1.23M"
static std::string format_number(f64 val) {
    char buf[32];
    f64 abs_val = std::abs(val);
    if (abs_val >= 1e6) {
        std::snprintf(buf, sizeof(buf), "%.1fM", val / 1e6);
    } else if (abs_val >= 10000.0) {
        std::snprintf(buf, sizeof(buf), "%.1fk", val / 1000.0);
    } else if (abs_val >= 100.0) {
        std::snprintf(buf, sizeof(buf), "%.0f", val);
    } else if (abs_val >= 1.0) {
        std::snprintf(buf, sizeof(buf), "%.1f", val);
    } else {
        std::snprintf(buf, sizeof(buf), "%.2f", val);
    }
    return buf;
}

void HudRenderer::update(const sim::SimState& sim, i32 player_army,
                           FontCache& font_cache, TextureCache& tex_cache,
                           u32 viewport_w, u32 /*viewport_h*/) {
    quads_.clear();
    groups_.clear();
    quad_count_ = 0;

    if (player_army < 0 || player_army >= static_cast<i32>(sim.army_count()))
        return;

    auto* brain = sim.army_at(static_cast<size_t>(player_army));
    if (!brain) return;

    auto& econ = brain->economy();
    VkDescriptorSet white_ds = tex_cache.fallback_descriptor();

    // Layout: centered at top of screen
    //   [Mass icon area] [Mass bar] [Mass text]  gap  [Energy icon area] [Energy bar] [Energy text]
    f32 sw = static_cast<f32>(viewport_w);
    f32 total_w = BAR_WIDTH * 2.0f + BAR_SPACING * 3.0f + 120.0f; // 2 bars + text areas
    f32 start_x = (sw - total_w) * 0.5f;
    if (start_x < 10.0f) start_x = 10.0f;

    // Font atlas for text
    auto* font_atlas = font_cache.get("Arial", FONT_SIZE);
    VkDescriptorSet font_ds = font_atlas ? font_atlas->descriptor_set : VK_NULL_HANDLE;

    // --- Mass bar ---
    f32 mass_x = start_x;
    f32 bar_y = BAR_Y;

    f64 mass_ratio = (econ.mass.max_storage > 0)
                         ? econ.mass.stored / econ.mass.max_storage
                         : 0.0;
    mass_ratio = std::clamp(mass_ratio, 0.0, 1.0);

    // Mass: green accent
    f32 mass_r = 0.3f, mass_g = 0.9f, mass_b = 0.3f;

    // Draw solid quads for mass bar
    begin_group(white_ds);

    // Mass label background
    emit_solid(mass_x, bar_y, BAR_WIDTH + 60.0f, BAR_HEIGHT + 20.0f,
               0.05f, 0.08f, 0.05f, 0.85f);

    // Mass bar background (dark)
    f32 inner_x = mass_x + 4.0f;
    f32 inner_y = bar_y + 4.0f;
    emit_solid(inner_x, inner_y, BAR_WIDTH, BAR_HEIGHT,
               0.1f, 0.12f, 0.1f, 0.9f);

    // Mass bar fill
    f32 fill_w = BAR_WIDTH * static_cast<f32>(mass_ratio);
    if (fill_w > 0.5f) {
        emit_solid(inner_x, inner_y, fill_w, BAR_HEIGHT,
                   mass_r * 0.7f, mass_g * 0.7f, mass_b * 0.7f, 0.9f);
    }

    // Income indicator (thin green bar at top)
    f32 income_frac = (econ.mass.max_storage > 0)
                          ? static_cast<f32>(econ.mass.income / econ.mass.max_storage)
                          : 0.0f;
    income_frac = std::clamp(income_frac, 0.0f, 1.0f);
    if (income_frac > 0.001f) {
        emit_solid(inner_x, inner_y + BAR_HEIGHT - 2.0f,
                   BAR_WIDTH * income_frac, 2.0f,
                   0.4f, 1.0f, 0.4f, 0.8f);
    }

    // --- Energy bar ---
    f32 energy_x = mass_x + BAR_WIDTH + 68.0f + BAR_SPACING;

    f64 energy_ratio = (econ.energy.max_storage > 0)
                           ? econ.energy.stored / econ.energy.max_storage
                           : 0.0;
    energy_ratio = std::clamp(energy_ratio, 0.0, 1.0);

    // Energy: yellow/gold accent
    f32 en_r = 1.0f, en_g = 0.85f, en_b = 0.2f;

    // Energy label background
    emit_solid(energy_x, bar_y, BAR_WIDTH + 60.0f, BAR_HEIGHT + 20.0f,
               0.08f, 0.07f, 0.02f, 0.85f);

    // Energy bar background
    f32 en_inner_x = energy_x + 4.0f;
    f32 en_inner_y = bar_y + 4.0f;
    emit_solid(en_inner_x, en_inner_y, BAR_WIDTH, BAR_HEIGHT,
               0.12f, 0.1f, 0.04f, 0.9f);

    // Energy bar fill
    f32 en_fill_w = BAR_WIDTH * static_cast<f32>(energy_ratio);
    if (en_fill_w > 0.5f) {
        emit_solid(en_inner_x, en_inner_y, en_fill_w, BAR_HEIGHT,
                   en_r * 0.7f, en_g * 0.7f, en_b * 0.7f, 0.9f);
    }

    // Energy income indicator
    f32 en_income_frac = (econ.energy.max_storage > 0)
                             ? static_cast<f32>(econ.energy.income / econ.energy.max_storage)
                             : 0.0f;
    en_income_frac = std::clamp(en_income_frac, 0.0f, 1.0f);
    if (en_income_frac > 0.001f) {
        emit_solid(en_inner_x, en_inner_y + BAR_HEIGHT - 2.0f,
                   BAR_WIDTH * en_income_frac, 2.0f,
                   1.0f, 1.0f, 0.5f, 0.8f);
    }

    end_group();

    // --- Text labels (requires font atlas) ---
    if (font_ds) {
        begin_group(font_ds);

        // Mass stored / max
        std::string mass_text = format_number(econ.mass.stored) + " / " +
                                format_number(econ.mass.max_storage);
        f32 text_y = bar_y + 3.0f;
        emit_text(mass_text, inner_x + BAR_WIDTH + 6.0f, text_y,
                  mass_r, mass_g, mass_b, 1.0f,
                  font_cache, "Arial", FONT_SIZE);

        // Mass income/expense below bar
        std::string mass_flow = "+" + format_number(econ.mass.income) +
                                " / -" + format_number(econ.mass.requested);
        emit_text(mass_flow, mass_x + 4.0f, bar_y + BAR_HEIGHT + 6.0f,
                  0.6f, 0.8f, 0.6f, 0.8f,
                  font_cache, "Arial", FONT_SIZE - 2);

        // Energy stored / max
        std::string energy_text = format_number(econ.energy.stored) + " / " +
                                  format_number(econ.energy.max_storage);
        emit_text(energy_text, en_inner_x + BAR_WIDTH + 6.0f, text_y,
                  en_r, en_g, en_b, 1.0f,
                  font_cache, "Arial", FONT_SIZE);

        // Energy income/expense below bar
        std::string energy_flow = "+" + format_number(econ.energy.income) +
                                  " / -" + format_number(econ.energy.requested);
        emit_text(energy_flow, energy_x + 4.0f, bar_y + BAR_HEIGHT + 6.0f,
                  0.8f, 0.75f, 0.4f, 0.8f,
                  font_cache, "Arial", FONT_SIZE - 2);

        // Efficiency indicators (if stalling)
        if (brain->mass_efficiency() < 0.99) {
            char eff_buf[32];
            std::snprintf(eff_buf, sizeof(eff_buf), "%.0f%%",
                          brain->mass_efficiency() * 100.0);
            emit_text(eff_buf, mass_x + BAR_WIDTH * 0.5f - 10.0f, inner_y + 1.0f,
                      1.0f, 0.3f, 0.3f, 1.0f,
                      font_cache, "Arial", FONT_SIZE);
        }
        if (brain->energy_efficiency() < 0.99) {
            char eff_buf[32];
            std::snprintf(eff_buf, sizeof(eff_buf), "%.0f%%",
                          brain->energy_efficiency() * 100.0);
            emit_text(eff_buf, energy_x + BAR_WIDTH * 0.5f - 10.0f, en_inner_y + 1.0f,
                      1.0f, 0.3f, 0.3f, 1.0f,
                      font_cache, "Arial", FONT_SIZE);
        }

        end_group();
    }

    // Upload to GPU
    if (!quads_.empty() && instance_mapped_[fi_]) {
        u32 count = std::min(quad_count_, MAX_HUD_QUADS);
        std::memcpy(instance_mapped_[fi_], quads_.data(),
                    count * sizeof(UIInstance));
    }
}

void HudRenderer::render(VkCommandBuffer cmd, VkPipelineLayout layout,
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
