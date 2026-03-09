#include "renderer/strategic_icon_renderer.hpp"
#include "renderer/camera.hpp"
#include "renderer/texture_cache.hpp"
#include "sim/sim_state.hpp"
#include "sim/entity.hpp"
#include "sim/unit.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace osc::renderer {

// Default army colors (duplicated from unit_renderer.cpp / minimap_renderer.cpp)
static constexpr std::array<std::array<f32, 3>, 8> ARMY_COLORS = {{
    {0.2f, 0.4f, 1.0f},  // Blue
    {1.0f, 0.2f, 0.2f},  // Red
    {0.2f, 0.8f, 0.2f},  // Green
    {1.0f, 1.0f, 0.2f},  // Yellow
    {1.0f, 0.5f, 0.1f},  // Orange
    {0.7f, 0.2f, 0.9f},  // Purple
    {0.2f, 0.9f, 0.9f},  // Cyan
    {0.9f, 0.9f, 0.9f},  // White
}};

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

void StrategicIconRenderer::init(VkDevice /*device*/, VmaAllocator allocator) {
    VkBufferCreateInfo buf_info{};
    buf_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buf_info.size = MAX_ICON_QUADS * sizeof(UIInstance);
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

void StrategicIconRenderer::destroy(VkDevice /*device*/, VmaAllocator allocator) {
    for (u32 i = 0; i < FRAMES_IN_FLIGHT; i++) {
        if (instance_buf_[i].buffer)
            vmaDestroyBuffer(allocator, instance_buf_[i].buffer,
                             instance_buf_[i].allocation);
        instance_buf_[i] = {};
        instance_mapped_[i] = nullptr;
    }
}

// --- Procedural icon atlas generation ---

// Draw a filled circle centered in cell
static void draw_circle(u8* pixels, u32 atlas_w, u32 atlas_h,
                         u32 cx, u32 cy, u32 radius) {
    i32 r2 = static_cast<i32>(radius * radius);
    i32 icx = static_cast<i32>(cx);
    i32 icy = static_cast<i32>(cy);
    for (i32 dy = -static_cast<i32>(radius); dy <= static_cast<i32>(radius); dy++) {
        for (i32 dx = -static_cast<i32>(radius); dx <= static_cast<i32>(radius); dx++) {
            if (dx * dx + dy * dy <= r2) {
                i32 ipx = icx + dx;
                i32 ipy = icy + dy;
                if (ipx < 0 || ipy < 0 ||
                    static_cast<u32>(ipx) >= atlas_w ||
                    static_cast<u32>(ipy) >= atlas_h)
                    continue;
                u32 idx = (static_cast<u32>(ipy) * atlas_w + static_cast<u32>(ipx)) * 4;
                pixels[idx + 0] = 255;
                pixels[idx + 1] = 255;
                pixels[idx + 2] = 255;
                pixels[idx + 3] = 255;
            }
        }
    }
}

// Draw a filled convex polygon (scanline fill) - up to 8 vertices
static void draw_polygon(u8* pixels, u32 atlas_w, u32 atlas_h,
                          const f32 verts[][2], u32 vert_count) {
    // Find Y bounds
    f32 min_y = verts[0][1], max_y = verts[0][1];
    for (u32 i = 1; i < vert_count; i++) {
        if (verts[i][1] < min_y) min_y = verts[i][1];
        if (verts[i][1] > max_y) max_y = verts[i][1];
    }

    i32 iy_min = std::max(0, static_cast<i32>(min_y));
    i32 iy_max = std::min(static_cast<i32>(atlas_h) - 1,
                           static_cast<i32>(max_y));

    for (i32 y = iy_min; y <= iy_max; y++) {
        f32 fy = static_cast<f32>(y) + 0.5f;
        f32 x_min = 1e6f, x_max = -1e6f;

        for (u32 i = 0; i < vert_count; i++) {
            u32 j = (i + 1) % vert_count;
            f32 y0 = verts[i][1], y1 = verts[j][1];
            if ((y0 <= fy && y1 > fy) || (y1 <= fy && y0 > fy)) {
                f32 t = (fy - y0) / (y1 - y0);
                f32 x = verts[i][0] + t * (verts[j][0] - verts[i][0]);
                if (x < x_min) x_min = x;
                if (x > x_max) x_max = x;
            }
        }

        i32 ix_min = std::max(0, static_cast<i32>(x_min));
        i32 ix_max = std::min(static_cast<i32>(atlas_w) - 1,
                               static_cast<i32>(x_max));
        for (i32 x = ix_min; x <= ix_max; x++) {
            u32 idx = (static_cast<u32>(y) * atlas_w + static_cast<u32>(x)) * 4;
            pixels[idx + 0] = 255;
            pixels[idx + 1] = 255;
            pixels[idx + 2] = 255;
            pixels[idx + 3] = 255;
        }
    }
}

void StrategicIconRenderer::draw_icon_shape(u8* pixels, u32 atlas_w,
                                             u32 cell_x, u32 cell_y,
                                             u32 cell_size,
                                             StrategicIconType type) {
    f32 cx = static_cast<f32>(cell_x) + static_cast<f32>(cell_size) * 0.5f;
    f32 cy = static_cast<f32>(cell_y) + static_cast<f32>(cell_size) * 0.5f;
    f32 hs = static_cast<f32>(cell_size) * 0.4f; // half-size

    switch (type) {
    case StrategicIconType::Land: {
        // Diamond
        f32 verts[4][2] = {
            {cx, cy - hs}, {cx + hs, cy},
            {cx, cy + hs}, {cx - hs, cy}
        };
        draw_polygon(pixels, atlas_w, ATLAS_H, verts, 4);
        break;
    }
    case StrategicIconType::Air: {
        // Upward-pointing triangle (arrow)
        f32 verts[3][2] = {
            {cx, cy - hs},
            {cx + hs * 0.8f, cy + hs * 0.7f},
            {cx - hs * 0.8f, cy + hs * 0.7f}
        };
        draw_polygon(pixels, atlas_w, ATLAS_H, verts, 3);
        break;
    }
    case StrategicIconType::Naval: {
        // Downward chevron (boat-like)
        f32 verts[5][2] = {
            {cx - hs, cy - hs * 0.5f},
            {cx + hs, cy - hs * 0.5f},
            {cx + hs * 0.7f, cy + hs * 0.5f},
            {cx, cy + hs},
            {cx - hs * 0.7f, cy + hs * 0.5f}
        };
        draw_polygon(pixels, atlas_w, ATLAS_H, verts, 5);
        break;
    }
    case StrategicIconType::Engineer: {
        // Small diamond with circle (wrench-like composite)
        draw_circle(pixels, atlas_w, ATLAS_H,
                     static_cast<u32>(cx), static_cast<u32>(cy),
                     static_cast<u32>(hs * 0.45f));
        // Outer ring via larger circle minus inner
        // Just use a slightly larger filled circle for simplicity
        draw_circle(pixels, atlas_w, ATLAS_H,
                     static_cast<u32>(cx), static_cast<u32>(cy),
                     static_cast<u32>(hs * 0.8f));
        // Cut inner to make ring effect — draw inner as black
        // Actually, simpler: just draw a gear-like shape with small diamond + dots
        f32 verts[4][2] = {
            {cx, cy - hs * 0.5f}, {cx + hs * 0.5f, cy},
            {cx, cy + hs * 0.5f}, {cx - hs * 0.5f, cy}
        };
        draw_polygon(pixels, atlas_w, ATLAS_H, verts, 4);
        break;
    }
    case StrategicIconType::Commander: {
        // Star (8-pointed via two overlapping squares/diamonds)
        // Outer diamond
        f32 verts1[4][2] = {
            {cx, cy - hs}, {cx + hs, cy},
            {cx, cy + hs}, {cx - hs, cy}
        };
        draw_polygon(pixels, atlas_w, ATLAS_H, verts1, 4);
        // Rotated square overlay
        f32 s = hs * 0.65f;
        f32 verts2[4][2] = {
            {cx - s, cy - s}, {cx + s, cy - s},
            {cx + s, cy + s}, {cx - s, cy + s}
        };
        draw_polygon(pixels, atlas_w, ATLAS_H, verts2, 4);
        break;
    }
    case StrategicIconType::Structure: {
        // Square
        f32 s = hs * 0.8f;
        f32 verts[4][2] = {
            {cx - s, cy - s}, {cx + s, cy - s},
            {cx + s, cy + s}, {cx - s, cy + s}
        };
        draw_polygon(pixels, atlas_w, ATLAS_H, verts, 4);
        break;
    }
    case StrategicIconType::Generic:
    default: {
        // Circle
        draw_circle(pixels, atlas_w, ATLAS_H,
                     static_cast<u32>(cx), static_cast<u32>(cy),
                     static_cast<u32>(hs * 0.7f));
        break;
    }
    }
}

void StrategicIconRenderer::build_atlas(TextureCache& tex_cache) {
    std::vector<u8> pixels(ATLAS_W * ATLAS_H * 4, 0); // transparent black

    for (u32 i = 0; i < static_cast<u32>(StrategicIconType::COUNT); i++) {
        draw_icon_shape(pixels.data(), ATLAS_W,
                         i * ICON_CELL_SIZE, 0, ICON_CELL_SIZE,
                         static_cast<StrategicIconType>(i));
    }

    auto* tex = tex_cache.upload_rgba("__strategic_icon_atlas",
                                       pixels.data(), ATLAS_W, ATLAS_H);
    if (tex)
        atlas_ds_ = tex->descriptor_set;
}

// --- Unit classification ---

StrategicIconType StrategicIconRenderer::classify_unit(const sim::Unit& unit) {
    if (unit.has_category("COMMAND"))
        return StrategicIconType::Commander;
    if (unit.has_category("ENGINEER") || unit.has_category("CONSTRUCTION"))
        return StrategicIconType::Engineer;
    if (unit.has_category("STRUCTURE"))
        return StrategicIconType::Structure;
    if (unit.has_category("AIR"))
        return StrategicIconType::Air;
    if (unit.has_category("NAVAL"))
        return StrategicIconType::Naval;
    if (unit.has_category("LAND"))
        return StrategicIconType::Land;
    return StrategicIconType::Generic;
}

// --- Rendering ---

bool StrategicIconRenderer::world_to_screen(f32 wx, f32 wy, f32 wz,
                                              const std::array<f32, 16>& vp,
                                              f32 sw, f32 sh,
                                              f32& out_x, f32& out_y) {
    f32 cx = vp[0]*wx + vp[4]*wy + vp[8]*wz  + vp[12];
    f32 cy = vp[1]*wx + vp[5]*wy + vp[9]*wz  + vp[13];
    f32 cw = vp[3]*wx + vp[7]*wy + vp[11]*wz + vp[15];

    if (cw <= 0.001f) return false;

    f32 ndc_x = cx / cw;
    f32 ndc_y = cy / cw;

    out_x = (ndc_x + 1.0f) * 0.5f * sw;
    out_y = (ndc_y + 1.0f) * 0.5f * sh;
    return true;
}

void StrategicIconRenderer::emit_quad(f32 x, f32 y, f32 w, f32 h,
                                       f32 u0, f32 v0, f32 u1, f32 v1,
                                       f32 r, f32 g, f32 b, f32 a) {
    if (quad_count_ >= MAX_ICON_QUADS) return;
    UIInstance inst{};
    inst.rect[0] = x; inst.rect[1] = y; inst.rect[2] = w; inst.rect[3] = h;
    inst.uv[0] = u0; inst.uv[1] = v0; inst.uv[2] = u1; inst.uv[3] = v1;
    inst.color[0] = r; inst.color[1] = g; inst.color[2] = b; inst.color[3] = a;
    quads_.push_back(inst);
    quad_count_++;
}

bool StrategicIconRenderer::update(const sim::SimState& sim,
                                    const Camera& camera,
                                    const std::array<f32, 16>& vp_matrix,
                                    const std::unordered_set<u32>* selected_ids,
                                    TextureCache& tex_cache,
                                    u32 viewport_w, u32 viewport_h) {
    quads_.clear();
    quads_.reserve(MAX_ICON_QUADS);
    quad_count_ = 0;
    ring_count_ = 0;
    white_ds_ = tex_cache.fallback_descriptor();

    f32 cam_dist = camera.distance();
    strategic_zoom_active_ = (cam_dist >= ZOOM_THRESHOLD);
    if (!strategic_zoom_active_) return false;

    f32 sw = static_cast<f32>(viewport_w);
    f32 sh = static_cast<f32>(viewport_h);

    // Icon size scales slightly with zoom (smaller when more zoomed out)
    f32 icon_size = std::clamp(24.0f * (ZOOM_THRESHOLD / cam_dist), 10.0f, 32.0f);

    // Eye position for distance culling
    f32 eye_x, eye_y, eye_z;
    camera.eye_position(eye_x, eye_y, eye_z);

    auto& registry = sim.entity_registry();

    // Collect visible units with screen positions (two-pass: rings first, icons second)
    struct VisibleUnit {
        f32 sx, sy;
        f32 r, g, b;
        StrategicIconType icon_type;
        bool is_selected;
    };
    std::vector<VisibleUnit> visible;
    visible.reserve(256);

    registry.for_each([&](const sim::Entity& entity) {
        if (entity.destroyed() || !entity.is_unit()) return;

        auto pos = entity.position();

        // Distance cull (wider range for strategic view)
        f32 dx = pos.x - eye_x;
        f32 dz = pos.z - eye_z;
        if (dx * dx + dz * dz > 1200.0f * 1200.0f) return;

        // Project to screen
        f32 sx, sy;
        if (!world_to_screen(pos.x, pos.y, pos.z, vp_matrix, sw, sh, sx, sy))
            return;

        // Skip if off-screen
        if (sx < -icon_size || sx > sw + icon_size ||
            sy < -icon_size || sy > sh + icon_size)
            return;

        // Get army color
        f32 r, g, b;
        get_army_color(entity, sim, r, g, b);

        bool is_selected = selected_ids &&
                           selected_ids->count(entity.entity_id()) > 0;

        auto* unit = static_cast<const sim::Unit*>(&entity);
        visible.push_back({sx, sy, r, g, b, classify_unit(*unit), is_selected});
    });

    // Pass 1: Selection rings (drawn with white_ds, behind icons)
    // Limit rings to half the budget so icons always have room.
    constexpr u32 MAX_RINGS = MAX_ICON_QUADS / 2;
    for (auto& vu : visible) {
        if (!vu.is_selected) continue;
        if (quad_count_ >= MAX_RINGS) break;
        f32 ring_size = icon_size + 4.0f;
        f32 ring_half = ring_size * 0.5f;
        emit_quad(vu.sx - ring_half, vu.sy - ring_half, ring_size, ring_size,
                  0.0f, 0.0f, 1.0f, 1.0f,
                  0.2f, 1.0f, 0.2f, 0.5f);
    }
    ring_count_ = quad_count_;

    // Pass 2: Icon quads (drawn with atlas_ds)
    for (auto& vu : visible) {
        if (quad_count_ >= MAX_ICON_QUADS) break;

        u32 icon_idx = static_cast<u32>(vu.icon_type);
        f32 u0 = static_cast<f32>(icon_idx * ICON_CELL_SIZE) /
                  static_cast<f32>(ATLAS_W);
        f32 u1 = static_cast<f32>((icon_idx + 1) * ICON_CELL_SIZE) /
                  static_cast<f32>(ATLAS_W);

        f32 half = icon_size * 0.5f;
        f32 r = vu.r, g = vu.g, b = vu.b;
        if (vu.is_selected) {
            r = r * 0.5f + 0.5f;
            g = g * 0.5f + 0.5f;
            b = b * 0.5f + 0.5f;
        }

        emit_quad(vu.sx - half, vu.sy - half, icon_size, icon_size,
                  u0, 0.0f, u1, 1.0f,
                  r, g, b, 1.0f);
    }

    // Upload to GPU
    if (!quads_.empty() && instance_mapped_[fi_]) {
        u32 count = std::min(quad_count_, MAX_ICON_QUADS);
        std::memcpy(instance_mapped_[fi_], quads_.data(),
                    count * sizeof(UIInstance));
    }

    return true;
}

void StrategicIconRenderer::render(VkCommandBuffer cmd, VkPipelineLayout layout,
                                    u32 viewport_w, u32 viewport_h) {
    if (quad_count_ == 0) return;

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

    // Draw group 1: Selection rings (instances 0..ring_count_-1) with white texture
    if (ring_count_ > 0 && white_ds_) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                layout, 0, 1, &white_ds_, 0, nullptr);
        vkCmdDraw(cmd, 6, ring_count_, 0, 0);
    }

    // Draw group 2: Icon quads (instances ring_count_..quad_count_-1) with atlas
    u32 icon_count = quad_count_ - ring_count_;
    if (icon_count > 0 && atlas_ds_) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                layout, 0, 1, &atlas_ds_, 0, nullptr);
        vkCmdDraw(cmd, 6, icon_count, 0, ring_count_);
    }
}

} // namespace osc::renderer
