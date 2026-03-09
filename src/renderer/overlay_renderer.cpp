#include "renderer/overlay_renderer.hpp"
#include "renderer/camera.hpp"
#include "renderer/texture_cache.hpp"
#include "sim/sim_state.hpp"
#include "sim/entity.hpp"
#include "sim/unit.hpp"
#include "sim/unit_command.hpp"
#include "sim/shield.hpp"
#include "sim/army_brain.hpp"
#include "sim/ieffect.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace osc::renderer {

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

void OverlayRenderer::update(sim::SimState& sim, const Camera& camera,
                              const std::array<f32, 16>& vp_matrix,
                              const std::unordered_set<u32>* selected_ids,
                              TextureCache& tex_cache,
                              u32 viewport_w, u32 viewport_h,
                              i32 game_result, f32 dt) {
    quads_.clear();
    quads_.reserve(MAX_OVERLAY_QUADS);
    quad_count_ = 0;
    white_ds_ = tex_cache.fallback_descriptor();

    f32 sw = static_cast<f32>(viewport_w);
    f32 sh = static_cast<f32>(viewport_h);

    // Camera distance for LOD (skip overlays when very far)
    f32 cam_dist = camera.distance();

    // Eye position for distance culling
    f32 eye_x, eye_y, eye_z;
    camera.eye_position(eye_x, eye_y, eye_z);

    // --- Consume death events and spawn explosion VFX ---
    for (auto& de : sim.death_events()) {
        if (explosions_.size() < MAX_EXPLOSIONS) {
            explosions_.push_back({de.x, de.y, de.z,
                                   std::max(de.scale, 1.0f),
                                   0.0f,
                                   1.0f, 0.8f, 0.3f}); // orange flash
        }
    }
    sim.clear_death_events();

    // Tick and render active explosions
    for (auto it = explosions_.begin(); it != explosions_.end();) {
        it->age += dt;
        if (it->age >= EXPLOSION_DURATION) {
            it = explosions_.erase(it);
            continue;
        }

        f32 t = it->age / EXPLOSION_DURATION; // 0→1 over lifetime
        f32 ex_sx, ex_sy;
        if (world_to_screen(it->x, it->y, it->z, vp_matrix, sw, sh,
                            ex_sx, ex_sy)) {
            // Expanding flash circle (fades out)
            f32 radius = it->scale * (10.0f + 30.0f * t); // pixels, expanding
            f32 alpha = (1.0f - t) * 0.9f;
            // Bright flash core
            emit_quad(ex_sx - radius, ex_sy - radius,
                      radius * 2.0f, radius * 2.0f,
                      it->r, it->g, it->b, alpha * 0.5f);
            // Smaller bright center
            f32 inner = radius * 0.4f * (1.0f - t);
            emit_quad(ex_sx - inner, ex_sy - inner,
                      inner * 2.0f, inner * 2.0f,
                      1.0f, 1.0f, 0.9f, alpha);

            // Debris particles (4 per explosion, scatter outward)
            for (int d = 0; d < 4; d++) {
                f32 ang = static_cast<f32>(d) * 1.5708f + it->age * 2.0f;
                f32 dist = radius * 0.7f * t;
                f32 px = ex_sx + std::cos(ang) * dist;
                f32 py = ex_sy + std::sin(ang) * dist;
                f32 ps = 3.0f * (1.0f - t);
                emit_quad(px - ps, py - ps, ps * 2.0f, ps * 2.0f,
                          0.6f, 0.3f, 0.1f, alpha * 0.7f); // dark debris
            }
        }
        ++it;
    }

    auto& registry = sim.entity_registry();

    // Iterate all entities for health bars + selection circles
    registry.for_each([&](const sim::Entity& entity) {
        if (entity.destroyed()) return;
        if (!entity.is_unit()) return;

        auto pos = entity.position();

        // Distance cull
        f32 dx = pos.x - eye_x;
        f32 dz = pos.z - eye_z;
        f32 dist2 = dx * dx + dz * dz;
        if (dist2 > 800.0f * 800.0f) return; // cull beyond 800 units

        bool is_selected = selected_ids &&
                           selected_ids->count(entity.entity_id()) > 0;

        // Project unit position to screen
        f32 sx, sy;
        if (!world_to_screen(pos.x, pos.y, pos.z, vp_matrix, sw, sh, sx, sy))
            return;

        // --- Health bar ---
        // Only show if damaged or selected
        f32 hp_frac = (entity.max_health() > 0)
                          ? entity.health() / entity.max_health()
                          : 1.0f;
        if (hp_frac < 0.999f || is_selected) {
            constexpr f32 BAR_W = 40.0f;
            constexpr f32 BAR_H = 4.0f;
            constexpr f32 BAR_Y_OFFSET = 20.0f; // pixels above unit center

            f32 bar_x = sx - BAR_W * 0.5f;
            f32 bar_y = sy - BAR_Y_OFFSET;

            // Background (dark)
            emit_quad(bar_x, bar_y, BAR_W, BAR_H, 0.1f, 0.1f, 0.1f, 0.7f);

            // Fill (green → yellow → red based on health)
            f32 fill_w = BAR_W * std::clamp(hp_frac, 0.0f, 1.0f);
            f32 hr = (hp_frac < 0.5f) ? 1.0f : 1.0f - (hp_frac - 0.5f) * 2.0f;
            f32 hg = (hp_frac < 0.5f) ? hp_frac * 2.0f : 1.0f;
            if (fill_w > 0.5f)
                emit_quad(bar_x, bar_y, fill_w, BAR_H, hr, hg, 0.0f, 0.9f);
        }

        // --- Build progress indicator (for units being built OR actively building) ---
        auto* unit = static_cast<const sim::Unit*>(&entity);
        if (unit->is_being_built() && entity.fraction_complete() < 0.999f) {
            // Unit under construction: blue progress bar below health bar
            constexpr f32 BP_W = 40.0f;
            constexpr f32 BP_H = 3.0f;
            constexpr f32 BP_Y_OFFSET = 14.0f; // pixels above center (below health bar)
            f32 bp_x = sx - BP_W * 0.5f;
            f32 bp_y = sy - BP_Y_OFFSET;
            f32 bp_frac = std::clamp(entity.fraction_complete(), 0.0f, 1.0f);

            emit_quad(bp_x, bp_y, BP_W, BP_H, 0.1f, 0.1f, 0.15f, 0.7f);
            f32 bp_fill = BP_W * bp_frac;
            if (bp_fill > 0.5f)
                emit_quad(bp_x, bp_y, bp_fill, BP_H, 0.3f, 0.6f, 1.0f, 0.9f);
        } else if (unit->is_building() && cam_dist < 400.0f) {
            // Builder actively constructing: small yellow indicator below health bar
            constexpr f32 BI_W = 30.0f;
            constexpr f32 BI_H = 3.0f;
            constexpr f32 BI_Y_OFFSET = 14.0f;
            f32 bi_x = sx - BI_W * 0.5f;
            f32 bi_y = sy - BI_Y_OFFSET;
            f32 wp = std::clamp(unit->work_progress(), 0.0f, 1.0f);

            emit_quad(bi_x, bi_y, BI_W, BI_H, 0.1f, 0.1f, 0.05f, 0.6f);
            f32 bi_fill = BI_W * wp;
            if (bi_fill > 0.5f)
                emit_quad(bi_x, bi_y, bi_fill, BI_H, 1.0f, 0.85f, 0.2f, 0.8f);
        }

        // --- Veterancy indicators (gold chevrons above health bar) ---
        if (unit->vet_level() > 0 && cam_dist < 400.0f && (hp_frac < 0.999f || is_selected)) {
            constexpr f32 CHEV_SIZE = 5.0f;  // each chevron square
            constexpr f32 CHEV_GAP  = 1.5f;  // gap between chevrons
            constexpr f32 CHEV_Y_OFFSET = 26.0f; // above health bar
            u8 vl = unit->vet_level();
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
        if (!unit->cargo_ids().empty() && cam_dist < 400.0f) {
            constexpr f32 CARGO_DOT = 4.0f;
            constexpr f32 CARGO_GAP = 2.0f;
            constexpr f32 CARGO_Y = 8.0f; // below unit center
            auto& cargo = unit->cargo_ids();
            u32 cargo_count = static_cast<u32>(cargo.size());
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
        if (cam_dist < 400.0f) {
            i32 nuke = unit->nuke_silo_ammo();
            i32 tac = unit->tactical_silo_ammo();
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

        // --- Selection circle (projected 12-segment circle on ground) ---
        if (is_selected) {
            constexpr f32 RING_RADIUS = 2.5f;
            constexpr f32 RING_THICK = 2.0f; // pixels
            constexpr u32 SEL_SEGMENTS = 12;
            constexpr f32 SEL_PI2 = 6.283185307f;

            f32 prev_sx3 = 0, prev_sy3 = 0;
            bool prev_valid3 = false;

            for (u32 si = 0; si <= SEL_SEGMENTS; si++) {
                f32 angle = static_cast<f32>(si) * SEL_PI2 /
                            static_cast<f32>(SEL_SEGMENTS);
                f32 wx = pos.x + RING_RADIUS * std::cos(angle);
                f32 wz = pos.z + RING_RADIUS * std::sin(angle);

                f32 sx_pt = 0, sy_pt = 0;
                bool valid = world_to_screen(wx, pos.y, wz,
                                              vp_matrix, sw, sh,
                                              sx_pt, sy_pt);

                if (valid && prev_valid3) {
                    // Draw segment as AABB quad
                    f32 ldx = sx_pt - prev_sx3;
                    f32 ldy = sy_pt - prev_sy3;
                    f32 len = std::sqrt(ldx * ldx + ldy * ldy);
                    if (len >= 1.0f) {
                        f32 nx = -ldy / len * RING_THICK;
                        f32 ny = ldx / len * RING_THICK;
                        f32 min_x = std::min({prev_sx3 + nx, prev_sx3 - nx,
                                              sx_pt + nx, sx_pt - nx});
                        f32 min_y = std::min({prev_sy3 + ny, prev_sy3 - ny,
                                              sy_pt + ny, sy_pt - ny});
                        f32 max_x = std::max({prev_sx3 + nx, prev_sx3 - nx,
                                              sx_pt + nx, sx_pt - nx});
                        f32 max_y = std::max({prev_sy3 + ny, prev_sy3 - ny,
                                              sy_pt + ny, sy_pt - ny});
                        emit_quad(min_x, min_y, max_x - min_x, max_y - min_y,
                                  0.2f, 1.0f, 0.2f, 0.8f);
                    }
                }

                prev_sx3 = sx_pt;
                prev_sy3 = sy_pt;
                prev_valid3 = valid;
            }

            // --- Adjacency lines (orange lines to adjacent structures) ---
            auto& adj_ids = unit->adjacent_unit_ids();
            if (!adj_ids.empty() && cam_dist < 400.0f) {
                for (u32 adj_id : adj_ids) {
                    // Only draw each pair once (lower id draws the line)
                    if (adj_id < entity.entity_id()) continue;

                    auto* adj = registry.find(adj_id);
                    if (!adj || adj->destroyed()) continue;

                    auto adj_pos = adj->position();
                    f32 adj_sx, adj_sy;
                    if (!world_to_screen(adj_pos.x, adj_pos.y, adj_pos.z,
                                          vp_matrix, sw, sh, adj_sx, adj_sy))
                        continue;

                    f32 ldx = adj_sx - sx, ldy = adj_sy - sy;
                    f32 len = std::sqrt(ldx * ldx + ldy * ldy);
                    if (len < 2.0f) continue;

                    constexpr f32 ADJ_THICK = 1.5f;
                    f32 nx = -ldy / len * ADJ_THICK;
                    f32 ny = ldx / len * ADJ_THICK;

                    f32 min_x = std::min({sx + nx, sx - nx, adj_sx + nx, adj_sx - nx});
                    f32 min_y = std::min({sy + ny, sy - ny, adj_sy + ny, adj_sy - ny});
                    f32 max_x = std::max({sx + nx, sx - nx, adj_sx + nx, adj_sx - nx});
                    f32 max_y = std::max({sy + ny, sy - ny, adj_sy + ny, adj_sy - ny});

                    emit_quad(min_x, min_y, max_x - min_x, max_y - min_y,
                              1.0f, 0.6f, 0.1f, 0.5f); // orange
                }
            }
        }
    });

    // --- Command lines (full queue for selected units) ---
    if (selected_ids && !selected_ids->empty()) {
        for (u32 uid : *selected_ids) {
            auto* e = registry.find(uid);
            if (!e || !e->is_unit() || e->destroyed()) continue;
            auto* unit = static_cast<const sim::Unit*>(e);
            auto& cmds = unit->command_queue();
            if (cmds.empty()) continue;

            // Start from unit position
            auto pos = e->position();
            f32 prev_sx, prev_sy;
            if (!world_to_screen(pos.x, pos.y, pos.z, vp_matrix, sw, sh,
                                 prev_sx, prev_sy))
                continue;

            // Draw line + waypoint marker for each command in queue
            constexpr u32 MAX_CMD_LINES = 8;
            u32 cmd_drawn = 0;
            for (auto& cmd : cmds) {
                if (cmd_drawn >= MAX_CMD_LINES) break;

                // Only draw spatial commands
                if (cmd.type != sim::CommandType::Move &&
                    cmd.type != sim::CommandType::Attack &&
                    cmd.type != sim::CommandType::Patrol &&
                    cmd.type != sim::CommandType::Guard &&
                    cmd.type != sim::CommandType::Reclaim &&
                    cmd.type != sim::CommandType::Repair &&
                    cmd.type != sim::CommandType::BuildMobile &&
                    cmd.type != sim::CommandType::Capture)
                    continue;

                f32 sx1, sy1;
                bool projected = false;
                // For entity-targeted commands, project target entity position
                if (cmd.target_id > 0) {
                    auto* target = registry.find(cmd.target_id);
                    if (target && !target->destroyed()) {
                        auto tp = target->position();
                        projected = world_to_screen(tp.x, tp.y, tp.z, vp_matrix,
                                                     sw, sh, sx1, sy1);
                    } else {
                        // Dead target: skip if target_pos is zero (entity-only cmd)
                        auto& tp = cmd.target_pos;
                        if (tp.x == 0.0f && tp.y == 0.0f && tp.z == 0.0f) {
                            cmd_drawn++;
                            continue;
                        }
                        projected = world_to_screen(tp.x, tp.y, tp.z, vp_matrix,
                                                     sw, sh, sx1, sy1);
                    }
                } else {
                    projected = world_to_screen(cmd.target_pos.x, cmd.target_pos.y,
                                                 cmd.target_pos.z, vp_matrix,
                                                 sw, sh, sx1, sy1);
                }
                if (!projected) {
                    cmd_drawn++;
                    continue;
                }

                // Line color by command type
                f32 lr = 0, lg = 0, lb = 0;
                switch (cmd.type) {
                    case sim::CommandType::Move:      lg = 1.0f; lb = 0.3f; break;
                    case sim::CommandType::Attack:     lr = 1.0f; lg = 0.2f; break;
                    case sim::CommandType::Patrol:     lb = 1.0f; lg = 0.5f; break;
                    case sim::CommandType::Guard:      lg = 0.8f; lb = 0.8f; break;
                    case sim::CommandType::Reclaim:    lg = 0.7f; lb = 0.2f; break;
                    case sim::CommandType::Repair:     lg = 1.0f; break;
                    case sim::CommandType::BuildMobile:lr = 0.3f; lg = 0.8f; lb = 1.0f; break;
                    case sim::CommandType::Capture:    lr = 1.0f; lg = 1.0f; break;
                    default: lg = 0.5f; break;
                }

                {
                    // Fade alpha for queued commands (first is bright, rest dimmer)
                    f32 alpha = (cmd_drawn == 0) ? 0.6f : 0.35f;

                    // Draw line from prev to target
                    f32 ldx = sx1 - prev_sx, ldy = sy1 - prev_sy;
                    f32 len = std::sqrt(ldx * ldx + ldy * ldy);
                    if (len >= 2.0f) {
                        constexpr f32 LINE_THICK = 1.5f;
                        f32 nx = -ldy / len * LINE_THICK;
                        f32 ny =  ldx / len * LINE_THICK;

                        f32 min_x = std::min({prev_sx + nx, prev_sx - nx, sx1 + nx, sx1 - nx});
                        f32 min_y = std::min({prev_sy + ny, prev_sy - ny, sy1 + ny, sy1 - ny});
                        f32 max_x = std::max({prev_sx + nx, prev_sx - nx, sx1 + nx, sx1 - nx});
                        f32 max_y = std::max({prev_sy + ny, prev_sy - ny, sy1 + ny, sy1 - ny});

                        emit_quad(min_x, min_y, max_x - min_x, max_y - min_y,
                                  lr, lg, lb, alpha);
                    }

                    // Waypoint marker (small diamond at target)
                    constexpr f32 MARKER_SIZE = 6.0f;
                    emit_quad(sx1 - MARKER_SIZE * 0.5f, sy1 - MARKER_SIZE * 0.5f,
                              MARKER_SIZE, MARKER_SIZE,
                              lr, lg, lb, alpha + 0.15f);

                    prev_sx = sx1;
                    prev_sy = sy1;
                }

                cmd_drawn++;
            }
        }
    }

    // --- Intel range circles (radar/sonar/omni for selected units) ---
    if (selected_ids && !selected_ids->empty() && cam_dist < 600.0f) {
        constexpr u32 INTEL_SEGMENTS = 24;
        constexpr f32 INTEL_LINE_THICK = 1.5f;
        constexpr f32 PI2 = 6.2831853f;

        for (u32 uid : *selected_ids) {
            auto* e = registry.find(uid);
            if (!e || !e->is_unit() || e->destroyed()) continue;
            auto* unit = static_cast<const sim::Unit*>(e);
            auto pos = e->position();

            for (auto& [type, state] : unit->intel_states()) {
                if (!state.enabled || state.radius < 1.0f) continue;

                // Color by intel type
                f32 cr = 0, cg = 0, cb = 0, ca = 0.35f;
                if (type == "Radar")       { cg = 0.8f; cb = 0.3f; }
                else if (type == "Sonar")  { cg = 0.4f; cb = 0.9f; }
                else if (type == "Omni")   { cr = 0.9f; cg = 0.9f; cb = 0.3f; }
                else if (type == "Vision") { cg = 0.6f; ca = 0.2f; }
                else continue; // skip unknown intel types

                f32 radius = state.radius;
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
                            f32 nx = -ldy / len * INTEL_LINE_THICK;
                            f32 ny = ldx / len * INTEL_LINE_THICK;

                            f32 min_x = std::min({prev_sx2 + nx, prev_sx2 - nx,
                                                   sx_pt + nx, sx_pt - nx});
                            f32 min_y = std::min({prev_sy2 + ny, prev_sy2 - ny,
                                                   sy_pt + ny, sy_pt - ny});
                            f32 max_x = std::max({prev_sx2 + nx, prev_sx2 - nx,
                                                   sx_pt + nx, sx_pt - nx});
                            f32 max_y = std::max({prev_sy2 + ny, prev_sy2 - ny,
                                                   sy_pt + ny, sy_pt - ny});

                            emit_quad(min_x, min_y, max_x - min_x, max_y - min_y,
                                      cr, cg, cb, ca);
                        }
                    }

                    prev_sx2 = sx_pt;
                    prev_sy2 = sy_pt;
                    prev_valid2 = valid;
                }
            }
        }
    }

    // --- Active operation beams (build/reclaim/repair/capture) ---
    if (cam_dist < 600.0f) {
        registry.for_each([&](const sim::Entity& entity) {
            if (entity.destroyed() || !entity.is_unit()) return;
            auto* unit = static_cast<const sim::Unit*>(&entity);

            // Determine beam target and color
            u32 target_id = 0;
            f32 br = 0, bg = 0, bb = 0, ba = 0.6f;
            if (unit->is_building()) {
                target_id = unit->build_target_id();
                br = 0.2f; bg = 0.9f; bb = 0.6f; // teal
            } else if (unit->is_reclaiming()) {
                target_id = unit->reclaim_target_id();
                br = 0.9f; bg = 0.8f; bb = 0.2f; // gold
            } else if (unit->is_repairing()) {
                target_id = unit->repair_target_id();
                br = 0.3f; bg = 1.0f; bb = 0.3f; // green
            } else if (unit->is_capturing()) {
                target_id = unit->capture_target_id();
                br = 1.0f; bg = 1.0f; bb = 0.2f; // yellow
            }
            if (target_id == 0) return;

            auto* target = registry.find(target_id);
            if (!target || target->destroyed()) return;

            auto src_pos = entity.position();
            auto dst_pos = target->position();

            // Distance cull
            f32 dx = src_pos.x - eye_x;
            f32 dz = src_pos.z - eye_z;
            if (dx * dx + dz * dz > 600.0f * 600.0f) return;

            f32 sx0, sy0, sx1, sy1;
            if (!world_to_screen(src_pos.x, src_pos.y, src_pos.z,
                                  vp_matrix, sw, sh, sx0, sy0))
                return;
            if (!world_to_screen(dst_pos.x, dst_pos.y, dst_pos.z,
                                  vp_matrix, sw, sh, sx1, sy1))
                return;

            // Draw beam line (thicker than command lines)
            f32 ldx = sx1 - sx0, ldy = sy1 - sy0;
            f32 len = std::sqrt(ldx * ldx + ldy * ldy);
            if (len < 2.0f) return;

            constexpr f32 BEAM_THICK = 2.5f;
            f32 nx = -ldy / len * BEAM_THICK;
            f32 ny = ldx / len * BEAM_THICK;

            f32 min_x = std::min({sx0 + nx, sx0 - nx, sx1 + nx, sx1 - nx});
            f32 min_y = std::min({sy0 + ny, sy0 - ny, sy1 + ny, sy1 - ny});
            f32 max_x = std::max({sx0 + nx, sx0 - nx, sx1 + nx, sx1 - nx});
            f32 max_y = std::max({sy0 + ny, sy0 - ny, sy1 + ny, sy1 - ny});

            emit_quad(min_x, min_y, max_x - min_x, max_y - min_y,
                      br, bg, bb, ba);
        });
    }

    // --- CollisionBeam rendering ---
    if (cam_dist < 600.0f) {
        registry.for_each([&](const sim::Entity& entity) {
            if (entity.destroyed()) return;
            if (!entity.is_collision_beam() || !entity.beam_enabled()) return;

            auto src_pos = entity.position();
            auto dst_pos = entity.beam_endpoint();

            f32 dx = src_pos.x - eye_x;
            f32 dz = src_pos.z - eye_z;
            if (dx * dx + dz * dz > 600.0f * 600.0f) return;

            f32 sx0, sy0, sx1, sy1;
            if (!world_to_screen(src_pos.x, src_pos.y, src_pos.z,
                                  vp_matrix, sw, sh, sx0, sy0))
                return;
            if (!world_to_screen(dst_pos.x, dst_pos.y, dst_pos.z,
                                  vp_matrix, sw, sh, sx1, sy1))
                return;

            f32 ldx = sx1 - sx0, ldy = sy1 - sy0;
            f32 len = std::sqrt(ldx * ldx + ldy * ldy);
            if (len < 2.0f) return;

            constexpr f32 BEAM_THICK = 3.5f; // thicker for weapons
            f32 nx = -ldy / len * BEAM_THICK;
            f32 ny = ldx / len * BEAM_THICK;

            f32 min_x = std::min({sx0 + nx, sx0 - nx, sx1 + nx, sx1 - nx});
            f32 min_y = std::min({sy0 + ny, sy0 - ny, sy1 + ny, sy1 - ny});
            f32 max_x = std::max({sx0 + nx, sx0 - nx, sx1 + nx, sx1 - nx});
            f32 max_y = std::max({sy0 + ny, sy0 - ny, sy1 + ny, sy1 - ny});

            // Red/orange for weapon beams
            emit_quad(min_x, min_y, max_x - min_x, max_y - min_y,
                      1.0f, 0.4f, 0.1f, 0.8f);
        });
    }

    // --- Shield bubble rendering (projected circle outlines) ---
    {
        constexpr u32 SHIELD_SEGMENTS = 16;
        constexpr f32 SHIELD_LINE_THICK = 2.0f;
        constexpr f32 PI2 = 6.2831853f;

        registry.for_each([&](const sim::Entity& entity) {
            if (entity.destroyed() || !entity.is_shield()) return;
            auto* shield = static_cast<const sim::Shield*>(&entity);
            if (!shield->is_on) return;
            if (shield->owner_id == 0) return;

            // Find owner unit for position
            auto* owner = registry.find(shield->owner_id);
            if (!owner || owner->destroyed()) return;

            auto pos = owner->position();

            // Distance cull
            f32 dx = pos.x - eye_x;
            f32 dz = pos.z - eye_z;
            if (dx * dx + dz * dz > 800.0f * 800.0f) return;

            f32 radius = shield->size;
            if (radius < 1.0f) return;

            // Army color for shield tint
            f32 sr = 0.5f, sg = 0.5f, sb = 0.8f;
            i32 army = owner->army();
            if (army >= 0 && army < static_cast<i32>(sim.army_count())) {
                auto* brain = sim.army_at(static_cast<size_t>(army));
                if (brain && (brain->color_r() || brain->color_g() ||
                              brain->color_b())) {
                    sr = brain->color_r() / 255.0f;
                    sg = brain->color_g() / 255.0f;
                    sb = brain->color_b() / 255.0f;
                }
            }

            // Shield health ratio modulates alpha (damaged = more visible)
            f32 hp_frac = (shield->max_health() > 0)
                              ? shield->health() / shield->max_health()
                              : 1.0f;
            f32 alpha = 0.15f + (1.0f - hp_frac) * 0.25f; // 0.15 at full, 0.4 at empty

            // Project circle segments at ground plane (y = pos.y)
            f32 prev_sx = 0, prev_sy = 0;
            bool prev_valid = false;

            for (u32 i = 0; i <= SHIELD_SEGMENTS; i++) {
                f32 angle = PI2 * static_cast<f32>(i) / SHIELD_SEGMENTS;
                f32 wx = pos.x + radius * std::cos(angle);
                f32 wz = pos.z + radius * std::sin(angle);

                f32 sx_pt = 0, sy_pt = 0;
                bool valid = world_to_screen(wx, pos.y, wz,
                                              vp_matrix, sw, sh,
                                              sx_pt, sy_pt);

                if (valid && prev_valid && i > 0) {
                    // Draw line segment
                    f32 ldx = sx_pt - prev_sx, ldy = sy_pt - prev_sy;
                    f32 len = std::sqrt(ldx * ldx + ldy * ldy);
                    if (len >= 1.0f) {
                        f32 nx = -ldy / len * SHIELD_LINE_THICK;
                        f32 ny = ldx / len * SHIELD_LINE_THICK;

                        f32 min_x = std::min({prev_sx + nx, prev_sx - nx,
                                               sx_pt + nx, sx_pt - nx});
                        f32 min_y = std::min({prev_sy + ny, prev_sy - ny,
                                               sy_pt + ny, sy_pt - ny});
                        f32 max_x = std::max({prev_sx + nx, prev_sx - nx,
                                               sx_pt + nx, sx_pt - nx});
                        f32 max_y = std::max({prev_sy + ny, prev_sy - ny,
                                               sy_pt + ny, sy_pt - ny});

                        emit_quad(min_x, min_y, max_x - min_x, max_y - min_y,
                                  sr, sg, sb, alpha);
                    }
                }

                prev_sx = sx_pt;
                prev_sy = sy_pt;
                prev_valid = valid;
            }

            // Filled center area (single large quad at reduced alpha)
            f32 cx_s, cy_s;
            if (world_to_screen(pos.x, pos.y, pos.z,
                                 vp_matrix, sw, sh, cx_s, cy_s)) {
                // Project a point at radius distance to estimate screen radius
                f32 edge_sx, edge_sy;
                if (world_to_screen(pos.x + radius, pos.y, pos.z,
                                     vp_matrix, sw, sh, edge_sx, edge_sy)) {
                    f32 screen_r = std::abs(edge_sx - cx_s);
                    if (screen_r > 4.0f) {
                        emit_quad(cx_s - screen_r, cy_s - screen_r,
                                  screen_r * 2.0f, screen_r * 2.0f,
                                  sr, sg, sb, alpha * 0.3f);
                    }
                }
            }
        });
    }

    // --- VFX/emitter particle rendering (billboard particles for IEffect) ---
    if (cam_dist < 600.0f && quad_count_ < MAX_OVERLAY_QUADS) {
        auto& effects = sim.effect_registry().all();
        for (auto& fx_ptr : effects) {
            if (!fx_ptr || fx_ptr->destroyed()) continue;

            auto type = fx_ptr->type();

            // Skip decals/splats (handled by decal renderer)
            if (type == sim::EffectType::DECAL || type == sim::EffectType::SPLAT)
                continue;

            // Resolve effect world position from parent entity + offset
            f32 wx = fx_ptr->offset_x();
            f32 wy = fx_ptr->offset_y();
            f32 wz = fx_ptr->offset_z();

            if (fx_ptr->entity_id() > 0) {
                auto* parent = registry.find(fx_ptr->entity_id());
                if (!parent || parent->destroyed()) continue;
                auto pp = parent->position();
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
                if (fx_ptr->target_entity_id() == 0) continue;
                auto* target = registry.find(fx_ptr->target_entity_id());
                if (!target || target->destroyed()) continue;

                auto tp = target->position();
                f32 sx0, sy0, sx1, sy1;
                if (!world_to_screen(wx, wy, wz, vp_matrix, sw, sh, sx0, sy0))
                    continue;
                if (!world_to_screen(tp.x, tp.y, tp.z, vp_matrix, sw, sh, sx1, sy1))
                    continue;

                f32 ldx2 = sx1 - sx0, ldy2 = sy1 - sy0;
                f32 len = std::sqrt(ldx2 * ldx2 + ldy2 * ldy2);
                if (len < 2.0f) continue;

                f32 thick = static_cast<f32>(fx_ptr->get_param("THICKNESS"));
                if (thick < 1.0f) thick = 2.0f;
                f32 nx = -ldy2 / len * thick;
                f32 ny = ldx2 / len * thick;

                f32 min_x = std::min({sx0 + nx, sx0 - nx, sx1 + nx, sx1 - nx});
                f32 min_y = std::min({sy0 + ny, sy0 - ny, sy1 + ny, sy1 - ny});
                f32 max_x = std::max({sx0 + nx, sx0 - nx, sx1 + nx, sx1 - nx});
                f32 max_y = std::max({sy0 + ny, sy0 - ny, sy1 + ny, sy1 - ny});

                emit_quad(min_x, min_y, max_x - min_x, max_y - min_y,
                          0.8f, 0.9f, 1.0f, 0.6f); // pale blue beam
                continue;
            }

            // Attached beam: draw a line from entity in forward direction
            if (type == sim::EffectType::ATTACHED_BEAM) {
                f32 beam_len = static_cast<f32>(fx_ptr->get_param("LENGTH"));
                if (beam_len < 1.0f) beam_len = 5.0f;

                f32 sx0, sy0, sx1, sy1;
                if (!world_to_screen(wx, wy, wz, vp_matrix, sw, sh, sx0, sy0))
                    continue;
                // TODO: use entity heading instead of +Z when orientation is available
                if (!world_to_screen(wx, wy, wz + beam_len,
                                      vp_matrix, sw, sh, sx1, sy1))
                    continue;

                f32 ldx2 = sx1 - sx0, ldy2 = sy1 - sy0;
                f32 len = std::sqrt(ldx2 * ldx2 + ldy2 * ldy2);
                if (len >= 2.0f) {
                    f32 thick = static_cast<f32>(fx_ptr->get_param("THICKNESS"));
                    if (thick < 1.0f) thick = 2.0f;
                    f32 nx = -ldy2 / len * thick;
                    f32 ny = ldx2 / len * thick;

                    f32 min_x = std::min({sx0 + nx, sx0 - nx, sx1 + nx, sx1 - nx});
                    f32 min_y = std::min({sy0 + ny, sy0 - ny, sy1 + ny, sy1 - ny});
                    f32 max_x = std::max({sx0 + nx, sx0 - nx, sx1 + nx, sx1 - nx});
                    f32 max_y = std::max({sy0 + ny, sy0 - ny, sy1 + ny, sy1 - ny});

                    emit_quad(min_x, min_y, max_x - min_x, max_y - min_y,
                              0.6f, 0.8f, 1.0f, 0.5f);
                }
                continue;
            }

            // Project position for particle effects
            f32 sx_fx, sy_fx;
            if (!world_to_screen(wx, wy, wz, vp_matrix, sw, sh, sx_fx, sy_fx))
                continue;

            // Light particle: larger glowing circle
            if (type == sim::EffectType::LIGHT_PARTICLE) {
                f32 size = fx_ptr->light_size() * fx_ptr->scale();
                if (size < 1.0f) size = 4.0f;
                f32 screen_size = size * 3.0f; // scale to screen pixels
                emit_quad(sx_fx - screen_size * 0.5f, sy_fx - screen_size * 0.5f,
                          screen_size, screen_size,
                          1.0f, 0.9f, 0.5f, 0.6f); // warm glow
                continue;
            }

            // Emitter particles: small colored dot at effect position
            f32 psize = 4.0f * fx_ptr->scale();
            // Color by army (simple hash)
            f32 pr = 0.7f, pg = 0.7f, pb = 0.7f;
            i32 army = fx_ptr->army();
            if (army >= 0 && army < static_cast<i32>(sim.army_count())) {
                auto* brain = sim.army_at(static_cast<size_t>(army));
                if (brain && (brain->color_r() || brain->color_g() ||
                              brain->color_b())) {
                    pr = brain->color_r() / 255.0f;
                    pg = brain->color_g() / 255.0f;
                    pb = brain->color_b() / 255.0f;
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
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                       sizeof(f32) * 2, vp);

    // Full-screen scissor
    VkRect2D scissor{};
    scissor.extent = {viewport_w, viewport_h};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    // Bind white texture
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            layout, 0, 1, &white_ds_, 0, nullptr);

    // Bind instance buffer
    VkBuffer buf = instance_buf_[fi_].buffer;
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &buf, &offset);

    // Draw all quads (6 verts per quad, instanced)
    vkCmdDraw(cmd, 6, quad_count_, 0, 0);
}

} // namespace osc::renderer
