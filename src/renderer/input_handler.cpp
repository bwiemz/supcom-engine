#include "renderer/input_handler.hpp"
#include "renderer/renderer.hpp"

#include "sim/sim_state.hpp"
#include "sim/entity.hpp"
#include "sim/unit.hpp"
#include "sim/unit_command.hpp"
#include "map/terrain.hpp"

#include <GLFW/glfw3.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace osc::renderer {

void InputHandler::update(Renderer& renderer, sim::SimState& sim,
                          f64 /*dt*/) {
    f64 mx_d, my_d;
    renderer.mouse_position(mx_d, my_d);
    f32 mx = static_cast<f32>(mx_d);
    f32 my = static_cast<f32>(my_d);

    // Process control groups and camera bookmarks (number keys)
    handle_groups_and_bookmarks(renderer, sim);

    bool lmb = renderer.is_mouse_pressed(GLFW_MOUSE_BUTTON_LEFT);
    bool rmb = renderer.is_mouse_pressed(GLFW_MOUSE_BUTTON_RIGHT);

    // --- Check if mouse is over minimap ---
    f32 map_w = 0, map_h = 0;
    if (sim.terrain()) {
        map_w = static_cast<f32>(sim.terrain()->map_width());
        map_h = static_cast<f32>(sim.terrain()->map_height());
    }

    f32 mm_wx, mm_wz;
    bool on_minimap = renderer.minimap().hit_test(
        mx, my, renderer.width(), renderer.height(),
        map_w, map_h, mm_wx, mm_wz);

    // --- Left mouse: selection or minimap click-to-jump ---
    if (lmb && !lmb_was_pressed_) {
        if (on_minimap && map_w > 0) {
            // Minimap click — jump camera to that world position
            auto& cam = renderer.camera();
            cam.set_target(mm_wx, mm_wz);
            lmb_was_pressed_ = lmb;
            rmb_was_pressed_ = rmb;
            return; // consume the click
        }
        // Normal click — start potential drag
        drag_start_x_ = mx;
        drag_start_y_ = my;
        dragging_ = false;
    }

    if (lmb && lmb_was_pressed_) {
        if (on_minimap && map_w > 0) {
            // Dragging on minimap — continuously move camera
            auto& cam = renderer.camera();
            cam.set_target(mm_wx, mm_wz);
        } else {
            // Held down — check for drag
            f32 dx = mx - drag_start_x_;
            f32 dy = my - drag_start_y_;
            if (!dragging_ && (dx * dx + dy * dy) > DRAG_THRESHOLD * DRAG_THRESHOLD) {
                dragging_ = true;
            }
            if (dragging_) {
                drag_end_x_ = mx;
                drag_end_y_ = my;

                // Update world-space drag rect
                const auto& cam = renderer.camera();
                f32 w = static_cast<f32>(renderer.width());
                f32 h = static_cast<f32>(renderer.height());
                cam.screen_to_world(drag_start_x_, drag_start_y_, w, h, 0,
                                    drag_world_x0_, drag_world_z0_);
                cam.screen_to_world(drag_end_x_, drag_end_y_, w, h, 0,
                                    drag_world_x1_, drag_world_z1_);
            }
        }
    }

    if (!lmb && lmb_was_pressed_) {
        // Left button just released
        if (dragging_) {
            handle_drag_select(renderer, sim);
            dragging_ = false;
        } else if (!on_minimap) {
            handle_left_click(renderer, sim, mx, my);
        }
    }

    lmb_was_pressed_ = lmb;

    // --- Right mouse: commands (also works on minimap) ---
    if (rmb && !rmb_was_pressed_) {
        if (on_minimap && map_w > 0 && !selected_.empty()) {
            // Right-click on minimap: issue move to minimap position
            f32 wy = 0;
            if (sim.terrain())
                wy = sim.terrain()->get_surface_height(mm_wx, mm_wz);

            for (u32 uid : selected_) {
                auto* e = sim.entity_registry().find(uid);
                if (!e || !e->is_unit() || e->destroyed()) continue;
                auto* unit = static_cast<sim::Unit*>(e);

                sim::UnitCommand cmd;
                cmd.type = sim::CommandType::Move;
                cmd.target_pos = {mm_wx, wy, mm_wz};
                cmd.command_id = sim.next_command_id();
                unit->push_command(cmd, true);
            }
            spdlog::debug("Minimap move: {} units to ({:.0f},{:.0f})",
                          selected_.size(), mm_wx, mm_wz);
        } else {
            handle_right_click(renderer, sim, mx, my);
        }
    }
    rmb_was_pressed_ = rmb;
}

void InputHandler::handle_left_click(Renderer& renderer,
                                     sim::SimState& sim,
                                     f32 mx, f32 my) {
    const auto& cam = renderer.camera();
    f32 w = static_cast<f32>(renderer.width());
    f32 h = static_cast<f32>(renderer.height());

    f32 wx, wz;
    if (!cam.screen_to_world(mx, my, w, h, 0, wx, wz))
        return;

    // Check if Shift is held (additive selection)
    bool shift = renderer.is_key_pressed(GLFW_KEY_LEFT_SHIFT) ||
                 renderer.is_key_pressed(GLFW_KEY_RIGHT_SHIFT);

    u32 picked = pick_unit(sim, wx, wz, 5.0f);

    if (!shift)
        selected_.clear();

    if (picked != 0) {
        if (shift && selected_.count(picked))
            selected_.erase(picked);
        else
            selected_.insert(picked);
    }

    spdlog::debug("Selection: {} units (click at world {:.0f},{:.0f})",
                  selected_.size(), wx, wz);
}

void InputHandler::handle_drag_select(Renderer& renderer,
                                      sim::SimState& sim) {
    const auto& cam = renderer.camera();
    f32 w = static_cast<f32>(renderer.width());
    f32 h = static_cast<f32>(renderer.height());

    f32 wx0, wz0, wx1, wz1;
    if (!cam.screen_to_world(drag_start_x_, drag_start_y_, w, h, 0, wx0, wz0))
        return;
    if (!cam.screen_to_world(drag_end_x_, drag_end_y_, w, h, 0, wx1, wz1))
        return;

    // Normalize rect
    if (wx0 > wx1) std::swap(wx0, wx1);
    if (wz0 > wz1) std::swap(wz0, wz1);

    bool shift = renderer.is_key_pressed(GLFW_KEY_LEFT_SHIFT) ||
                 renderer.is_key_pressed(GLFW_KEY_RIGHT_SHIFT);
    if (!shift)
        selected_.clear();

    // Use spatial grid to find units in rect
    auto ids = sim.entity_registry().collect_in_rect(wx0, wz0, wx1, wz1);
    for (u32 id : ids) {
        auto* e = sim.entity_registry().find(id);
        if (!e || !e->is_unit() || e->destroyed()) continue;
        if (e->army() != player_army_) continue;
        selected_.insert(id);
    }

    spdlog::debug("Drag select: {} units in rect ({:.0f},{:.0f})-({:.0f},{:.0f})",
                  selected_.size(), wx0, wz0, wx1, wz1);
}

void InputHandler::handle_right_click(Renderer& renderer,
                                      sim::SimState& sim,
                                      f32 mx, f32 my) {
    if (selected_.empty()) return;

    const auto& cam = renderer.camera();
    f32 w = static_cast<f32>(renderer.width());
    f32 h = static_cast<f32>(renderer.height());

    f32 wx, wz;
    if (!cam.screen_to_world(mx, my, w, h, 0, wx, wz))
        return;

    // Check if clicking on an enemy unit — if so, attack
    // Search for any unit near the click point
    auto nearby = sim.entity_registry().collect_in_radius(wx, wz, 5.0f);
    u32 enemy_id = 0;
    for (u32 id : nearby) {
        auto* e = sim.entity_registry().find(id);
        if (!e || !e->is_unit() || e->destroyed()) continue;
        if (e->army() != player_army_ && e->army() >= 0) {
            enemy_id = id;
            break;
        }
    }

    // Get terrain height at target for Y coordinate
    f32 wy = 0;
    if (sim.terrain())
        wy = sim.terrain()->get_surface_height(wx, wz);

    // Issue commands to all selected units
    for (u32 uid : selected_) {
        auto* e = sim.entity_registry().find(uid);
        if (!e || !e->is_unit() || e->destroyed()) continue;
        auto* unit = static_cast<sim::Unit*>(e);

        sim::UnitCommand cmd;
        if (enemy_id != 0) {
            cmd.type = sim::CommandType::Attack;
            cmd.target_id = enemy_id;
            cmd.target_pos = {wx, wy, wz};
        } else {
            cmd.type = sim::CommandType::Move;
            cmd.target_pos = {wx, wy, wz};
        }
        cmd.command_id = sim.next_command_id();
        unit->push_command(cmd, true); // clear existing commands
    }

    spdlog::debug("Right-click: {} to {} units at ({:.0f},{:.0f})",
                  enemy_id ? "Attack" : "Move",
                  selected_.size(), wx, wz);
}

u32 InputHandler::pick_unit(sim::SimState& sim, f32 wx, f32 wz,
                            f32 radius) const {
    auto nearby = sim.entity_registry().collect_in_radius(wx, wz, radius);

    u32 best_id = 0;
    f32 best_dist2 = std::numeric_limits<f32>::max();

    for (u32 id : nearby) {
        auto* e = sim.entity_registry().find(id);
        if (!e || !e->is_unit() || e->destroyed()) continue;
        if (e->army() != player_army_) continue;

        f32 dx = e->position().x - wx;
        f32 dz = e->position().z - wz;
        f32 d2 = dx * dx + dz * dz;
        if (d2 < best_dist2) {
            best_dist2 = d2;
            best_id = id;
        }
    }

    return best_id;
}

void InputHandler::handle_groups_and_bookmarks(Renderer& renderer,
                                                sim::SimState& sim) {
    bool ctrl = renderer.is_key_pressed(GLFW_KEY_LEFT_CONTROL) ||
                renderer.is_key_pressed(GLFW_KEY_RIGHT_CONTROL);
    bool shift = renderer.is_key_pressed(GLFW_KEY_LEFT_SHIFT) ||
                 renderer.is_key_pressed(GLFW_KEY_RIGHT_SHIFT);

    // GLFW_KEY_0 through GLFW_KEY_9
    static constexpr int KEY_MAP[NUM_GROUPS] = {
        GLFW_KEY_0, GLFW_KEY_1, GLFW_KEY_2, GLFW_KEY_3, GLFW_KEY_4,
        GLFW_KEY_5, GLFW_KEY_6, GLFW_KEY_7, GLFW_KEY_8, GLFW_KEY_9
    };

    for (u32 i = 0; i < NUM_GROUPS; i++) {
        bool pressed = renderer.is_key_pressed(KEY_MAP[i]);

        // Edge-triggered (only on press, not hold)
        if (pressed && !number_was_pressed_[i]) {
            if (ctrl && shift) {
                // Ctrl+Shift+N: save camera bookmark
                camera_bookmarks_[i] = {
                    renderer.camera().target_x(),
                    renderer.camera().target_z(),
                    true
                };
                spdlog::debug("Camera bookmark {} saved at ({:.0f},{:.0f})",
                              i, camera_bookmarks_[i].x, camera_bookmarks_[i].z);
            } else if (ctrl) {
                // Ctrl+N: assign control group from current selection
                control_groups_[i] = selected_;
                spdlog::debug("Control group {} assigned: {} units",
                              i, control_groups_[i].size());
            } else if (shift) {
                // Shift+N: recall camera bookmark
                if (camera_bookmarks_[i].valid) {
                    auto& cam = renderer.camera();
                    cam.set_target(camera_bookmarks_[i].x,
                                   camera_bookmarks_[i].z);
                    spdlog::debug("Camera bookmark {} recalled", i);
                }
            } else {
                // N: recall control group (select those units)
                if (!control_groups_[i].empty()) {
                    // Select live units from group (don't mutate stored group)
                    const auto& group = control_groups_[i];
                    std::unordered_set<u32> live;
                    for (u32 uid : group) {
                        auto* e = sim.entity_registry().find(uid);
                        if (e && !e->destroyed())
                            live.insert(uid);
                    }

                    selected_ = live;
                    spdlog::debug("Control group {} recalled: {} units",
                                  i, selected_.size());
                }
            }
        }

        number_was_pressed_[i] = pressed;
    }
}

} // namespace osc::renderer
