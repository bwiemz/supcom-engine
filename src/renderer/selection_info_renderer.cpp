#include "renderer/selection_info_renderer.hpp"
#include "renderer/army_colors.hpp"
#include "renderer/strategic_icon_renderer.hpp"
#include "renderer/font_cache.hpp"
#include "renderer/texture_cache.hpp"
#include "sim/sim_state.hpp"
#include "sim/entity.hpp"
#include "sim/unit.hpp"
#include "sim/unit_command.hpp"
#include "sim/army_brain.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace osc::renderer {

static void get_army_color(const sim::Entity& entity,
                           const sim::SimState& sim,
                           f32& r, f32& g, f32& b) {
    i32 army = entity.army();
    if (army >= 0 && army < static_cast<i32>(sim.army_count())) {
        auto* brain = sim.army_at(static_cast<size_t>(army));
        if (brain && (brain->color_r() || brain->color_g() ||
                      brain->color_b())) {
            r = brain->color_r() / 255.0f;
            g = brain->color_g() / 255.0f;
            b = brain->color_b() / 255.0f;
            return;
        }
        if (army < 8) {
            r = ARMY_COLORS[army][0];
            g = ARMY_COLORS[army][1];
            b = ARMY_COLORS[army][2];
            return;
        }
    }
    r = g = b = 0.7f;
}

static const char* command_name(sim::CommandType type) {
    switch (type) {
        case sim::CommandType::Stop:           return "Stop";
        case sim::CommandType::Move:           return "Moving";
        case sim::CommandType::Attack:         return "Attacking";
        case sim::CommandType::Guard:          return "Guarding";
        case sim::CommandType::Patrol:         return "Patrolling";
        case sim::CommandType::BuildMobile:    return "Building";
        case sim::CommandType::BuildFactory:   return "Producing";
        case sim::CommandType::Reclaim:        return "Reclaiming";
        case sim::CommandType::Repair:         return "Repairing";
        case sim::CommandType::Upgrade:        return "Upgrading";
        case sim::CommandType::Capture:        return "Capturing";
        case sim::CommandType::Enhance:        return "Enhancing";
        case sim::CommandType::TransportLoad:  return "Loading";
        case sim::CommandType::TransportUnload:return "Unloading";
        case sim::CommandType::Nuke:           return "Nuke";
        case sim::CommandType::Tactical:       return "Tactical";
        case sim::CommandType::Overcharge:     return "Overcharge";
        case sim::CommandType::Sacrifice:      return "Sacrificing";
        case sim::CommandType::Teleport:       return "Teleporting";
        case sim::CommandType::Ferry:          return "Ferrying";
        case sim::CommandType::Dive:           return "Diving";
    }
    return "Idle";
}

/// Extract a short display name from blueprint ID like "uel0001" -> "UEL0001"
/// or custom_name if set.
static std::string unit_display_name(const sim::Unit& unit) {
    if (!unit.custom_name().empty()) return unit.custom_name();
    // Uppercase the unit_id
    std::string name = unit.unit_id();
    for (char& c : name) c = static_cast<char>(std::toupper(static_cast<u8>(c)));
    return name;
}

void SelectionInfoRenderer::init(VkDevice /*device*/, VmaAllocator allocator) {
    VkBufferCreateInfo buf_info{};
    buf_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buf_info.size = MAX_INFO_QUADS * sizeof(UIInstance);
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

void SelectionInfoRenderer::destroy(VkDevice /*device*/, VmaAllocator allocator) {
    for (u32 i = 0; i < FRAMES_IN_FLIGHT; i++) {
        if (instance_buf_[i].buffer)
            vmaDestroyBuffer(allocator, instance_buf_[i].buffer,
                             instance_buf_[i].allocation);
        instance_buf_[i] = {};
        instance_mapped_[i] = nullptr;
    }
}

void SelectionInfoRenderer::emit_quad(f32 x, f32 y, f32 w, f32 h,
                                       f32 u0, f32 v0, f32 u1, f32 v1,
                                       f32 r, f32 g, f32 b, f32 a) {
    if (quad_count_ >= MAX_INFO_QUADS) return;
    UIInstance inst{};
    inst.rect[0] = x; inst.rect[1] = y; inst.rect[2] = w; inst.rect[3] = h;
    inst.uv[0] = u0; inst.uv[1] = v0; inst.uv[2] = u1; inst.uv[3] = v1;
    inst.color[0] = r; inst.color[1] = g; inst.color[2] = b; inst.color[3] = a;
    quads_.push_back(inst);
    quad_count_++;
}

void SelectionInfoRenderer::emit_solid(f32 x, f32 y, f32 w, f32 h,
                                        f32 r, f32 g, f32 b, f32 a) {
    emit_quad(x, y, w, h, 0.0f, 0.0f, 1.0f, 1.0f, r, g, b, a);
}

void SelectionInfoRenderer::begin_group(VkDescriptorSet ds) {
    current_ds_ = ds;
    current_group_start_ = quad_count_;
}

void SelectionInfoRenderer::end_group() {
    u32 count = quad_count_ - current_group_start_;
    if (count > 0) {
        groups_.push_back({current_ds_, current_group_start_, count});
    }
}

f32 SelectionInfoRenderer::emit_text(const std::string& text, f32 x, f32 y,
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

void SelectionInfoRenderer::build_single_unit(const sim::Unit& unit,
                                               FontCache& font_cache,
                                               TextureCache& tex_cache,
                                               VkDescriptorSet icon_atlas_ds,
                                               f32 px, f32 py,
                                               f32 pw, f32 /*ph*/) {
    VkDescriptorSet white_ds = tex_cache.fallback_descriptor();

    // --- Solid quads (panel bg, bars) ---
    begin_group(white_ds);

    // Panel background
    emit_solid(px, py, pw, PANEL_HEIGHT_SINGLE, 0.05f, 0.06f, 0.1f, 0.88f);

    // Army color accent bar at top
    f32 ar, ag, ab;
    // We need SimState for army color but don't have it here — use fallback
    // Actually, the caller should pass sim. Let's use the army index directly.
    i32 army = unit.army();
    if (army >= 0 && army < 8) {
        ar = ARMY_COLORS[army][0];
        ag = ARMY_COLORS[army][1];
        ab = ARMY_COLORS[army][2];
    } else {
        ar = ag = ab = 0.7f;
    }
    emit_solid(px, py, pw, 3.0f, ar, ag, ab, 1.0f);

    // Health bar
    f32 hp_frac = (unit.max_health() > 0)
                      ? unit.health() / unit.max_health()
                      : 1.0f;
    hp_frac = std::clamp(hp_frac, 0.0f, 1.0f);

    f32 bar_x = px + 42.0f;
    f32 bar_y = py + 28.0f;
    f32 bar_w = pw - 52.0f;
    f32 bar_h = 10.0f;

    // Bar bg
    emit_solid(bar_x, bar_y, bar_w, bar_h, 0.1f, 0.1f, 0.1f, 0.9f);

    // Bar fill (green → yellow → red)
    f32 fill_w = bar_w * hp_frac;
    f32 hr = (hp_frac < 0.5f) ? 1.0f : 1.0f - (hp_frac - 0.5f) * 2.0f;
    f32 hg = (hp_frac < 0.5f) ? hp_frac * 2.0f : 1.0f;
    if (fill_w > 0.5f)
        emit_solid(bar_x, bar_y, fill_w, bar_h, hr, hg, 0.0f, 0.9f);

    // HP text
    char hp_buf[48];
    std::snprintf(hp_buf, sizeof(hp_buf), "%.0f / %.0f",
                  unit.health(), unit.max_health());

    // Build progress bar (if being built)
    if (unit.is_being_built() || unit.fraction_complete() < 0.999f) {
        f32 bp_y = bar_y + bar_h + 4.0f;
        f32 bp_frac = std::clamp(unit.fraction_complete(), 0.0f, 1.0f);

        emit_solid(bar_x, bp_y, bar_w, 6.0f, 0.1f, 0.1f, 0.1f, 0.8f);
        f32 bp_fill = bar_w * bp_frac;
        if (bp_fill > 0.5f)
            emit_solid(bar_x, bp_y, bp_fill, 6.0f, 0.3f, 0.6f, 1.0f, 0.9f);
    }

    end_group();

    // --- Text labels (font atlas) ---
    auto* font_atlas = font_cache.get("Arial", FONT_SIZE);
    VkDescriptorSet font_ds = font_atlas ? font_atlas->descriptor_set : VK_NULL_HANDLE;

    if (font_ds) {
        begin_group(font_ds);

        // Unit name
        std::string name = unit_display_name(unit);
        emit_text(name, px + 42.0f, py + 8.0f,
                  0.9f, 0.9f, 0.95f, 1.0f,
                  font_cache, "Arial", FONT_SIZE);

        // HP numbers on bar
        emit_text(hp_buf, bar_x + 4.0f, bar_y - 1.0f,
                  0.8f, 0.8f, 0.8f, 0.9f,
                  font_cache, "Arial", FONT_SIZE_SMALL);

        // Build progress percentage
        if (unit.is_being_built() || unit.fraction_complete() < 0.999f) {
            char bp_buf[16];
            std::snprintf(bp_buf, sizeof(bp_buf), "%.0f%%",
                          unit.fraction_complete() * 100.0f);
            emit_text(bp_buf, bar_x + bar_w - 30.0f, bar_y + bar_h + 3.0f,
                      0.5f, 0.7f, 1.0f, 0.9f,
                      font_cache, "Arial", FONT_SIZE_SMALL);
        }

        // Current command status
        auto& cmds = unit.command_queue();
        const char* cmd_text = cmds.empty() ? "Idle" : command_name(cmds.front().type);
        emit_text(cmd_text, px + 42.0f, py + PANEL_HEIGHT_SINGLE - 24.0f,
                  0.6f, 0.7f, 0.6f, 0.8f,
                  font_cache, "Arial", FONT_SIZE_SMALL);

        // Weapon count
        if (unit.weapon_count() > 0) {
            char wbuf[32];
            std::snprintf(wbuf, sizeof(wbuf), "%d wpn", unit.weapon_count());
            emit_text(wbuf, px + pw - 60.0f, py + PANEL_HEIGHT_SINGLE - 24.0f,
                      0.7f, 0.6f, 0.5f, 0.7f,
                      font_cache, "Arial", FONT_SIZE_SMALL);
        }

        end_group();
    }

    // --- Icon (strategic icon type) ---
    if (icon_atlas_ds) {
        begin_group(icon_atlas_ds);

        auto icon_type = StrategicIconRenderer::classify_unit(unit);
        u32 icon_idx = static_cast<u32>(icon_type);
        constexpr u32 CELL = StrategicIconRenderer::ICON_CELL_SIZE;
        constexpr u32 AW = StrategicIconRenderer::ATLAS_W;
        f32 u0 = static_cast<f32>(icon_idx * CELL) / static_cast<f32>(AW);
        f32 u1 = static_cast<f32>((icon_idx + 1) * CELL) / static_cast<f32>(AW);

        emit_quad(px + 6.0f, py + 10.0f, 30.0f, 30.0f,
                  u0, 0.0f, u1, 1.0f,
                  ar, ag, ab, 1.0f);

        end_group();
    }
}

void SelectionInfoRenderer::build_multi_unit(const sim::SimState& sim,
                                              const std::unordered_set<u32>& selected_ids,
                                              FontCache& font_cache,
                                              TextureCache& tex_cache,
                                              VkDescriptorSet icon_atlas_ds,
                                              f32 px, f32 py,
                                              f32 pw, f32 /*ph*/) {
    VkDescriptorSet white_ds = tex_cache.fallback_descriptor();
    auto& registry = sim.entity_registry();

    // Count by icon type
    u32 type_counts[static_cast<u32>(StrategicIconType::COUNT)] = {};
    f32 total_hp = 0, total_max_hp = 0;

    for (u32 uid : selected_ids) {
        auto* e = registry.find(uid);
        if (!e || !e->is_unit() || e->destroyed()) continue;
        auto* unit = static_cast<const sim::Unit*>(e);
        auto itype = StrategicIconRenderer::classify_unit(*unit);
        type_counts[static_cast<u32>(itype)]++;
        total_hp += e->health();
        total_max_hp += e->max_health();
    }

    // --- Panel bg + aggregate HP bar ---
    begin_group(white_ds);

    emit_solid(px, py, pw, PANEL_HEIGHT_MULTI, 0.05f, 0.06f, 0.1f, 0.88f);

    // Aggregate health bar
    f32 hp_frac = (total_max_hp > 0) ? total_hp / total_max_hp : 1.0f;
    hp_frac = std::clamp(hp_frac, 0.0f, 1.0f);

    f32 bar_x = px + 10.0f;
    f32 bar_y = py + 28.0f;
    f32 bar_w = pw - 20.0f;
    f32 bar_h = 8.0f;

    emit_solid(bar_x, bar_y, bar_w, bar_h, 0.1f, 0.1f, 0.1f, 0.9f);
    f32 fill_w = bar_w * hp_frac;
    f32 hr = (hp_frac < 0.5f) ? 1.0f : 1.0f - (hp_frac - 0.5f) * 2.0f;
    f32 hg = (hp_frac < 0.5f) ? hp_frac * 2.0f : 1.0f;
    if (fill_w > 0.5f)
        emit_solid(bar_x, bar_y, fill_w, bar_h, hr, hg, 0.0f, 0.9f);

    end_group();

    // --- Text ---
    auto* font_atlas = font_cache.get("Arial", FONT_SIZE);
    VkDescriptorSet font_ds = font_atlas ? font_atlas->descriptor_set : VK_NULL_HANDLE;

    if (font_ds) {
        begin_group(font_ds);

        // Selection count header
        char header[64];
        std::snprintf(header, sizeof(header), "%u units selected",
                      static_cast<u32>(selected_ids.size()));
        emit_text(header, px + 10.0f, py + 8.0f,
                  0.9f, 0.9f, 0.95f, 1.0f,
                  font_cache, "Arial", FONT_SIZE);

        // Type breakdown text below icon grid
        static const char* TYPE_NAMES[] = {
            "Land", "Air", "Naval", "Engr", "ACU", "Struct", "Other"
        };
        static_assert(sizeof(TYPE_NAMES) / sizeof(TYPE_NAMES[0]) ==
                       static_cast<size_t>(StrategicIconType::COUNT),
                       "TYPE_NAMES must match StrategicIconType::COUNT");
        f32 text_y = py + PANEL_HEIGHT_MULTI - 22.0f;
        f32 text_x = px + 10.0f;
        for (u32 i = 0; i < static_cast<u32>(StrategicIconType::COUNT); i++) {
            if (type_counts[i] == 0) continue;
            char tbuf[32];
            std::snprintf(tbuf, sizeof(tbuf), "%s:%u ", TYPE_NAMES[i], type_counts[i]);
            f32 adv = emit_text(tbuf, text_x, text_y,
                                0.7f, 0.7f, 0.7f, 0.8f,
                                font_cache, "Arial", FONT_SIZE_SMALL);
            text_x += adv + 4.0f;
            if (text_x > px + pw - 20.0f) break;
        }

        end_group();
    }

    // --- Icon grid (strategic icons for each type with count) ---
    if (icon_atlas_ds) {
        begin_group(icon_atlas_ds);

        constexpr u32 CELL = StrategicIconRenderer::ICON_CELL_SIZE;
        constexpr u32 AW = StrategicIconRenderer::ATLAS_W;
        constexpr f32 ICON_SIZE = 24.0f;
        constexpr f32 ICON_PAD = 4.0f;

        f32 grid_x = px + 10.0f;
        f32 grid_y = py + 42.0f;

        for (u32 i = 0; i < static_cast<u32>(StrategicIconType::COUNT); i++) {
            if (type_counts[i] == 0) continue;

            f32 u0 = static_cast<f32>(i * CELL) / static_cast<f32>(AW);
            f32 u1 = static_cast<f32>((i + 1) * CELL) / static_cast<f32>(AW);

            emit_quad(grid_x, grid_y, ICON_SIZE, ICON_SIZE,
                      u0, 0.0f, u1, 1.0f,
                      0.8f, 0.8f, 0.9f, 0.9f);

            grid_x += ICON_SIZE + ICON_PAD;
            if (grid_x > px + pw - ICON_SIZE) {
                grid_x = px + 10.0f;
                grid_y += ICON_SIZE + ICON_PAD;
            }
        }

        end_group();
    }
}

void SelectionInfoRenderer::update(const sim::SimState& sim,
                                    const std::unordered_set<u32>* selected_ids,
                                    FontCache& font_cache, TextureCache& tex_cache,
                                    VkDescriptorSet icon_atlas_ds,
                                    u32 viewport_w, u32 viewport_h) {
    quads_.clear();
    groups_.clear();
    quad_count_ = 0;

    if (!selected_ids || selected_ids->empty()) return;

    quads_.reserve(MAX_INFO_QUADS);

    f32 sw = static_cast<f32>(viewport_w);
    f32 sh = static_cast<f32>(viewport_h);

    auto& registry = sim.entity_registry();

    u32 valid_count = 0;
    const sim::Unit* single_unit = nullptr;
    for (u32 uid : *selected_ids) {
        auto* e = registry.find(uid);
        if (!e || !e->is_unit() || e->destroyed()) continue;
        valid_count++;
        single_unit = static_cast<const sim::Unit*>(e);
    }

    if (valid_count == 0) return;
    if (valid_count > 1) single_unit = nullptr;

    bool is_single = (valid_count == 1 && single_unit);
    f32 panel_h = is_single ? PANEL_HEIGHT_SINGLE : PANEL_HEIGHT_MULTI;
    f32 panel_w = PANEL_WIDTH;
    f32 panel_x = (sw - panel_w) * 0.5f;
    f32 panel_y = sh - panel_h - PANEL_MARGIN_BOTTOM;

    if (is_single) {
        build_single_unit(*single_unit, font_cache, tex_cache, icon_atlas_ds,
                          panel_x, panel_y, panel_w, panel_h);
    } else {
        build_multi_unit(sim, *selected_ids, font_cache, tex_cache, icon_atlas_ds,
                         panel_x, panel_y, panel_w, panel_h);
    }

    // Upload to GPU
    if (!quads_.empty() && instance_mapped_[fi_]) {
        u32 count = std::min(quad_count_, MAX_INFO_QUADS);
        std::memcpy(instance_mapped_[fi_], quads_.data(),
                    count * sizeof(UIInstance));
    }
}

void SelectionInfoRenderer::render(VkCommandBuffer cmd, VkPipelineLayout layout,
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
