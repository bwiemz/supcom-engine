#include "renderer/overlay_renderer.hpp"
#include "renderer/renderer.hpp"
#include "sim/build_placement.hpp"
#include "renderer/vk_cmd.hpp"
#include "renderer/beam_renderer.hpp"
#include "renderer/particle_system.hpp"
#include "renderer/trail_renderer.hpp"
#include "renderer/camera.hpp"
#include "renderer/recon_view.hpp"
#include "renderer/texture_cache.hpp"
#include "sim/world_snapshot.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace osc::renderer {

std::vector<std::array<f32, 4>> line_runs(f32 x0, f32 y0, f32 x1, f32 y1, f32 thick, u32 max_runs) {
    std::vector<std::array<f32, 4>> runs;
    const f32 dx = x1 - x0;
    const f32 dy = y1 - y0;
    const f32 width = std::max(2.0f * thick, 1.0f);
    const bool x_major = std::abs(dx) >= std::abs(dy);
    const f32 minor = x_major ? std::abs(dy) : std::abs(dx);
    const u32 steps =
        std::clamp(static_cast<u32>(std::ceil(minor / width)), 1u, std::max(max_runs, 1u));
    runs.reserve(steps);
    for (u32 i = 0; i < steps; ++i) {
        const f32 t0 = static_cast<f32>(i) / static_cast<f32>(steps);
        const f32 t1 = static_cast<f32>(i + 1) / static_cast<f32>(steps);
        const f32 xa = x0 + dx * t0;
        const f32 xb = x0 + dx * t1;
        const f32 ya = y0 + dy * t0;
        const f32 yb = y0 + dy * t1;
        // Along the main axis the step's span; across it, the step's span
        // and the line's width about it
        const f32 half = width * 0.5f;
        const f32 left = std::min(xa, xb) - (x_major ? 0.0f : half);
        const f32 right = std::max(xa, xb) + (x_major ? 0.0f : half);
        const f32 top = std::min(ya, yb) - (x_major ? half : 0.0f);
        const f32 bottom = std::max(ya, yb) + (x_major ? half : 0.0f);
        runs.push_back({left, top, right - left, bottom - top});
    }
    return runs;
}

void OverlayRenderer::init(VkDevice device, VmaAllocator allocator) {
    VkBufferCreateInfo buf_info{};
    buf_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buf_info.size = MAX_OVERLAY_QUADS * sizeof(UIInstance);
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

void OverlayRenderer::destroy(VkDevice /*device*/, VmaAllocator allocator) {
    for (u32 i = 0; i < FRAMES_IN_FLIGHT; i++) {
        if (instance_buf_[i].buffer)
            vmaDestroyBuffer(allocator, instance_buf_[i].buffer,
                             instance_buf_[i].allocation);
        instance_buf_[i] = {};
        instance_mapped_[i] = nullptr;
    }
}

bool OverlayRenderer::world_to_screen(f32 wx, f32 wy, f32 wz,
                                       const std::array<f32, 16>& vp,
                                       f32 sw, f32 sh,
                                       f32& out_x, f32& out_y) {
    // Column-major MVP multiply: clip = VP * [wx, wy, wz, 1]
    f32 cx = vp[0]*wx + vp[4]*wy + vp[8]*wz  + vp[12];
    f32 cy = vp[1]*wx + vp[5]*wy + vp[9]*wz  + vp[13];
    // cz unused for 2D projection
    f32 cw = vp[3]*wx + vp[7]*wy + vp[11]*wz + vp[15];

    if (cw <= 0.001f) return false; // behind camera

    f32 ndc_x = cx / cw;
    f32 ndc_y = cy / cw;

    // NDC [-1,1] → screen pixels
    // Our projection Y-flips, so ndc_y=-1 → top (0), ndc_y=+1 → bottom (sh)
    out_x = (ndc_x + 1.0f) * 0.5f * sw;
    out_y = (ndc_y + 1.0f) * 0.5f * sh;
    return true;
}

std::vector<std::array<f32, 4>> convex_rows(const std::array<f32, 4>& xs,
                                            const std::array<f32, 4>& ys, f32 row) {
    std::vector<std::array<f32, 4>> rows;
    const f32 top = *std::min_element(ys.begin(), ys.end());
    const f32 bottom = *std::max_element(ys.begin(), ys.end());
    const int count = row > 0.0f ? static_cast<int>(std::ceil((bottom - top) / row)) : 0;
    for (int k = 0; k < count; ++k) {
        const f32 y = top + static_cast<f32>(k) * row;
        const f32 mid = y + row * 0.5f;
        f32 left = 1e9f;
        f32 right = -1e9f;
        for (size_t i = 0; i < 4; ++i) {
            const size_t j = (i + 1) % 4;
            const f32 y0 = ys[i];
            const f32 y1 = ys[j];
            if ((mid < y0) == (mid < y1)) {
                continue;
            }
            const f32 x = xs[i] + (xs[j] - xs[i]) * (mid - y0) / (y1 - y0);
            left = std::min(left, x);
            right = std::max(right, x);
        }
        if (right > left) {
            rows.push_back({left, y, right - left, row});
        }
    }
    return rows;
}

std::vector<std::array<f32, 4>> outline_rows(const std::array<f32, 4>& xs,
                                             const std::array<f32, 4>& ys, f32 thickness) {
    std::vector<std::array<f32, 4>> rows;
    for (size_t i = 0; i < 4; ++i) {
        const size_t j = (i + 1) % 4;
        const f32 dx = xs[j] - xs[i];
        const f32 dy = ys[j] - ys[i];
        const f32 len = std::sqrt(dx * dx + dy * dy);
        if (len < 1e-3f) {
            continue;
        }
        const f32 nx = -dy / len * thickness * 0.5f;
        const f32 ny = dx / len * thickness * 0.5f;
        const auto edge = convex_rows({xs[i] + nx, xs[j] + nx, xs[j] - nx, xs[i] - nx},
                                      {ys[i] + ny, ys[j] + ny, ys[j] - ny, ys[i] - ny}, 1.0f);
        rows.insert(rows.end(), edge.begin(), edge.end());
    }
    return rows;
}

std::vector<std::pair<sim::StructureSite, f32>> structure_pads(const sim::FrameView& view,
                                                               const ReconView* recon) {
    std::vector<std::pair<sim::StructureSite, f32>> pads;
    for (const sim::EntityRecord& e : view.entities()) {
        if (!e.is_unit || !e.is_structure || (recon && recon->sight(e) != Sight::Seen)) {
            continue;
        }
        const sim::Vector3 pos = view.position(e);
        pads.emplace_back(sim::StructureSite::of(pos.x, pos.z, e.footprint_size_x,
                                                 e.footprint_size_z, e.skirt_size_x, e.skirt_size_z,
                                                 e.skirt_offset_x, e.skirt_offset_z),
                          pos.y);
    }
    return pads;
}

void OverlayRenderer::emit_quad(f32 x, f32 y, f32 w, f32 h,
                                 f32 r, f32 g, f32 b, f32 a) {
    if (quad_count_ >= MAX_OVERLAY_QUADS) return;
    UIInstance inst{};
    inst.rect[0] = x; inst.rect[1] = y; inst.rect[2] = w; inst.rect[3] = h;
    inst.uv[0] = 0; inst.uv[1] = 0; inst.uv[2] = 1; inst.uv[3] = 1;
    inst.color[0] = r; inst.color[1] = g; inst.color[2] = b; inst.color[3] = a;
    quads_.push_back(inst);
    quad_count_++;
}

void OverlayRenderer::emit_line(f32 x0, f32 y0, f32 x1, f32 y1, f32 thick, f32 r, f32 g, f32 b,
                                f32 a) {
    for (const auto& run : line_runs(x0, y0, x1, y1, thick)) {
        emit_quad(run[0], run[1], run[2], run[3], r, g, b, a);
    }
}

std::vector<bool> adjacency_lit(const std::vector<sim::StructureSite>& pads,
                                const sim::StructureSite& ghost) {
    std::vector<bool> lit(pads.size(), false);
    for (size_t i = 0; i < pads.size(); ++i) {
        lit[i] = pads[i].touches(ghost);
    }
    return lit;
}

void OverlayRenderer::emit_outline(const std::array<f32, 4>& xs, const std::array<f32, 4>& ys,
                                   f32 r, f32 g, f32 b, f32 a) {
    for (const auto& q : outline_rows(xs, ys, kOutlineThickness)) {
        emit_quad(q[0], q[1], q[2], q[3], r, g, b, a);
    }
}

namespace {

/// Moho's lifebar console defaults (faf-re CWldSession.cpp): ui_LifebarWidth,
/// ui_lifebarHeight and ui_LifebarOffset in world units, ui_LifebarLOD the
/// zoom bars show below, and the ui_LifeBar*/ui_*BarColor colours.
constexpr f32 kLifebarWidth = 1.5f;
constexpr f32 kLifebarHeight = 0.125f;
constexpr f32 kLifebarOffset = 0.1f;
constexpr f32 kLifebarLod = 200.0f;
constexpr f32 kLifebarRowGap = 2.0f;
constexpr f32 kLifebarMinFill = 2.0f;
constexpr u32 kLifebarBackdrop = 0xFF000000u;
constexpr u32 kLifeBarGood = 0xFF00FF00u;
constexpr u32 kLifeBarMed = 0xFFFFFF00u;
constexpr u32 kLifeBarBad = 0xFFFF0000u;
constexpr f32 kLifeBarGoodCutoff = 0.75f;
constexpr f32 kLifeBarBadCutoff = 0.25f;
constexpr u32 kFuelBar = 0xFFF4EC4Du;
constexpr u32 kFuelWarning = 0xFFFF0000u;
constexpr u32 kShieldBar = 0xFF00C3F7u;
constexpr u32 kProgressBar = 0xFFFF9900u;
constexpr f32 kFuelEmptyBlinkRate = 0.1f;

} // namespace

void OverlayRenderer::emit_argb(f32 left, f32 top, f32 right, f32 bottom, u32 argb) {
    emit_quad(left, top, right - left, bottom - top, static_cast<f32>((argb >> 16) & 0xFF) / 255.0f,
              static_cast<f32>((argb >> 8) & 0xFF) / 255.0f, static_cast<f32>(argb & 0xFF) / 255.0f,
              static_cast<f32>((argb >> 24) & 0xFF) / 255.0f);
}

void OverlayRenderer::emit_lifebars(const sim::EntityRecord& e, const sim::Vector3& pos,
                                    const Camera& camera, const std::array<f32, 16>& vp, f32 sw,
                                    f32 sh, f64 sim_time) {
    // Who gets bars (CWldSession::RenderStrategicIcons): zoomed in closer
    // than ui_LifebarLOD, not dead, its blueprint's LifeBarRender; the
    // player's and allies' units, another's only while hovered (its health
    // known: the caller drew only seen ones); none for a unit being upgraded
    // (the upgrade's own progress shows), one on a transport, or one whose
    // Display.HideLifebars.
    if (!unit_bars_ || camera.zoom() >= kLifebarLod || e.is_dying || !e.life_bar_render) return;
    const bool friendly = !recon_ || recon_->friendly(e.army);
    if (!friendly && e.id != hovered_) return;
    if (e.being_upgraded || e.attached || e.hide_lifebars) return;

    // Its size in world units at its own depth, so bars shrink as the
    // camera pulls back; the stack hangs below the unit along the camera's
    // up axis by its blueprint's LifeBarOffset (DrawUnitLifebars).
    const auto view = camera.view();
    const f32 right[3] = {view[0], view[4], view[8]};
    const f32 up[3] = {view[1], view[5], view[9]};
    f32 cx = 0, cy = 0, rx = 0, ry = 0;
    if (!world_to_screen(pos.x, pos.y, pos.z, vp, sw, sh, cx, cy) ||
        !world_to_screen(pos.x + right[0], pos.y + right[1], pos.z + right[2], vp, sw, sh, rx,
                         ry)) {
        return;
    }
    const f32 px_per_unit = std::hypot(rx - cx, ry - cy);
    const f32 bar_w = px_per_unit * (e.life_bar_size > 0 ? e.life_bar_size : kLifebarWidth);
    const f32 bar_h = px_per_unit * (e.life_bar_height > 0 ? e.life_bar_height : kLifebarHeight);
    const f32 drop = e.life_bar_offset + kLifebarOffset;
    f32 ax = 0, ay = 0;
    if (!world_to_screen(pos.x - up[0] * drop, pos.y - up[1] * drop, pos.z - up[2] * drop, vp, sw,
                         sh, ax, ay)) {
        return;
    }
    ax = std::floor(ax);
    ay = std::floor(ay);
    const f32 left = ax - bar_w * 0.5f;
    const f32 right_edge = left + bar_w;
    const f32 top = ay - bar_h * 0.5f;

    f32 health = e.max_health > 0 ? e.health / e.max_health : 1.0f;
    if (!(health < 1.0f)) health = 1.0f;
    if (health < 0.0f) health = 0.0f;
    const u32 health_color = health > kLifeBarGoodCutoff  ? kLifeBarGood
                             : health > kLifeBarBadCutoff ? kLifeBarMed
                                                          : kLifeBarBad;

    // Rows two and three: a shield's, then fuel (or, with no fuel at all,
    // work progress); without a shield, one row of whichever of fuel and
    // work progress is further along. Empty fuel blinks in the warning
    // colour, full.
    constexpr f32 kNoFuel = -1.0f;
    const bool blink = std::fmod(sim_time * kFuelEmptyBlinkRate, 1.0) > 0.5;
    f32 second = 0, third = 0;
    u32 second_color = 0, third_color = 0;
    if (e.is_unit) {
        if (e.shield_ratio > 0) {
            second_color = kShieldBar;
            second = e.shield_ratio;
            if (!(e.fuel_ratio > kNoFuel)) {
                third_color = kProgressBar;
                third = e.work_progress;
            } else {
                third_color = kFuelBar;
                third = e.fuel_ratio;
                if (!(e.fuel_ratio > 0.0f) && blink) {
                    third_color = kFuelWarning;
                    third = 1.0f;
                }
            }
        } else {
            if (e.fuel_ratio > e.work_progress) {
                second_color = kFuelBar;
                second = e.fuel_ratio;
            } else {
                second_color = kProgressBar;
                second = e.work_progress;
            }
            if (e.fuel_ratio > kNoFuel && !(e.fuel_ratio > 0.0f) && blink) {
                second_color = kFuelWarning;
                second = 1.0f;
            }
        }
        second = std::clamp(second, 0.0f, 1.0f);
        third = std::clamp(third, 0.0f, 1.0f);
    }

    const f32 second_top = top + bar_h + kLifebarRowGap;
    const f32 third_top = second_top + bar_h + kLifebarRowGap;
    emit_argb(left, top, right_edge, top + bar_h, kLifebarBackdrop);
    if (second > 0) {
        emit_argb(left, second_top, right_edge, second_top + bar_h, kLifebarBackdrop);
        if (third > 0) emit_argb(left, third_top, right_edge, third_top + bar_h, kLifebarBackdrop);
    }
    // The fills, inset a pixel
    const f32 track = bar_w - 1.0f;
    const f32 fill_h = std::max(kLifebarMinFill, (bar_h - kLifebarRowGap) + 1.0f);
    emit_argb(left + 1.0f, top + 1.0f, left + health * track, top + fill_h, health_color);
    if (second > 0) {
        emit_argb(left + 1.0f, second_top + 1.0f, left + second * track, second_top + fill_h,
                  second_color);
        if (third > 0)
            emit_argb(left + 1.0f, third_top + 1.0f, left + third * track, third_top + fill_h,
                      third_color);
    }
}

void OverlayRenderer::update(const sim::FrameView& view, sim::WorldEvents& events,
                             const Camera& camera, const std::array<f32, 16>& vp_matrix,
                             const std::unordered_set<u32>* selected_ids, TextureCache& tex_cache,
                             u32 viewport_w, u32 viewport_h, i32 game_result, f32 dt,
                             const Frustum* frustum, const BuildGhost* ghost) {
    quads_.clear();
    quads_.reserve(MAX_OVERLAY_QUADS);
    quad_count_ = 0;
    white_ds_ = tex_cache.fallback_descriptor();
    static const sim::WorldSnapshot kNoWorld;
    const sim::WorldSnapshot& snap = view.cur() ? *view.cur() : kNoWorld;

    f32 sw = static_cast<f32>(viewport_w);
    f32 sh = static_cast<f32>(viewport_h);

    // Camera distance for LOD (skip overlays when very far)
    f32 cam_dist = camera.eye_distance();

    // Eye position for distance culling
    f32 eye_x, eye_y, eye_z;
    camera.eye_position(eye_x, eye_y, eye_z);

    if (ghost) {
        constexpr f32 kDim = 216.0f / 255.0f;
        const auto pads = structure_pads(view, recon_);
        std::vector<sim::StructureSite> pad_sites;
        pad_sites.reserve(pads.size());
        for (const auto& pad : pads) {
            pad_sites.push_back(pad.first);
        }
        std::vector<const BuildGhost*> ghost_sites{ghost};
        for (const BuildGhost& site : ghost->line) {
            ghost_sites.push_back(&site);
        }
        std::vector<bool> lit(pad_sites.size(), false);
        std::vector<bool> ghost_lit(ghost_sites.size(), false);
        for (size_t ghost_index = 0; ghost_index < ghost_sites.size(); ++ghost_index) {
            const BuildGhost& site = *ghost_sites[ghost_index];
            if (!site.valid) continue;
            const sim::StructureSite site_pad{site.pad_x0, site.pad_z0, site.pad_x1, site.pad_z1};
            const auto site_lit = adjacency_lit(pad_sites, site_pad);
            for (size_t pad_index = 0; pad_index < site_lit.size(); ++pad_index) {
                lit[pad_index] = lit[pad_index] || site_lit[pad_index];
                ghost_lit[ghost_index] = ghost_lit[ghost_index] || site_lit[pad_index];
            }
        }
        const auto outline = [&](const sim::StructureSite& pad, f32 y, f32 r, f32 g, f32 a) {
            const std::array<std::array<f32, 2>, 4> corners = {
                {{pad.x0, pad.z0}, {pad.x1, pad.z0}, {pad.x1, pad.z1}, {pad.x0, pad.z1}}};
            std::array<f32, 4> xs{};
            std::array<f32, 4> ys{};
            for (size_t i = 0; i < 4; ++i) {
                if (!world_to_screen(corners[i][0], y, corners[i][1], vp_matrix, sw, sh, xs[i],
                                     ys[i])) {
                    return;
                }
            }
            emit_outline(xs, ys, r, g, 0.0f, a);
        };
        for (size_t i = 0; i < ghost_sites.size(); ++i) {
            const BuildGhost& site = *ghost_sites[i];
            const sim::StructureSite site_pad{site.pad_x0, site.pad_z0, site.pad_x1, site.pad_z1};
            if (!site.valid) {
                outline(site_pad, site.y, kDim, 0.0f, kDim);
            } else if (ghost_lit[i]) {
                outline(site_pad, site.y, 0.0f, 1.0f, 1.0f);
            } else {
                outline(site_pad, site.y, 0.0f, kDim, kDim);
            }
        }
        for (size_t i = 0; i < pads.size(); ++i) {
            outline(pads[i].first, pads[i].second, 0.0f, lit[i] ? 1.0f : kDim,
                    lit[i] ? 1.0f : kDim);
        }
    }

    // A death draws nothing of the engine's own: its effects are the unit
    // script's (FA's CreateDefault*Explosion emitters, through the particle
    // system). The flash the engine drew in their stead before they worked
    // showed as a square.
    events.deaths.clear();

    // Iterate all entities for health bars + selection circles
    for (const sim::EntityRecord& entity : view.entities()) {
        if (!entity.is_unit) continue;
        if (recon_ && recon_->sight(entity) != Sight::Seen) continue;

        auto pos = view.position(entity);

        // Frustum cull
        if (frustum && !frustum->is_sphere_visible(pos.x, pos.y, pos.z, 10.0f)) continue;

        bool is_selected = selected_ids &&
                           selected_ids->count(entity.id) > 0;

        // Project unit position to screen
        f32 sx, sy;
        if (!world_to_screen(pos.x, pos.y, pos.z, vp_matrix, sw, sh, sx, sy))
            continue;

        // --- Lifebars (Moho's lifebar pass) ---
        emit_lifebars(entity, pos, camera, vp_matrix, sw, sh,
                      static_cast<f64>(snap.tick) + static_cast<f64>(view.alpha()));
        const sim::EntityRecord* unit = &entity;
        f32 hp_frac = (entity.max_health > 0) ? entity.health / entity.max_health : 1.0f;

        // --- Veterancy indicators (gold chevrons above health bar) ---
        if (unit_bars_ && unit->vet_level > 0 && cam_dist < 400.0f &&
            (hp_frac < 0.999f || is_selected)) {
            constexpr f32 CHEV_SIZE = 5.0f;  // each chevron square
            constexpr f32 CHEV_GAP  = 1.5f;  // gap between chevrons
            constexpr f32 CHEV_Y_OFFSET = 26.0f; // above health bar
            u8 vl = unit->vet_level;
            if (vl > 5) vl = 5;
            f32 total_w = vl * CHEV_SIZE + (vl - 1) * CHEV_GAP;
            f32 start_x = sx - total_w * 0.5f;
            f32 cy = sy - CHEV_Y_OFFSET;
            for (u8 v = 0; v < vl; v++) {
                f32 cx = start_x + v * (CHEV_SIZE + CHEV_GAP);
                emit_quad(cx, cy, CHEV_SIZE, CHEV_SIZE,
                          1.0f, 0.85f, 0.1f, 0.9f); // gold
            }
        }

        // --- Transport cargo indicators (small dots below unit) ---
        if (unit_bars_ && unit->cargo_count > 0 && cam_dist < 400.0f) {
            constexpr f32 CARGO_DOT = 4.0f;
            constexpr f32 CARGO_GAP = 2.0f;
            constexpr f32 CARGO_Y = 8.0f; // below unit center
            u32 cargo_count = unit->cargo_count;
            if (cargo_count > 8) cargo_count = 8; // cap display at 8
            f32 total_cw = cargo_count * CARGO_DOT + (cargo_count - 1) * CARGO_GAP;
            f32 cx_start = sx - total_cw * 0.5f;
            for (u32 ci = 0; ci < cargo_count; ci++) {
                f32 cx = cx_start + ci * (CARGO_DOT + CARGO_GAP);
                emit_quad(cx, sy + CARGO_Y, CARGO_DOT, CARGO_DOT,
                          0.3f, 0.8f, 1.0f, 0.8f); // light blue
            }
        }

        // --- Silo ammo indicators (nuke = red, tactical = blue) ---
        if (unit_bars_ && cam_dist < 400.0f) {
            i32 nuke = unit->nuke_silo_ammo;
            i32 tac = unit->tactical_silo_ammo;
            if (nuke > 0 || tac > 0) {
                constexpr f32 AMMO_DOT = 5.0f;
                constexpr f32 AMMO_GAP = 2.0f;
                constexpr f32 AMMO_Y = 14.0f; // below unit center
                f32 ax = sx;
                // Nuke ammo (red dots, left side)
                if (nuke > 0) {
                    i32 nd = nuke > 5 ? 5 : nuke;
                    f32 nw = nd * AMMO_DOT + (nd - 1) * AMMO_GAP;
                    f32 nx_start = ax - nw - 2.0f;
                    for (i32 ni = 0; ni < nd; ni++) {
                        emit_quad(nx_start + ni * (AMMO_DOT + AMMO_GAP),
                                  sy + AMMO_Y, AMMO_DOT, AMMO_DOT,
                                  1.0f, 0.15f, 0.1f, 0.9f); // red
                    }
                }
                // Tactical ammo (blue dots, right side)
                if (tac > 0) {
                    i32 td = tac > 5 ? 5 : tac;
                    for (i32 ti = 0; ti < td; ti++) {
                        emit_quad(ax + 2.0f + ti * (AMMO_DOT + AMMO_GAP),
                                  sy + AMMO_Y, AMMO_DOT, AMMO_DOT,
                                  0.2f, 0.4f, 1.0f, 0.9f); // blue
                    }
                }
            }
        }
    }

    // --- Intel range circles (radar/sonar/omni for selected units) ---
    if (selected_ids && !selected_ids->empty() && cam_dist < 600.0f) {
        constexpr u32 INTEL_SEGMENTS = 24;
        constexpr f32 INTEL_LINE_THICK = 1.5f;
        constexpr f32 PI2 = 6.2831853f;

        for (u32 uid : *selected_ids) {
            auto* e = view.find(uid);
            if (!e || !e->is_unit) continue;
            auto pos = view.position(*e);

            for (const auto& intel : snap.intel_of(*e)) {
                // Captured with a radius of at least 1; ringed when on.
                const std::string& type = intel.type;
                if (!intel.enabled || !intel_ring_types_.count(type)) continue;

                // Color by intel type
                f32 cr = 0, cg = 0, cb = 0, ca = 0.35f;
                if (type == "Radar")       { cg = 0.8f; cb = 0.3f; }
                else if (type == "Sonar")  { cg = 0.4f; cb = 0.9f; }
                else if (type == "Omni")   { cr = 0.9f; cg = 0.9f; cb = 0.3f; }
                else if (type == "Vision") { cg = 0.6f; ca = 0.2f; }
                else continue; // skip unknown intel types

                f32 radius = intel.radius;
                f32 prev_sx2 = 0, prev_sy2 = 0;
                bool prev_valid2 = false;

                for (u32 i = 0; i <= INTEL_SEGMENTS; i++) {
                    f32 angle = PI2 * static_cast<f32>(i) / INTEL_SEGMENTS;
                    f32 wx = pos.x + radius * std::cos(angle);
                    f32 wz = pos.z + radius * std::sin(angle);

                    f32 sx_pt = 0, sy_pt = 0;
                    bool valid = world_to_screen(wx, pos.y, wz,
                                                  vp_matrix, sw, sh,
                                                  sx_pt, sy_pt);

                    if (valid && prev_valid2 && i > 0) {
                        f32 ldx = sx_pt - prev_sx2;
                        f32 ldy = sy_pt - prev_sy2;
                        f32 len = std::sqrt(ldx * ldx + ldy * ldy);
                        if (len >= 1.0f) {
                            emit_line(prev_sx2, prev_sy2, sx_pt, sy_pt, INTEL_LINE_THICK, cr, cg,
                                      cb, ca);
                        }
                    }

                    prev_sx2 = sx_pt;
                    prev_sy2 = sy_pt;
                    prev_valid2 = valid;
                }
            }
        }
    }

    // --- CollisionBeam rendering ---
    if (cam_dist < 600.0f) {
        for (const sim::EntityRecord& entity : view.entities()) {
            if (!entity.is_collision_beam || !entity.beam_enabled) continue;
            if (beams_ && beams_->drew_on_entity(entity.id)) continue; // drawn as FA's

            auto src_pos = view.position(entity);
            auto dst_pos = view.beam_end(entity);
            // A beam shows where the player's army sees either end
            // (CEfxBeam::CanSeeCam; M215b).
            if (recon_ && !recon_->sees_beam(view, src_pos, dst_pos)) continue;

            f32 dx = src_pos.x - eye_x;
            f32 dz = src_pos.z - eye_z;
            if (dx * dx + dz * dz > 600.0f * 600.0f) continue;

            f32 sx0, sy0, sx1, sy1;
            if (!world_to_screen(src_pos.x, src_pos.y, src_pos.z,
                                  vp_matrix, sw, sh, sx0, sy0))
                continue;
            if (!world_to_screen(dst_pos.x, dst_pos.y, dst_pos.z,
                                  vp_matrix, sw, sh, sx1, sy1))
                continue;

            f32 ldx = sx1 - sx0, ldy = sy1 - sy0;
            f32 len = std::sqrt(ldx * ldx + ldy * ldy);
            if (len < 2.0f) continue;

            constexpr f32 BEAM_THICK = 3.5f; // thicker for weapons
            // Red/orange for weapon beams
            emit_line(sx0, sy0, sx1, sy1, BEAM_THICK, 1.0f, 0.4f, 0.1f, 0.8f);
        }
    }

    // --- VFX/emitter particle rendering (billboard particles for IEffect) ---
    if (cam_dist < 600.0f && quad_count_ < MAX_OVERLAY_QUADS) {
        for (const sim::EffectRecord& fx : snap.effects) {
            const sim::EffectRecord* fx_ptr = &fx;

            auto type = fx_ptr->type;

            // Skip decals/splats (handled by decal renderer)
            if (type == sim::EffectType::DECAL || type == sim::EffectType::SPLAT)
                continue;
            // And beams the beam renderer draws as FA does (M214a).
            if (beams_ && beams_->drew_effect(fx_ptr->id)) continue;
            // And trails the trail renderer draws, or left out at this
            // fidelity (M214b).
            if (trails_ && (trails_->draws_effect(fx_ptr->id) || trails_->unmade(fx_ptr->id)))
                continue;
            // And emitters and lights the particle system draws (M214c).
            // (One it never made, CreateIfVisible or left out at this
            // fidelity, shows nothing at all.)
            if (particles_ &&
                (particles_->draws_effect(fx_ptr->id) || particles_->unmade(fx_ptr->id)))
                continue;

            // Resolve effect world position from parent entity + offset
            f32 wx = fx_ptr->offset_x;
            f32 wy = fx_ptr->offset_y;
            f32 wz = fx_ptr->offset_z;

            if (fx_ptr->entity_id > 0) {
                auto* parent = view.find(fx_ptr->entity_id);
                if (!parent) continue;
                auto pp = view.position(*parent);
                wx += pp.x;
                wy += pp.y;
                wz += pp.z;
            }

            // Distance cull
            f32 dx = wx - eye_x;
            f32 dz = wz - eye_z;
            if (dx * dx + dz * dz > 500.0f * 500.0f) continue;

            // Beam entity-to-entity: draw line between two entities
            if (type == sim::EffectType::BEAM_ENTITY_TO_ENTITY) {
                if (fx_ptr->target_entity_id == 0) continue;
                auto* target = view.find(fx_ptr->target_entity_id);
                if (!target) continue;

                auto tp = view.position(*target);
                if (recon_ && !recon_->sees_beam(view, {wx, wy, wz}, tp)) continue;
                f32 sx0, sy0, sx1, sy1;
                if (!world_to_screen(wx, wy, wz, vp_matrix, sw, sh, sx0, sy0))
                    continue;
                if (!world_to_screen(tp.x, tp.y, tp.z, vp_matrix, sw, sh, sx1, sy1))
                    continue;

                f32 ldx2 = sx1 - sx0, ldy2 = sy1 - sy0;
                f32 len = std::sqrt(ldx2 * ldx2 + ldy2 * ldy2);
                if (len < 2.0f) continue;

                f32 thick = fx_ptr->thickness;
                if (thick < 1.0f) thick = 2.0f;
                emit_line(sx0, sy0, sx1, sy1, thick, 0.8f, 0.9f, 1.0f, 0.6f); // pale blue beam
                continue;
            }

            // Attached beam: draw a line from entity in forward direction
            if (type == sim::EffectType::ATTACHED_BEAM) {
                f32 beam_len = fx_ptr->length;
                if (beam_len < 1.0f) beam_len = 5.0f;

                f32 sx0, sy0, sx1, sy1;
                if (!world_to_screen(wx, wy, wz, vp_matrix, sw, sh, sx0, sy0))
                    continue;
                // Use entity heading to orient the beam
                f32 fwd_x = 0.0f, fwd_y = 0.0f, fwd_z = 1.0f;
                if (fx_ptr->entity_id > 0) {
                    auto* beam_parent = view.find(fx_ptr->entity_id);
                    if (beam_parent) {
                        const auto q = view.orientation(*beam_parent);
                        fwd_x = 2.0f * (q.x * q.z + q.w * q.y);
                        fwd_y = 2.0f * (q.y * q.z - q.w * q.x);
                        fwd_z = 1.0f - 2.0f * (q.x * q.x + q.y * q.y);
                    }
                }
                if (recon_ && !recon_->sees_beam(view, {wx, wy, wz},
                                                 {wx + fwd_x * beam_len, wy + fwd_y * beam_len,
                                                  wz + fwd_z * beam_len}))
                    continue;
                if (!world_to_screen(wx + fwd_x * beam_len,
                                      wy + fwd_y * beam_len,
                                      wz + fwd_z * beam_len,
                                      vp_matrix, sw, sh, sx1, sy1))
                    continue;

                f32 ldx2 = sx1 - sx0, ldy2 = sy1 - sy0;
                f32 len = std::sqrt(ldx2 * ldx2 + ldy2 * ldy2);
                if (len >= 2.0f) {
                    f32 thick = fx_ptr->thickness;
                    if (thick < 1.0f) thick = 2.0f;
                    emit_line(sx0, sy0, sx1, sy1, thick, 0.6f, 0.8f, 1.0f, 0.5f);
                }
                continue;
            }

            // An emitter shows where the player's army sees it
            // (CEfxEmitter::CanSeeCam; M215b).
            if (recon_ && !recon_->sees_at(view, -1, wx, wz)) continue;

            // Project position for particle effects
            f32 sx_fx, sy_fx;
            if (!world_to_screen(wx, wy, wz, vp_matrix, sw, sh, sx_fx, sy_fx))
                continue;

            // Emitter particles: small colored dot at effect position
            f32 psize = 4.0f * fx_ptr->scale;
            // Color by army (simple hash)
            f32 pr = 0.7f, pg = 0.7f, pb = 0.7f;
            if (const auto* brain = snap.army(fx_ptr->army)) {
                if (brain->has_color) {
                    pr = brain->r / 255.0f;
                    pg = brain->g / 255.0f;
                    pb = brain->b / 255.0f;
                }
            }

            emit_quad(sx_fx - psize * 0.5f, sy_fx - psize * 0.5f,
                      psize, psize, pr, pg, pb, 0.7f);
        }
    }

    // --- Game over banner ---
    if (game_result != 0) {
        // Full-screen dark overlay
        emit_quad(0, 0, sw, sh, 0.0f, 0.0f, 0.0f, 0.5f);

        // Centered banner bar
        f32 banner_w = 400.0f;
        f32 banner_h = 60.0f;
        f32 bx = (sw - banner_w) * 0.5f;
        f32 by = sh * 0.35f;

        // Banner background
        emit_quad(bx, by, banner_w, banner_h, 0.05f, 0.05f, 0.1f, 0.9f);

        // Color accent stripe at top of banner
        f32 ar = 0, ag = 0, ab = 0;
        if (game_result == 1) { ag = 0.8f; ab = 0.2f; } // victory = green
        if (game_result == 2) { ar = 0.9f; ag = 0.1f; } // defeat = red
        if (game_result == 3) { ar = 0.5f; ag = 0.5f; ab = 0.5f; } // draw = grey
        emit_quad(bx, by, banner_w, 4.0f, ar, ag, ab, 1.0f);

        // Sub-banner hint: "Press R to restart"
        f32 hint_w = 200.0f;
        f32 hint_h = 24.0f;
        f32 hx = (sw - hint_w) * 0.5f;
        f32 hy = by + banner_h + 10.0f;
        emit_quad(hx, hy, hint_w, hint_h, 0.1f, 0.1f, 0.15f, 0.7f);
    }

    // Upload to GPU
    if (!quads_.empty() && instance_mapped_[fi_]) {
        u32 count = std::min(quad_count_, MAX_OVERLAY_QUADS);
        std::memcpy(instance_mapped_[fi_], quads_.data(),
                    count * sizeof(UIInstance));
    }
}

void OverlayRenderer::render(VkCommandBuffer cmd, VkPipelineLayout layout,
                              u32 viewport_w, u32 viewport_h) {
    if (quad_count_ == 0 || !white_ds_) return;

    // Push viewport size (same format as UI pipeline)
    f32 vp[2] = {static_cast<f32>(viewport_w),
                 static_cast<f32>(viewport_h)};
    vkc::push_constants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(f32) * 2, vp);

    // Full-screen scissor
    VkRect2D scissor{};
    scissor.extent = {viewport_w, viewport_h};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    // Bind white texture
    vkc::bind_descriptor_sets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &white_ds_, 0,
                              nullptr);

    // Bind instance buffer
    VkBuffer buf = instance_buf_[fi_].buffer;
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &buf, &offset);

    // Draw all quads (6 verts per quad, instanced)
    vkc::draw(cmd, 6, quad_count_, 0, 0);
}

} // namespace osc::renderer
