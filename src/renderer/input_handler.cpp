#include "renderer/input_handler.hpp"
#include "renderer/command_graph_renderer.hpp"
#include "sim/build_placement.hpp"
#include "renderer/recon_view.hpp"
#include "renderer/renderer.hpp"

#include "sim/army_brain.hpp"
#include "sim/bone_data.hpp"
#include "sim/sim_state.hpp"
#include "sim/entity.hpp"
#include "sim/prop.hpp"
#include "sim/unit.hpp"
#include "sim/unit_command.hpp"
#include "map/pathfinding_grid.hpp"
#include "map/terrain.hpp"

#include <GLFW/glfw3.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace osc::renderer {

namespace {

bool targetable_prop(const sim::Entity& e) {
    return e.is_prop() && !static_cast<const sim::Prop&>(e).untargetable;
}

// Moho's func_GetRightMouseButtonAction: a unit riding another is busy.
bool capture_target(const sim::Unit& u) {
    return u.capturable() && !u.is_being_built() && u.parent_entity_id() == 0;
}

} // namespace

bool aboard(const sim::Unit& unit) {
    return (unit.transport_id() != 0 || unit.parent_entity_id() != 0) && unit.is_mobile() &&
           !unit.has_category("POD");
}

bool selectable(const sim::Entity& e) {
    if (!e.is_unit() || e.destroyed() || e.unselectable()) {
        return false;
    }
    const auto& unit = static_cast<const sim::Unit&>(e);
    return !aboard(unit) && (!unit.is_being_built() || unit.has_category("FACTORY")) &&
           !unit.is_dying() && !unit.has_category("INSIGNIFICANTUNIT");
}

u32 carrier_of(const sim::Entity& e) {
    if (e.parent_entity_id() != 0) {
        return e.parent_entity_id();
    }
    return e.is_unit() ? static_cast<const sim::Unit&>(e).transport_id() : 0;
}

bool inside_ground_quad(const std::array<sim::Vector3, 4>& q, f32 x, f32 z) {
    bool positive = false;
    bool negative = false;
    for (size_t i = 0; i < 4; ++i) {
        const sim::Vector3& a = q[i];
        const sim::Vector3& b = q[(i + 1) % 4];
        const f32 cross = (b.x - a.x) * (z - a.z) - (b.z - a.z) * (x - a.x);
        positive = positive || cross > 0;
        negative = negative || cross < 0;
    }
    return !(positive && negative);
}

std::optional<f32> ray_box_distance(const PickRay& ray, const sim::Vector3& centre,
                                    const sim::Quaternion& orient, const sim::Vector3& half) {
    // Into the box's own frame: turned back by its orientation.
    const sim::Quaternion back{-orient.x, -orient.y, -orient.z, orient.w};
    const sim::Vector3 o = sim::quat_rotate(
        back, {ray.origin.x - centre.x, ray.origin.y - centre.y, ray.origin.z - centre.z});
    const sim::Vector3 d = sim::quat_rotate(back, ray.dir);
    // The slabs: where the ray is inside each pair of faces.
    f32 t_in = -std::numeric_limits<f32>::max();
    f32 t_out = std::numeric_limits<f32>::max();
    const f32 os[3] = {o.x, o.y, o.z};
    const f32 ds[3] = {d.x, d.y, d.z};
    const f32 hs[3] = {half.x, half.y, half.z};
    for (int i = 0; i < 3; ++i) {
        if (std::abs(ds[i]) < 1e-8f) {
            if (std::abs(os[i]) > hs[i]) return std::nullopt; // parallel, outside
            continue;
        }
        f32 a = (-hs[i] - os[i]) / ds[i];
        f32 b = (hs[i] - os[i]) / ds[i];
        if (a > b) std::swap(a, b);
        t_in = std::max(t_in, a);
        t_out = std::min(t_out, b);
        if (t_in > t_out) return std::nullopt;
    }
    if (t_out < 0) return std::nullopt; // behind it
    return std::max(t_in, 0.0f);
}

bool solid_meets_box(const PickSolid& solid, const sim::Vector3& lo, const sim::Vector3& hi) {
    for (const sim::Vector3& n : solid.inward) {
        const f32 x = n.x >= 0 ? hi.x : lo.x;
        const f32 y = n.y >= 0 ? hi.y : lo.y;
        const f32 z = n.z >= 0 ? hi.z : lo.z;
        if (n.x * (x - solid.eye.x) + n.y * (y - solid.eye.y) + n.z * (z - solid.eye.z) < 0) {
            return false;
        }
    }
    return true;
}

std::optional<std::array<f32, 2>> screen_point(const std::array<f32, 16>& view_proj,
                                               const sim::Vector3& p, f32 width, f32 height) {
    // As the overlays project: clip = VP * p, and our projection's y runs
    // down the screen.
    const f32 cx = view_proj[0] * p.x + view_proj[4] * p.y + view_proj[8] * p.z + view_proj[12];
    const f32 cy = view_proj[1] * p.x + view_proj[5] * p.y + view_proj[9] * p.z + view_proj[13];
    const f32 cw = view_proj[3] * p.x + view_proj[7] * p.y + view_proj[11] * p.z + view_proj[15];
    if (cw <= 0.001f) return std::nullopt;
    return std::array<f32, 2>{(cx / cw + 1.0f) * 0.5f * width, (cy / cw + 1.0f) * 0.5f * height};
}

std::vector<u32> highest_selection_priority(const std::vector<std::pair<u32, int>>& units) {
    int best = std::numeric_limits<int>::max();
    for (const auto& unit : units) {
        best = std::min(best, unit.second);
    }
    std::vector<u32> kept;
    for (const auto& [id, priority] : units) {
        if (priority == best) {
            kept.push_back(id);
        }
    }
    return kept;
}

void InputHandler::update(Renderer& renderer, sim::SimState& sim, f64 dt,
                          const std::function<bool()>& mouse_over_ui) {
    clock_ += dt;
    prune_selection(sim.entity_registry());
    f64 mx_d, my_d;
    renderer.mouse_position(mx_d, my_d);
    f32 mx = static_cast<f32>(mx_d);
    f32 my = static_cast<f32>(my_d);
    measure_snap_radius(renderer, sim, mx, my);

    const bool lmb_raw = renderer.is_mouse_pressed(GLFW_MOUSE_BUTTON_LEFT);
    const bool rmb_raw = renderer.is_mouse_pressed(GLFW_MOUSE_BUTTON_RIGHT);

    // A press belongs to whatever was under the cursor when it began. One
    // that began over FA's panels is theirs until released: the world sees
    // the button as up throughout, so neither the press nor its release.
    const bool lmb_down = lmb_raw && !lmb_raw_prev_;
    const bool rmb_down = rmb_raw && !rmb_raw_prev_;
    if (lmb_down || rmb_down) {
        const bool over_ui = mouse_over_ui && mouse_over_ui();
        if (lmb_down) lmb_on_ui_ = over_ui;
        if (rmb_down) rmb_on_ui_ = over_ui;
    }
    if (lmb_down && formation_) {
        cycle_formation();
        lmb_on_ui_ = true;
    }
    lmb_raw_prev_ = lmb_raw;
    rmb_raw_prev_ = rmb_raw;
    bool lmb = lmb_raw && !lmb_on_ui_;
    bool rmb = rmb_raw && !rmb_on_ui_;

    // --- Check if mouse is over minimap ---
    f32 map_w = 0, map_h = 0;
    if (sim.terrain()) {
        map_w = static_cast<f32>(sim.terrain()->map_width());
        map_h = static_cast<f32>(sim.terrain()->map_height());
    }

    // The minimap takes clicks where it was drawn: the C++ HUD's corner, or
    // FA's minimap window while that is shown.
    f32 mm_wx = 0, mm_wz = 0;
    bool on_minimap = renderer.minimap().hit_test(mx, my, renderer.width(), renderer.height(),
                                                  map_w, map_h, mm_wx, mm_wz);

    // FA's command mode (a build icon or order button picked in the UI)
    // turns a world click into that command.
    const CommandMode mode = mode_hooks_.current ? mode_hooks_.current() : CommandMode{};
    const bool mode_active = mode.mode == "build" || mode.mode == "order";
    cursor_world_.reset();
    cursor_ray_.reset();
    camera_zoom_ = renderer.camera().zoom();
    hovered_ = 0;
    hovered_command_ = 0;
    if (!on_minimap && !(mouse_over_ui && mouse_over_ui())) {
        f32 wx = 0;
        f32 wz = 0;
        if (world_at(renderer, sim, mx, my, wx, wz)) {
            cursor_world_ = std::array<f32, 2>{wx, wz};
            // The ray the cursor is on, for picking units where they are
            // drawn (an aircraft where it flies).
            set_cursor_view(renderer.camera(), static_cast<f32>(renderer.width()),
                            static_cast<f32>(renderer.height()), mx, my, wx, wz);
            if (!dragging_) {
                hovered_ = unit_under(sim, wx, wz);
            }
        }
        // Shown as the renderer draws it: not while the UI holds the keys
        const bool graph_shown =
            !renderer.ui_keys_blocked() && (renderer.is_key_pressed(GLFW_KEY_LEFT_SHIFT) ||
                                            renderer.is_key_pressed(GLFW_KEY_RIGHT_SHIFT));
        if (graph_shown && !dragging_ && !order_drag_) {
            hovered_command_ =
                waypoint_under_cursor(waypoints_on_screen(command_graph_nodes(), renderer.camera(),
                                                          static_cast<f32>(renderer.width()),
                                                          static_cast<f32>(renderer.height())),
                                      mx, my);
        }
    }

    if (order_drag_) {
        hovered_command_ = order_drag_->command_id;
    }
    converts_to_patrol_ = !moves_to_patrol(sim, hovered_command_).empty();
    if (dropped_) {
        dropped_for_ += dt;
        const auto node = graph_node(dropped_->command_id);
        if (!node || dropped_for_ > 1.0 || std::abs(node->position.x - dropped_from_.x) > 0.01f ||
            std::abs(node->position.z - dropped_from_.z) > 0.01f) {
            dropped_.reset();
        }
    }

    // --- Left mouse: selection, or the minimap ---
    // A press that begins on the minimap is the minimap's until released: it
    // moves the camera while over the map and never selects or drags a box.
    if (lmb && !lmb_was_pressed_) {
        lmb_on_minimap_ = on_minimap && map_w > 0;
        drag_start_x_ = mx;
        drag_start_y_ = my;
        dragging_ = false;
        // In build mode a press starts a line, a click being one of no length
        f32 wx = 0;
        f32 wz = 0;
        build_line_ = std::nullopt;
        if (mode.mode == "build" && !lmb_on_minimap_ && world_at(renderer, sim, mx, my, wx, wz)) {
            build_line_ = std::array<f32, 4>{wx, wz, wx, wz};
            line_drag_build_ = mode.drag_build;
            line_spacing_ = mode.drag_spacing;
        }
        if (mode.mode.empty() && !lmb_on_minimap_ && begin_order_drag(hovered_command_) &&
            mode_hooks_.drag_begin) {
            mode_hooks_.drag_begin();
        }
    }

    if (lmb && order_drag_) {
        const f32 dx = mx - drag_start_x_;
        const f32 dy = my - drag_start_y_;
        if ((order_drag_moved_ || dx * dx + dy * dy > DRAG_THRESHOLD * DRAG_THRESHOLD) &&
            cursor_world_) {
            drag_order_to(sim, (*cursor_world_)[0], (*cursor_world_)[1]);
        }
    } else if (lmb && build_line_) {
        f32 wx = 0;
        f32 wz = 0;
        if (mode.drag_build && world_at(renderer, sim, mx, my, wx, wz)) {
            (*build_line_)[2] = wx;
            (*build_line_)[3] = wz;
        }
    } else if (lmb && lmb_on_minimap_) {
        if (on_minimap) renderer.camera().set_target(mm_wx, mm_wz);
    } else if (lmb && lmb_was_pressed_) {
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
            world_at(renderer, sim, drag_start_x_, drag_start_y_, drag_world_x0_, drag_world_z0_);
            world_at(renderer, sim, drag_end_x_, drag_end_y_, drag_world_x1_, drag_world_z1_);
            const std::array<std::array<f32, 2>, 4> screen = {{{drag_start_x_, drag_start_y_},
                                                               {drag_end_x_, drag_start_y_},
                                                               {drag_end_x_, drag_end_y_},
                                                               {drag_start_x_, drag_end_y_}}};
            for (size_t i = 0; i < 4; ++i) {
                f32 wx = 0;
                f32 wz = 0;
                world_at(renderer, sim, screen[i][0], screen[i][1], wx, wz);
                const f32 wy = sim.terrain() ? sim.terrain()->get_surface_height(wx, wz) : 0.0f;
                drag_quad_[i] = {wx, wy, wz};
            }
        }
    }

    if (!lmb && lmb_was_pressed_) {
        // Left button just released
        if (order_drag_) {
            const u32 command = order_drag_->command_id;
            f32 wx = 0;
            f32 wz = 0;
            if (world_at(renderer, sim, mx, my, wx, wz)) {
                release_order_drag(sim, wx, wz);
            }
            order_drag_.reset();
            if (mode_hooks_.drag_end) {
                mode_hooks_.drag_end(command, mx, my);
            }
        } else if (build_line_) {
            const bool shift = renderer.is_key_pressed(GLFW_KEY_LEFT_SHIFT) ||
                               renderer.is_key_pressed(GLFW_KEY_RIGHT_SHIFT);
            const auto line = *build_line_;
            build_line_ = std::nullopt;
            if (mode.mode == "build") {
                for (const auto& issued :
                     build_line(sim, mode, line[0], line[1], line[2], line[3], shift)) {
                    if (mode_hooks_.issued) {
                        mode_hooks_.issued(issued);
                    }
                }
            }
        } else if (lmb_on_minimap_) {
            lmb_on_minimap_ = false; // the minimap's press: nothing more to do
        } else if (dragging_) {
            handle_drag_select(renderer, sim);
            dragging_ = false;
        } else if (!on_minimap && mode_active) {
            f32 wx, wz;
            const bool shift = renderer.is_key_pressed(GLFW_KEY_LEFT_SHIFT) ||
                               renderer.is_key_pressed(GLFW_KEY_RIGHT_SHIFT);
            if (world_at(renderer, sim, mx, my, wx, wz)) {
                if (auto issued = click_in_command_mode(sim, mode, wx, wz, shift);
                    issued && mode_hooks_.issued)
                    mode_hooks_.issued(*issued);
            }
        } else if (!on_minimap) {
            // A second click soon after and close by is a double-click, not a
            // third (Windows' rule: the click after a double-click is single).
            const bool double_click = !last_click_double_ && last_click_time_ >= 0.0 &&
                                      clock_ - last_click_time_ <= kDoubleClickSeconds &&
                                      std::abs(mx - last_click_x_) <= kDoubleClickPixels &&
                                      std::abs(my - last_click_y_) <= kDoubleClickPixels;
            f32 wx = 0;
            f32 wz = 0;
            if (world_at(renderer, sim, mx, my, wx, wz)) {
                const bool shift = renderer.is_key_pressed(GLFW_KEY_LEFT_SHIFT) ||
                                   renderer.is_key_pressed(GLFW_KEY_RIGHT_SHIFT);
                const f32 aspect = renderer.height() > 0 ? static_cast<f32>(renderer.width()) /
                                                               static_cast<f32>(renderer.height())
                                                         : 1.0f;
                const bool ctrl = renderer.is_key_pressed(GLFW_KEY_LEFT_CONTROL) ||
                                  renderer.is_key_pressed(GLFW_KEY_RIGHT_CONTROL);
                world_click(sim, wx, wz, shift, ctrl, double_click,
                            renderer.camera().view_proj(aspect));
            }
            last_click_double_ = double_click;
            last_click_time_ = clock_;
            last_click_x_ = mx;
            last_click_y_ = my;
        }
    }

    lmb_was_pressed_ = lmb;

    // --- Right mouse: commands (also works on minimap) ---
    const bool shift_held = renderer.is_key_pressed(GLFW_KEY_LEFT_SHIFT) ||
                            renderer.is_key_pressed(GLFW_KEY_RIGHT_SHIFT);
    const bool ctrl_held = renderer.is_key_pressed(GLFW_KEY_LEFT_CONTROL) ||
                           renderer.is_key_pressed(GLFW_KEY_RIGHT_CONTROL);
    if (!rmb && rmb_was_pressed_) {
        if (rmb_removes_) {
            rmb_removes_ = false;
            if (removes_order(hovered_command_, shift_held, ctrl_held)) {
                remove_order(sim, hovered_command_);
            }
        } else {
            right_release(sim);
        }
    }
    if (rmb && rmb_was_pressed_) {
        right_drag(cursor_world_, dt);
    }
    if (rmb && !rmb_was_pressed_) {
        if (!mode_active && removes_order(hovered_command_, shift_held, ctrl_held)) {
            rmb_removes_ = true;
        } else if (on_minimap && map_w > 0 && !selected_.empty()) {
            // Right-click on minimap: issue move to minimap position
            f32 wy = 0;
            if (sim.terrain())
                wy = sim.terrain()->get_surface_height(mm_wx, mm_wz);

            bool shift = renderer.is_key_pressed(GLFW_KEY_LEFT_SHIFT) ||
                         renderer.is_key_pressed(GLFW_KEY_RIGHT_SHIFT);
            sim::UnitCommand cmd;
            cmd.type = sim::CommandType::Move;
            cmd.target_pos = {mm_wx, wy, mm_wz};
            std::vector<u32> ids;
            for (u32 uid : selected_) {
                auto* e = sim.entity_registry().find(uid);
                if (!e || !e->is_unit() || e->destroyed()) continue;
                ids.push_back(uid);
            }
            // Player-issued order: routed so it applies inside a tick (and a
            // networked match broadcasts it).
            sim.set_human_input_active(true);
            sim.route_player_command(ids, cmd, !shift); // shift-click queues, no clear
            sim.set_human_input_active(false);
            spdlog::debug("Minimap move: {} units to ({:.0f},{:.0f})",
                          selected_.size(), mm_wx, mm_wz);
        } else if (mode_active) {
            // Right-click leaves the command mode, as in FA.
            if (mode_hooks_.cancel) mode_hooks_.cancel();
        } else if (!selected_.empty() && cursor_world_) {
            const f32 wx = (*cursor_world_)[0];
            const f32 wz = (*cursor_world_)[1];
            if (converts_to_patrol_ && right_button_order(sim, wx, wz) == sim::CommandType::Move) {
                restart_as_patrol(sim, hovered_command_);
            } else {
                right_press(sim, wx, wz, shift_held);
            }
        }
    }
    rmb_was_pressed_ = rmb;
}

std::vector<CommandGraphNode> InputHandler::command_graph_nodes() const {
    static const CommandGraphStyle kStyle;
    return renderer::command_graph_nodes(command_graph_paths(
        view_, selected_.empty() ? nullptr : &selected_, player_army_,
        [](sim::CommandType type) { return command_graph_key(type).empty() ? nullptr : &kStyle; }));
}

std::optional<CommandGraphNode> InputHandler::graph_node(u32 command_id) const {
    if (command_id == 0) {
        return std::nullopt;
    }
    for (CommandGraphNode& node : command_graph_nodes()) {
        if (node.order.command_id == command_id) {
            return std::move(node);
        }
    }
    return std::nullopt;
}

std::vector<u32> InputHandler::own_units(sim::SimState& sim, const std::vector<u32>& ids) const {
    std::vector<u32> own;
    for (u32 id : ids) {
        const sim::Entity* e = sim.entity_registry().find(id);
        if (e && !e->destroyed() && e->army() == player_army_) {
            own.push_back(id);
        }
    }
    return own;
}

// CWldSession::ProcessCommandDrag
OrderDrag InputHandler::order_drop(sim::SimState& sim, const CommandGraphNode& node, f32 wx, f32 wz,
                                   bool released, u32& target) const {
    OrderDrag drop;
    drop.command_id = node.order.command_id;
    drop.at = {wx, sim.terrain() ? sim.terrain()->get_surface_height(wx, wz) : 0.0f, wz};
    target = 0;
    if (node.order.target_id != 0) {
        if (!released) {
            return drop;
        }
        const sim::Entity* old = sim.entity_registry().find(node.order.target_id);
        f32 best = std::numeric_limits<f32>::max();
        sim.entity_registry().for_each([&](const sim::Entity& e) {
            if (!old || e.destroyed() || e.is_unit() != old->is_unit() || e.army() != old->army() ||
                (!e.is_unit() && !targetable_prop(e)) || !shown(e) ||
                std::find(node.units.begin(), node.units.end(), e.entity_id()) !=
                    node.units.end()) {
                return;
            }
            const f32 d = std::hypot(e.position().x - wx, e.position().z - wz);
            if (d < best || (d == best && e.entity_id() < target)) {
                best = d;
                target = e.entity_id();
            }
        });
        drop.valid = target != 0;
        return drop;
    }
    if (node.order.type == sim::CommandType::BuildMobile && mode_hooks_.can_place) {
        const std::string& bp = node.order.blueprint_id;
        mode_hooks_.can_place(player_army_, bp, wx, wz, drop.command_id);
        const auto& rules = sim.placement_rules(bp, [] { return sim::PlacementRules{}; });
        sim::snap_structure_center(drop.at.x, drop.at.z, rules.size_x, rules.size_z);
        drop.valid = mode_hooks_.can_place(player_army_, bp, drop.at.x, drop.at.z, drop.command_id);
    }
    return drop;
}

bool InputHandler::begin_order_drag(u32 command_id) {
    if (command_id == 0 || player_army_ < 0) {
        return false;
    }
    order_drag_ = OrderDrag{command_id, {}, true, true};
    order_drag_moved_ = false;
    dropped_.reset();
    return true;
}

void InputHandler::drag_order_to(sim::SimState& sim, f32 wx, f32 wz) {
    if (!order_drag_) {
        return;
    }
    const auto node = graph_node(order_drag_->command_id);
    if (!node) {
        order_drag_.reset();
        return;
    }
    u32 target = 0;
    order_drag_ = order_drop(sim, *node, wx, wz, false, target);
    order_drag_moved_ = true;
}

std::optional<sim::SimCallbackEntry> InputHandler::release_order_drag(sim::SimState& sim, f32 wx,
                                                                      f32 wz) {
    if (!order_drag_) {
        return std::nullopt;
    }
    const u32 command = order_drag_->command_id;
    const bool moved = order_drag_moved_;
    order_drag_.reset();
    const auto node = graph_node(command);
    if (!moved || !node) {
        return std::nullopt;
    }
    u32 target = 0;
    OrderDrag drop = order_drop(sim, *node, wx, wz, true, target);
    if (!drop.valid) {
        return std::nullopt;
    }
    sim::SimCallbackEntry cb;
    cb.unit_ids = own_units(sim, node->units);
    if (cb.unit_ids.empty()) {
        return std::nullopt;
    }
    cb.func_name = sim::kSetCommandTargetCallback;
    cb.args["Command"] = static_cast<f64>(command);
    if (target != 0) {
        cb.args["Target"] = static_cast<f64>(target);
        drop.at = sim.entity_registry().find(target)->position();
    } else {
        cb.args["X"] = static_cast<f64>(drop.at.x);
        cb.args["Y"] = static_cast<f64>(drop.at.y);
        cb.args["Z"] = static_cast<f64>(drop.at.z);
    }
    sim.submit_callback(cb);
    drop.held = false;
    dropped_ = drop;
    dropped_from_ = node->position;
    dropped_for_ = 0;
    return cb;
}

std::optional<OrderDrag> InputHandler::order_drag() const {
    if (order_drag_ && order_drag_moved_) {
        return order_drag_;
    }
    return dropped_;
}

std::optional<sim::SimCallbackEntry> InputHandler::remove_order(sim::SimState& sim,
                                                                u32 command_id) {
    const auto node = graph_node(command_id);
    if (!node || player_army_ < 0) {
        return std::nullopt;
    }
    sim::SimCallbackEntry cb;
    cb.unit_ids = own_units(sim, node->units);
    if (cb.unit_ids.empty()) {
        return std::nullopt;
    }
    cb.func_name = sim::kRemoveCommandCallback;
    cb.args["Command"] = static_cast<f64>(command_id);
    sim.submit_callback(cb);
    return cb;
}

std::vector<u32> InputHandler::moves_to_patrol(sim::SimState& sim, u32 hovered) const {
    const sim::WorldSnapshot* cur = view_.cur();
    if (hovered == 0 || player_army_ < 0 || !cur) {
        return {};
    }
    std::vector<u32> ids(selected_.begin(), selected_.end());
    std::sort(ids.begin(), ids.end());
    std::vector<u32> moves;
    for (const u32 id : ids) {
        const sim::Entity* e = sim.entity_registry().find(id);
        const sim::EntityRecord* record = cur->find(id);
        if (!e || e->destroyed() || !e->is_unit() || !record) {
            continue;
        }
        if (static_cast<const sim::Unit*>(e)->has_category("POD")) {
            return {};
        }
        const auto orders = cur->orders_of(*record);
        const auto at = std::find_if(orders.begin(), orders.end(),
                                     [&](const auto& c) { return c.command_id == hovered; });
        if (orders.size() <= 1 || orders.back().command_id == hovered || at == orders.end()) {
            return {};
        }
        if (std::any_of(orders.begin(), orders.end(),
                        [](const auto& c) { return c.type != sim::CommandType::Move; })) {
            return {};
        }
        for (auto it = at; it != orders.end(); ++it) {
            if (it->command_id != 0 &&
                std::find(moves.begin(), moves.end(), it->command_id) == moves.end()) {
                moves.push_back(it->command_id);
            }
        }
    }
    return moves;
}

std::vector<sim::SimCallbackEntry> InputHandler::restart_as_patrol(sim::SimState& sim,
                                                                   u32 hovered) {
    std::vector<sim::SimCallbackEntry> sent;
    for (const u32 command : moves_to_patrol(sim, hovered)) {
        const auto node = graph_node(command);
        if (!node) {
            continue;
        }
        sim::SimCallbackEntry cb;
        cb.unit_ids = own_units(sim, node->units);
        if (cb.unit_ids.empty()) {
            continue;
        }
        cb.func_name = sim::kSetCommandTypeCallback;
        cb.args["Command"] = static_cast<f64>(command);
        cb.args["Type"] = static_cast<f64>(sim::CommandType::Patrol);
        sim.submit_callback(cb);
        sent.push_back(std::move(cb));
    }
    return sent;
}

bool InputHandler::world_at(const Renderer& renderer, const sim::SimState& sim, f32 mx, f32 my,
                            f32& wx, f32& wz) {
    // Where the cursor's ray meets the ground, or the water over it (M217a):
    // clicks had met the plane y = 0, short of the cursor on high ground.
    f32 wy = 0.0f;
    return renderer.camera().pick_ground(mx, my, static_cast<f32>(renderer.width()),
                                         static_cast<f32>(renderer.height()), sim.terrain(), wx, wy,
                                         wz);
}

void InputHandler::snap_to_deposit(const sim::SimState& sim, const CommandMode& mode, f32& x,
                                   f32& z) const {
    if (mode.mode != "build") {
        return;
    }
    if (const auto at = sim::deposit_snap(sim.resource_deposits(), mode.deposit, x, z,
                                          mode.footprint_x, mode.footprint_z, snap_radius_)) {
        x = at->first;
        z = at->second;
    }
}

void InputHandler::measure_snap_radius(const Renderer& renderer, const sim::SimState& sim, f32 mx,
                                       f32 my) {
    f32 wx = 0;
    f32 wz = 0;
    const f32 width = static_cast<f32>(renderer.width());
    if (width <= 0 || !world_at(renderer, sim, mx, my, wx, wz)) {
        return;
    }
    const Camera& camera = renderer.camera();
    f32 ex = 0;
    f32 ey = 0;
    f32 ez = 0;
    camera.eye_position(ex, ey, ez);
    const f32 wy = sim.terrain() ? sim.terrain()->get_terrain_height(wx, wz) : 0.0f;
    const f32 dist =
        std::sqrt((wx - ex) * (wx - ex) + (wy - ey) * (wy - ey) + (wz - ez) * (wz - ez));
    snap_radius_ = sim::extract_snap_radius(2.0f * dist * std::tan(camera.fov() * 0.5f) / width);
}

void InputHandler::left_click_at(sim::SimState& sim, f32 wx, f32 wz, bool shift, bool ctrl) {
    const u32 picked = unit_under(sim, wx, wz, true);

    if (picked != 0 && ctrl) {
        select_blueprint_of(sim, picked, shift);
        return;
    }

    if (!shift && !ctrl) {
        selected_.clear();
    }

    if (picked != 0) {
        if (shift && selected_.count(picked))
            selected_.erase(picked);
        else
            selected_.insert(picked);
    }

    selection_event_ = true;
    spdlog::debug("Selection: {} units (click at world {:.0f},{:.0f})",
                  selected_.size(), wx, wz);
}

void InputHandler::select_blueprint_of(sim::SimState& sim, u32 picked, bool shift) {
    const sim::Entity* hovered = sim.entity_registry().find(picked);
    if (!hovered || !hovered->is_unit()) {
        return;
    }
    const std::string blueprint = static_cast<const sim::Unit&>(*hovered).blueprint_id();
    const auto of_blueprint = [&](u32 id) {
        const sim::Entity* e = sim.entity_registry().find(id);
        return e && e->is_unit() && static_cast<const sim::Unit&>(*e).blueprint_id() == blueprint;
    };
    selection_event_ = true;
    if (shift && selected_.count(picked)) {
        std::erase_if(selected_, of_blueprint);
        return;
    }
    if (!shift) {
        selected_.clear();
    }
    sim.entity_registry().for_each_unit([&](const sim::Entity& e) {
        if (e.destroyed() || e.army() != player_army_ || !selectable(e)) {
            return;
        }
        const auto& unit = static_cast<const sim::Unit&>(e);
        if (unit.blueprint_id() == blueprint && !unit.is_being_built()) {
            selected_.insert(e.entity_id());
        }
    });
}

void InputHandler::prune_selection(const sim::EntityRegistry& registry) {
    std::erase_if(selected_, [&](u32 id) {
        const sim::Entity* e = registry.find(id);
        if (!e || !e->is_unit() || e->destroyed()) {
            return false;
        }
        const auto& unit = static_cast<const sim::Unit&>(*e);
        return aboard(unit) || unit.is_dying();
    });
}

void InputHandler::world_click(sim::SimState& sim, f32 wx, f32 wz, bool shift, bool ctrl,
                               bool double_click, const std::array<f32, 16>& view_proj) {
    // A double-click's second press is no click of its own: Moho's world view
    // gets a ButtonDClick in its place, and it only adds the clicked unit's
    // like in view (with Shift held too: the first click's toggle stands).
    if (double_click) select_similar_in_view(sim, wx, wz, view_proj);
    else left_click_at(sim, wx, wz, shift, ctrl);
}

void InputHandler::select_similar_in_view(sim::SimState& sim, f32 wx, f32 wz,
                                          const std::array<f32, 16>& view_proj) {
    const u32 picked = unit_under(sim, wx, wz, true);
    const sim::Entity* hovered = picked ? sim.entity_registry().find(picked) : nullptr;
    if (!hovered || !hovered->is_unit() || hovered->army() != player_army_) return;
    const auto& unit = static_cast<const sim::Unit&>(*hovered);
    if (unit.has_category("WALL")) return;
    const std::string& blueprint = unit.blueprint_id();
    const auto in_view = [&](const sim::Vector3& p) {
        // Column-major: clip = view_proj * (p, 1); in the frustum with
        // |x|, |y| <= w and 0 <= z <= w (Vulkan's depth).
        const auto& m = view_proj;
        const f32 x = m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12];
        const f32 y = m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13];
        const f32 z = m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14];
        const f32 w = m[3] * p.x + m[7] * p.y + m[11] * p.z + m[15];
        return w > 0.0f && std::abs(x) <= w && std::abs(y) <= w && z >= 0.0f && z <= w;
    };
    sim.entity_registry().for_each_unit([&](const sim::Entity& e) {
        if (e.entity_id() == picked || e.army() != player_army_ || !selectable(e)) return;
        const auto& other = static_cast<const sim::Unit&>(e);
        if (other.blueprint_id() != blueprint || other.has_unit_state("BeingUpgraded")) return;
        if (!in_view(view_.position(e))) return;
        selected_.insert(e.entity_id());
    });
    selection_event_ = true;
}

void InputHandler::handle_drag_select(Renderer& renderer,
                                      sim::SimState& sim) {
    const bool shift = renderer.is_key_pressed(GLFW_KEY_LEFT_SHIFT) ||
                       renderer.is_key_pressed(GLFW_KEY_RIGHT_SHIFT);
    const f32 sw = static_cast<f32>(renderer.width());
    const f32 sh = static_cast<f32>(renderer.height());
    select_in_box(sim, renderer.camera().view_proj(sh > 0 ? sw / sh : 1.0f), sw, sh, drag_start_x_,
                  drag_start_y_, drag_end_x_, drag_end_y_, shift);
}

void InputHandler::select_in_box(sim::SimState& sim, const std::array<f32, 16>& view_proj,
                                 f32 width, f32 height, f32 x0, f32 y0, f32 x1, f32 y1,
                                 bool shift) {
    if (!shift)
        selected_.clear();
    // What the box holds on the screen, where each unit is drawn: an
    // aircraft at its height, not the ground under it (which the box's
    // footprint on the ground had missed).
    const f32 sx0 = std::min(x0, x1);
    const f32 sx1 = std::max(x0, x1);
    const f32 sy0 = std::min(y0, y1);
    const f32 sy1 = std::max(y0, y1);
    std::vector<std::pair<u32, int>> boxed;
    sim.entity_registry().for_each_unit([&](const sim::Entity& e) {
        if (e.army() != player_army_ || !selectable(e)) return;
        const auto at = screen_point(view_proj, view_.position(e), width, height);
        if (at && (*at)[0] >= sx0 && (*at)[0] <= sx1 && (*at)[1] >= sy0 && (*at)[1] <= sy1)
            boxed.emplace_back(e.entity_id(),
                               static_cast<const sim::Unit&>(e).selection_priority());
    });
    if (shift) {
        for (const auto& unit : boxed) {
            selected_.insert(unit.first);
        }
    } else {
        for (u32 id : highest_selection_priority(boxed)) {
            selected_.insert(id);
        }
    }

    selection_event_ = true;
    spdlog::debug("Drag select: {} units in ({:.0f},{:.0f})-({:.0f},{:.0f})", selected_.size(), sx0,
                  sy0, sx1, sy1);
}

std::vector<std::pair<sim::UnitCommand, std::vector<u32>>>
InputHandler::right_click_orders(sim::SimState& sim, f32 wx, f32 wz) const {
    auto& registry = sim.entity_registry();
    const sim::ArmyBrain* me = sim.get_army(player_army_);
    const auto allied = [&](i32 army) {
        return army == player_army_ || (me && army >= 0 && me->is_ally(army));
    };
    const auto live = [&](u32 id) -> sim::Entity* {
        sim::Entity* e = registry.find(id);
        return e && !e->destroyed() ? e : nullptr;
    };
    enum class On : u8 { Ground, Enemy, Ally, Wreck };
    On on = On::Ground;
    u32 target = 0;
    if (const sim::Entity* e = live(unit_under(sim, wx, wz)); e && e->army() >= 0) {
        target = e->entity_id();
        on = allied(e->army()) ? On::Ally : On::Enemy;
    } else {
        target = prop_under(sim, wx, wz);
        if (target != 0) {
            on = On::Wreck;
        }
    }

    const f32 wy = sim.terrain() ? sim.terrain()->get_surface_height(wx, wz) : 0.0f;
    const sim::Entity* t = target ? live(target) : nullptr;
    const auto* tu = t && t->is_unit() ? static_cast<const sim::Unit*>(t) : nullptr;
    // Each unit's default order there.
    const auto order_for = [&](const sim::Unit& u) {
        sim::UnitCommand cmd;
        cmd.type = sim::CommandType::Move;
        cmd.target_pos = {wx, wy, wz};
        const auto aim = [&](sim::CommandType type) {
            cmd.type = type;
            cmd.target_id = target;
            cmd.target_pos = view_.position(*t);
        };
        if (on == On::Enemy) {
            if (u.has_command_cap("RULEUCC_Attack")) {
                aim(sim::CommandType::Attack);
            } else if (u.has_command_cap("RULEUCC_Capture") && tu && capture_target(*tu)) {
                aim(sim::CommandType::Capture);
            } else if (u.has_command_cap("RULEUCC_Reclaim") && tu && tu->parent_entity_id() == 0 &&
                       sim::reclaim_target_valid(*tu)) {
                aim(sim::CommandType::Reclaim);
            }
        } else if (on == On::Ally && tu) {
            // An aircraft, flying or landed.
            if (tu->has_category("AIRSTAGINGPLATFORM") && u.has_command_cap("RULEUCC_Dock") &&
                u.can_fly())
                aim(sim::CommandType::Dock);
            else if (tu->has_category("TRANSPORTATION") &&
                     u.has_command_cap("RULEUCC_CallTransport") && !u.can_fly())
                aim(sim::CommandType::TransportLoad);
            else if (tu->is_being_built() && u.has_command_cap("RULEUCC_Repair"))
                aim(sim::CommandType::Repair);
            else if (u.has_command_cap("RULEUCC_Guard")) aim(sim::CommandType::Guard);
        } else if (on == On::Wreck && t) {
            if (u.has_command_cap("RULEUCC_Reclaim")) aim(sim::CommandType::Reclaim);
        }
        return cmd;
    };

    // One order per kind (and target), in id order, each routed to its units.
    std::vector<u32> ids;
    for (u32 id : selected_)
        if (const sim::Entity* e = live(id); e && e->is_unit() && id != target) ids.push_back(id);
    std::sort(ids.begin(), ids.end());
    std::vector<std::pair<sim::UnitCommand, std::vector<u32>>> groups;
    for (u32 id : ids) {
        const sim::UnitCommand cmd = order_for(static_cast<const sim::Unit&>(*live(id)));
        auto it = std::find_if(groups.begin(), groups.end(), [&](const auto& g) {
            return g.first.type == cmd.type && g.first.target_id == cmd.target_id;
        });
        if (it == groups.end()) groups.push_back({cmd, {id}});
        else it->second.push_back(id);
    }
    return groups;
}

std::optional<sim::CommandType> InputHandler::right_button_order(sim::SimState& sim, f32 wx,
                                                                 f32 wz) const {
    const auto groups = right_click_orders(sim, wx, wz);
    if (groups.empty()) {
        return std::nullopt;
    }
    for (const auto& group : groups) {
        if (group.first.type != sim::CommandType::Move) {
            return group.first.type;
        }
    }
    return sim::CommandType::Move;
}

std::vector<IssuedCommand> InputHandler::right_click_at(sim::SimState& sim, f32 wx, f32 wz,
                                                        bool shift) {
    return issue_right_orders(sim, right_click_orders(sim, wx, wz), shift, nullptr);
}

std::vector<IssuedCommand> InputHandler::issue_right_orders(
    sim::SimState& sim, const std::vector<std::pair<sim::UnitCommand, std::vector<u32>>>& orders,
    bool shift, const FormationDrag* formation) {
    std::vector<IssuedCommand> issued;
    for (const auto& [order, units] : orders) {
        sim::UnitCommand cmd = order;
        if (formation && cmd.type == sim::CommandType::Move && formation->units.size() > 1) {
            cmd.formation = formation->script;
            cmd.has_facing = true;
            cmd.facing = formation->facing;
        }
        // Player-issued: routed so it applies inside a tick (and a networked
        // match broadcasts it); a move goes to factories as their rally point.
        sim.set_human_input_active(true);
        sim.route_player_command(units, cmd, !shift);
        sim.set_human_input_active(false);
        IssuedCommand out;
        out.position = cmd.target_pos;
        out.target_id = cmd.target_id;
        out.clear = !shift;
        out.units = units;
        switch (cmd.type) {
        case sim::CommandType::Attack: out.type = "Attack"; break;
        case sim::CommandType::Capture: out.type = "Capture"; break;
        case sim::CommandType::Guard: out.type = "Guard"; break;
        case sim::CommandType::Repair: out.type = "Repair"; break;
        case sim::CommandType::TransportLoad: out.type = "TransportLoadUnits"; break;
        case sim::CommandType::Dock: out.type = "Dock"; break;
        case sim::CommandType::Reclaim: out.type = "Reclaim"; break;
        default: out.type = "Move"; break;
        }
        issued.push_back(out);
    }
    return issued;
}

void InputHandler::right_press(sim::SimState& sim, f32 wx, f32 wz, bool shift) {
    formation_.reset();
    pending_right_ = PendingRight{right_click_orders(sim, wx, wz), shift};
    const auto move =
        std::find_if(pending_right_->orders.begin(), pending_right_->orders.end(),
                     [](const auto& order) { return order.first.type == sim::CommandType::Move; });
    if (move == pending_right_->orders.end()) {
        return;
    }
    FormationDrag drag;
    drag.at = move->first.target_pos;
    drag.mouse = drag.at;
    bool flies = false;
    bool walks = false;
    f32 sx = 0;
    f32 sz = 0;
    for (const u32 id : move->second) {
        const auto* e = sim.entity_registry().find(id);
        if (!e || !e->is_unit()) {
            continue;
        }
        const auto& u = static_cast<const sim::Unit&>(*e);
        if (u.has_category("RALLYPOINT")) {
            continue;
        }
        drag.units.push_back(id);
        (u.can_fly() ? flies : walks) = true;
        sim::Vector3 from = view_.position(u);
        if (shift && !u.command_queue().empty()) {
            from = u.command_queue().back().target_pos;
        }
        sx += from.x;
        sz += from.z;
    }
    if (drag.units.empty()) {
        return;
    }
    // Moho's mixed formations are its surface ones (FORMATION_GetScriptName)
    const bool air = flies && !walks;
    formation_scripts_ = mode_hooks_.formation_scripts ? mode_hooks_.formation_scripts(air)
                                                       : std::vector<std::string>{};
    if (formation_scripts_.empty()) {
        return;
    }
    if (formation_index_ >= formation_scripts_.size()) {
        formation_index_ = 0;
    }
    drag.script = formation_scripts_[formation_index_];
    const f32 n = static_cast<f32>(drag.units.size());
    const f32 dx = drag.at.x - sx / n;
    const f32 dz = drag.at.z - sz / n;
    if (dx * dx + dz * dz > 0) {
        drag.facing = std::atan2(dx, dz);
    }
    formation_ = std::move(drag);
}

void InputHandler::right_drag(std::optional<std::array<f32, 2>> cursor, f64 dt) {
    if (!formation_) {
        return;
    }
    FormationDrag& drag = *formation_;
    drag.wait = std::max(0.0f, drag.wait - static_cast<f32>(dt));
    if (!drag.settled() || !cursor) {
        return;
    }
    const f32 x = (*cursor)[0];
    const f32 z = (*cursor)[1];
    const auto far = [&](const sim::Vector3& p) {
        return (p.x - x) * (p.x - x) + (p.z - z) * (p.z - z) >= 0.0025f;
    };
    if (!far(drag.mouse) || !far(drag.at)) {
        return;
    }
    drag.mouse = {x, drag.mouse.y, z};
    drag.facing = std::atan2(x - drag.at.x, z - drag.at.z);
}

std::vector<IssuedCommand> InputHandler::right_release(sim::SimState& sim) {
    std::vector<IssuedCommand> issued;
    if (pending_right_) {
        const FormationDrag* formation =
            formation_ && formation_->settled() ? &*formation_ : nullptr;
        issued = issue_right_orders(sim, pending_right_->orders, pending_right_->shift, formation);
    }
    pending_right_.reset();
    formation_.reset();
    return issued;
}

const std::vector<FormationGhost>& InputHandler::formation_ghosts(const sim::SimState& sim) {
    if (!formation_ || !formation_->settled() || !mode_hooks_.formation_slots) {
        formation_ghosts_.clear();
        formation_ghosts_for_.reset();
        return formation_ghosts_;
    }
    const std::pair<std::string, f32> key{formation_->script, formation_->facing};
    if (formation_ghosts_for_ == key) {
        return formation_ghosts_;
    }
    formation_ghosts_for_ = key;
    formation_ghosts_.clear();
    std::vector<sim::FormationMember> members;
    for (const u32 id : formation_->units) {
        const sim::Entity* e = sim.entity_registry().find(id);
        if (!e || e->destroyed() || !e->is_unit() || e->parent_entity_id() != 0 ||
            static_cast<const sim::Unit*>(e)->is_being_built()) {
            continue;
        }
        members.push_back({id, e->army(), view_.position(*e),
                           std::max(e->footprint_size_x(), e->footprint_size_z()),
                           e->blueprint_id()});
    }
    for (const sim::FormationSlot& slot : mode_hooks_.formation_slots(
             members, formation_->script, formation_->at, formation_->facing)) {
        const sim::Entity* e = sim.entity_registry().find(slot.unit_id);
        if (e) {
            formation_ghosts_.push_back({e->blueprint_id(), slot.position, formation_->facing});
        }
    }
    return formation_ghosts_;
}

bool InputHandler::cycle_formation() {
    if (!formation_ || formation_scripts_.size() < 2) {
        return false;
    }
    formation_index_ = (formation_index_ + 1) % formation_scripts_.size();
    formation_->script = formation_scripts_[formation_index_];
    formation_->wait = 0;
    return true;
}

namespace {

/// An FA order cap and the sim command it issues. `targets_unit`: the order
/// needs a unit (or, for reclaim, any entity) under the click.
struct OrderSpec {
    const char* cap;
    sim::CommandType type;
    const char* fa_type; // CommandType as FA's OnCommandIssued sees it
    bool targets_unit;
};

constexpr OrderSpec kOrders[] = {
    {"RULEUCC_Move", sim::CommandType::Move, "Move", false},
    {"RULEUCC_Attack", sim::CommandType::Attack, "Attack", false},
    {"RULEUCC_Patrol", sim::CommandType::Patrol, "Patrol", false},
    {"RULEUCC_Guard", sim::CommandType::Guard, "Guard", true},
    {"RULEUCC_Reclaim", sim::CommandType::Reclaim, "Reclaim", true},
    {"RULEUCC_Repair", sim::CommandType::Repair, "Repair", true},
    {"RULEUCC_Capture", sim::CommandType::Capture, "Capture", true},
    {"RULEUCC_Transport", sim::CommandType::TransportUnload, "TransportUnloadUnits", false},
    {"RULEUCC_Ferry", sim::CommandType::Ferry, "Ferry", false},
    {"RULEUCC_Teleport", sim::CommandType::Teleport, "Teleport", false},
    {"RULEUCC_Nuke", sim::CommandType::Nuke, "Nuke", false},
    {"RULEUCC_Tactical", sim::CommandType::Tactical, "Tactical", false},
    {"RULEUCC_Overcharge", sim::CommandType::Overcharge, "Overcharge", true},
    {"RULEUCC_Sacrifice", sim::CommandType::Sacrifice, "Sacrifice", true},
};

} // namespace

std::optional<IssuedCommand> InputHandler::click_in_command_mode(
    sim::SimState& sim, const CommandMode& mode, f32 wx, f32 wz, bool shift) {
    if (placing_template(mode)) {
        // A click is a drag of no length: every structure it orders, all but
        // the last reported here, the last (which ends the mode without
        // Shift) returned.
        auto issued = build_line(sim, mode, wx, wz, wx, wz, shift);
        if (issued.empty()) return std::nullopt;
        for (size_t i = 0; i + 1 < issued.size(); ++i)
            if (mode_hooks_.issued) mode_hooks_.issued(issued[i]);
        return issued.back();
    }
    snap_to_deposit(sim, mode, wx, wz);
    IssuedCommand out;
    out.clear = !shift;
    sim::UnitCommand cmd;
    std::vector<u32> ids;

    auto live_selected = [&](auto keep) {
        for (u32 uid : selected_) {
            auto* e = sim.entity_registry().find(uid);
            if (!e || !e->is_unit() || e->destroyed()) continue;
            if (keep(static_cast<const sim::Unit&>(*e))) ids.push_back(uid);
        }
        std::sort(ids.begin(), ids.end());
    };
    auto surface_y = [&](f32 x, f32 z) {
        return sim.terrain() ? sim.terrain()->get_surface_height(x, z) : 0.0f;
    };

    if (mode.mode == "build") {
        if (mode.name.empty()) return std::nullopt;
        sim::snap_structure_center(wx, wz, mode.footprint_x, mode.footprint_z);
        if (mode_hooks_.can_place && !mode_hooks_.can_place(player_army_, mode.name, wx, wz, 0)) {
            return std::nullopt;
        }
        // Mobile builders take the order; factories build through their queue.
        live_selected([](const sim::Unit& u) {
            return u.build_rate() > 0 && !u.has_category("STRUCTURE");
        });
        cmd.type = sim::CommandType::BuildMobile;
        cmd.blueprint_id = mode.name;
        out.type = "BuildMobile";
        out.blueprint = mode.name;
    } else if (mode.mode == "order" && mode.name == "RULEUCC_Script") {
        // An ability (M206w): the orders panel's button names its task. The
        // units it is for take it (ABILITYBUTTON, the category the panel
        // shows abilities for; they have no RULEUCC_Script cap), the click
        // as its Location.
        if (!mode.script_args_at) return std::nullopt;
        cmd.type = sim::CommandType::Script;
        cmd.script_args = mode.script_args_at({wx, surface_y(wx, wz), wz});
        if (cmd.script_args.empty()) return std::nullopt;
        out.type = "Script";
        live_selected([](const sim::Unit& u) { return u.has_category("ABILITYBUTTON"); });
    } else if (mode.mode == "order") {
        const OrderSpec* spec = nullptr;
        for (const auto& o : kOrders)
            if (mode.name == o.cap) { spec = &o; break; }
        if (!spec) return std::nullopt;
        cmd.type = spec->type;
        out.type = spec->fa_type;
        cmd.target_id = target_under(sim, wx, wz, spec->type);
        if (spec->targets_unit && cmd.target_id == 0) return std::nullopt;
        live_selected([&](const sim::Unit& u) {
            return u.has_command_cap(spec->cap) && u.entity_id() != cmd.target_id;
        });
    } else {
        return std::nullopt; // no mode, or one without a world click (ping)
    }
    // An Attack on bare ground: a mobile unit on ReturnFire attack-moves
    // there, and only the others get the ground attack (Moho's
    // SplitSelectionForAggressiveMove).
    std::vector<u32> attack_movers;
    if (cmd.type == sim::CommandType::Attack && cmd.target_id == 0) {
        std::vector<u32> rest;
        for (u32 id : ids) {
            const auto& u = static_cast<const sim::Unit&>(*sim.entity_registry().find(id));
            (u.is_mobile() && u.fire_state() == 0 ? attack_movers : rest).push_back(id);
        }
        ids = std::move(rest);
        if (ids.empty()) out.type = "AggressiveMove";
    }
    if (ids.empty() && attack_movers.empty()) return std::nullopt;

    cmd.target_pos = {wx, surface_y(wx, wz), wz};
    out.position = cmd.target_pos;
    out.target_id = cmd.target_id;
    // Player-issued order: routed so a networked match broadcasts it.
    sim.set_human_input_active(true);
    // Moho's patrol starts where the group stands, or where the last unit's queue ends with the
    // orders not yet run, unless one is patrolling (ResolveGroupMoveAnchorOrDetectPatrol)
    bool patrolling = false;
    const sim::UnitCommand* queue_end = nullptr;
    f32 ax = 0;
    f32 az = 0;
    const auto pending =
        shift ? sim.command_scheduler().pending() : std::vector<sim::ScheduledCommand>();
    for (u32 id : ids) {
        const auto& u = static_cast<const sim::Unit&>(*sim.entity_registry().find(id));
        ax += u.position().x;
        az += u.position().z;
        if (!shift) {
            continue;
        }
        const auto& queue = u.command_queue();
        const sim::UnitCommand* last = queue.empty() ? nullptr : &queue.back();
        for (const auto& scheduled : pending) {
            const sim::UnitCommand& order = scheduled.command;
            if (scheduled.callback || order.factory ||
                std::find(scheduled.unit_ids.begin(), scheduled.unit_ids.end(), id) ==
                    scheduled.unit_ids.end()) {
                continue;
            }
            if (order.type == sim::CommandType::Stop) {
                last = nullptr;
            } else if (order.type != sim::CommandType::SiloBuildNuke &&
                       order.type != sim::CommandType::SiloBuildTactical &&
                       sim.takes_command(u, order)) {
                last = &order;
            }
        }
        if (last) {
            patrolling |= last->type == sim::CommandType::Patrol;
            queue_end = last;
        }
    }
    if (cmd.type == sim::CommandType::Patrol && !patrolling) {
        if (queue_end) {
            ax = queue_end->target_pos.x;
            az = queue_end->target_pos.z;
        } else {
            ax /= static_cast<f32>(ids.size());
            az /= static_cast<f32>(ids.size());
        }
        sim::UnitCommand anchor = cmd;
        anchor.target_pos = {ax, surface_y(ax, az), az};
        sim.route_player_command(ids, anchor, !shift);
        sim.route_player_command(ids, cmd, false);
    } else if (!ids.empty()) {
        sim.route_player_command(ids, cmd, !shift);
    }
    if (!attack_movers.empty()) {
        sim::UnitCommand move = cmd;
        move.type = sim::CommandType::AggressiveMove;
        sim.route_player_command(attack_movers, move, !shift);
    }
    sim.set_human_input_active(false);
    out.units = ids;
    out.units.insert(out.units.end(), attack_movers.begin(), attack_movers.end());
    std::sort(out.units.begin(), out.units.end());
    return out;
}

std::vector<IssuedCommand> InputHandler::build_line(sim::SimState& sim, const CommandMode& mode,
                                                    f32 x0, f32 z0, f32 x1, f32 z1, bool shift) {
    std::vector<IssuedCommand> issued;
    if (mode.mode != "build" || mode.name.empty()) {
        return issued;
    }
    if (!placing_template(mode)) { // a template's structures don't snap
        snap_to_deposit(sim, mode, x0, z0);
        snap_to_deposit(sim, mode, x1, z1);
    }
    // Mobile builders take the orders; factories build through their queue.
    std::vector<u32> ids;
    for (u32 uid : selected_) {
        const auto* e = sim.entity_registry().find(uid);
        if (!e || !e->is_unit() || e->destroyed()) {
            continue;
        }
        const auto& u = static_cast<const sim::Unit&>(*e);
        if (u.build_rate() > 0 && !u.has_category("STRUCTURE")) {
            ids.push_back(uid);
        }
    }
    std::sort(ids.begin(), ids.end());
    if (ids.empty()) {
        return issued;
    }
    const auto order = [&](const std::string& bp, f32 x, f32 z) {
        const bool clear = issued.empty() && !shift;
        sim::UnitCommand cmd;
        cmd.type = sim::CommandType::BuildMobile;
        cmd.blueprint_id = bp;
        cmd.target_pos = {x, sim.terrain() ? sim.terrain()->get_surface_height(x, z) : 0.0f, z};
        // Player-issued order: routed so a networked match broadcasts it.
        sim.set_human_input_active(true);
        sim.route_player_command(ids, cmd, clear);
        sim.set_human_input_active(false);
        IssuedCommand out;
        out.type = "BuildMobile";
        out.blueprint = bp;
        out.position = cmd.target_pos;
        out.clear = false;
        out.units = ids;
        issued.push_back(out);
    };
    if (placing_template(mode)) {
        // Each structure of each copy that may stand where it falls; one
        // ordered before counts against the next (can_place sees it queued).
        for (const TemplateSite& site : template_sites_for(mode, x0, z0, x1, z1)) {
            if (mode_hooks_.can_place &&
                !mode_hooks_.can_place(player_army_, site.blueprint_id, site.x, site.z, 0))
                continue;
            order(site.blueprint_id, site.x, site.z);
        }
        if (!issued.empty()) issued.back().clear = !shift;
        return issued;
    }
    if (!mode.drag_build) {
        x1 = x0;
        z1 = z0;
    }
    for (const auto& [x, z] : sim::structure_line_sites(x0, z0, x1, z1, mode.footprint_x,
                                                        mode.footprint_z, mode.drag_spacing)) {
        if (mode_hooks_.can_place && !mode_hooks_.can_place(player_army_, mode.name, x, z, 0)) {
            continue;
        }
        order(mode.name, x, z);
    }
    // The last one tells commandmode.lua the line is done, ending the mode
    // without Shift
    if (!issued.empty()) {
        issued.back().clear = !shift;
    }
    return issued;
}

void InputHandler::set_cursor_view(const Camera& camera, f32 width, f32 height, f32 mx, f32 my,
                                   f32 wx, f32 wz) {
    cursor_ray_.reset();
    cursor_solid_.reset();
    const auto ray_at = [&](f32 x, f32 y) -> std::optional<PickRay> {
        f32 o[3];
        f32 d[3];
        if (!camera.screen_ray(x, y, width, height, o, d)) {
            return std::nullopt;
        }
        return PickRay{{o[0], o[1], o[2]}, {d[0], d[1], d[2]}};
    };
    cursor_ray_ = ray_at(mx, my);
    if (!cursor_ray_) {
        return;
    }
    cursor_ray_ground_ = {wx, wz};
    constexpr f32 t = kSelectTolerance;
    const std::array<std::optional<PickRay>, 4> corners = {
        ray_at(mx - t, my - t), ray_at(mx + t, my - t), ray_at(mx + t, my + t),
        ray_at(mx - t, my + t)};
    PickSolid solid{cursor_ray_->origin, {}};
    const sim::Vector3& c = cursor_ray_->dir;
    f32 spread = 0.0f;
    for (size_t i = 0; i < 4; ++i) {
        if (!corners[i] || !corners[(i + 1) % 4]) {
            return;
        }
        const sim::Vector3& a = corners[i]->dir;
        const sim::Vector3& b = corners[(i + 1) % 4]->dir;
        sim::Vector3 n{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
        if (n.x * c.x + n.y * c.y + n.z * c.z < 0) {
            n = {-n.x, -n.y, -n.z};
        }
        solid.inward[i] = n;
        spread = std::max(spread, std::hypot(a.x - c.x, a.y - c.y, a.z - c.z));
    }
    cursor_solid_ = solid;
    const sim::Vector3& eye = cursor_ray_->origin;
    cursor_reach_ = spread * std::hypot(wx - eye.x, eye.y, wz - eye.z);
    cursor_view_proj_ = camera.view_proj(width / height);
    cursor_screen_ = {mx, my};
    screen_size_ = {width, height};
}

u32 InputHandler::unit_under(sim::SimState& sim, f32 wx, f32 wz, bool own_only) const {
    // The cursor's ray if (wx, wz) is what it is on; else straight down.
    const bool on_cursor = cursor_ray_ && std::abs(cursor_ray_ground_[0] - wx) < 1e-3f &&
                           std::abs(cursor_ray_ground_[1] - wz) < 1e-3f;
    const PickRay ray = on_cursor ? *cursor_ray_ : PickRay{{wx, 10000.0f, wz}, {0.0f, -1.0f, 0.0f}};
    // What the ray passes over, from 150 above the ground down to it: an
    // aircraft at height is drawn over ground short of (wx, wz).
    f32 x0 = wx;
    f32 x1 = wx;
    f32 z0 = wz;
    f32 z1 = wz;
    if (on_cursor && ray.dir.y < -1e-4f) {
        const f32 back = 150.0f / -ray.dir.y;
        const f32 hx = wx - ray.dir.x * back;
        const f32 hz = wz - ray.dir.z * back;
        x0 = std::min(x0, hx);
        x1 = std::max(x1, hx);
        z0 = std::min(z0, hz);
        z1 = std::max(z1, hz);
    }
    const bool tolerant = on_cursor && cursor_solid_;
    constexpr f32 kReach = 16.0f; // the widest unit's half size
    const f32 reach = kReach + (tolerant ? cursor_reach_ : 0.0f);
    u32 best_id = 0;
    f32 best_t = std::numeric_limits<f32>::max();
    for (u32 id :
         sim.entity_registry().collect_in_rect(x0 - reach, z0 - reach, x1 + reach, z1 + reach)) {
        const auto* e = sim.entity_registry().find(id);
        if (!e || e->destroyed() || !e->is_unit() || !shown(*e) ||
            static_cast<const sim::Unit&>(*e).is_dying()) {
            continue;
        }
        if (own_only && (e->army() != player_army_ || !selectable(*e))) {
            continue;
        }
        const auto& unit = static_cast<const sim::Unit&>(*e);
        if (unit.has_category("UNTARGETABLE") && !unit.has_category("SELECTABLE") &&
            !unit.has_category("FERRYBEACON")) {
            continue;
        }
        const sim::Vector3 pos = view_.position(*e);
        const sim::Quaternion orient = view_.orientation(*e);
        sim::Vector3 lo{-std::max(unit.size_x(), 0.5f) * 0.5f, 0.0f,
                        -std::max(unit.size_z(), 0.5f) * 0.5f};
        sim::Vector3 hi{-lo.x, std::max(unit.size_y(), 0.5f), -lo.z};
        const sim::BoneData* model = unit.bone_data();
        if (model && model->mesh_bounds) {
            const sim::Vector3 s{model->model_scale * e->scale_x(),
                                 model->model_scale * e->scale_y(),
                                 model->model_scale * e->scale_z()};
            lo = {s.x * model->mesh_bounds->lo.x, s.y * model->mesh_bounds->lo.y,
                  s.z * model->mesh_bounds->lo.z};
            hi = {s.x * model->mesh_bounds->hi.x, s.y * model->mesh_bounds->hi.y,
                  s.z * model->mesh_bounds->hi.z};
        }
        const sim::Vector3 half{(hi.x - lo.x) * 0.5f, (hi.y - lo.y) * 0.5f, (hi.z - lo.z) * 0.5f};
        const sim::Vector3 mid = sim::quat_rotate(
            orient, {(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f});
        const sim::Vector3 centre{pos.x + mid.x, pos.y + mid.y, pos.z + mid.z};
        const PickBlueprint pick = tolerant && mode_hooks_.pick_blueprint
                                       ? mode_hooks_.pick_blueprint(unit.blueprint_id())
                                       : PickBlueprint{};
        const bool by_ray = !tolerant || (unit.is_mobile() && pick.oob_test_zoom > camera_zoom_);
        std::optional<f32> t;
        if (by_ray) {
            t = ray_box_distance(ray, centre, orient, half);
        } else {
            const sim::Vector3 ax = sim::quat_rotate(orient, {half.x, 0.0f, 0.0f});
            const sim::Vector3 ay = sim::quat_rotate(orient, {0.0f, half.y, 0.0f});
            const sim::Vector3 az = sim::quat_rotate(orient, {0.0f, 0.0f, half.z});
            const sim::Vector3 ext{
                (std::abs(ax.x) + std::abs(ay.x) + std::abs(az.x)) * pick.mesh_scale_x,
                std::abs(ax.y) + std::abs(ay.y) + std::abs(az.y),
                (std::abs(ax.z) + std::abs(ay.z) + std::abs(az.z)) * pick.mesh_scale_z};
            sim::Vector3 box_lo{centre.x - ext.x, centre.y - ext.y, centre.z - ext.z};
            sim::Vector3 box_hi{centre.x + ext.x, centre.y + ext.y, centre.z + ext.z};
            const f32 height = box_hi.y - box_lo.y;
            if (pick.use_top_amount <= 0.0f) {
                box_hi.y -= height * pick.y_offset;
            } else {
                box_lo.y += (1.0f - pick.use_top_amount) * height;
            }
            if (solid_meets_box(*cursor_solid_, box_lo, box_hi)) {
                t = 0.0f;
            }
        }
        if (!t) {
            continue;
        }
        if (tolerant) {
            const auto at = screen_point(cursor_view_proj_, pos, screen_size_[0], screen_size_[1]);
            if (!at) {
                continue;
            }
            t = std::hypot((*at)[0] - cursor_screen_[0], (*at)[1] - cursor_screen_[1]);
        }
        if (*t < best_t) {
            best_t = *t;
            best_id = id;
        }
    }
    return best_id;
}

u32 InputHandler::target_under(sim::SimState& sim, f32 wx, f32 wz, sim::CommandType type) const {
    const bool reclaim = type == sim::CommandType::Reclaim;
    const u32 unit = unit_under(sim, wx, wz);
    const auto* u =
        unit ? static_cast<const sim::Unit*>(sim.entity_registry().find(unit)) : nullptr;
    if (u && (!reclaim || sim::reclaim_target_valid(*u)) &&
        (type != sim::CommandType::Capture || capture_target(*u))) {
        return unit;
    }
    return reclaim ? prop_under(sim, wx, wz) : 0;
}

u32 InputHandler::prop_under(sim::SimState& sim, f32 wx, f32 wz) const {
    if (camera_zoom_ > 150.0f) {
        return 0;
    }
    u32 best_id = 0;
    f32 best = std::numeric_limits<f32>::max();
    for (u32 id : sim.entity_registry().collect_in_radius(wx, wz, 16.0f)) {
        const sim::Entity* e = sim.entity_registry().find(id);
        if (!e || e->destroyed() || !shown(*e) || !targetable_prop(*e) ||
            !sim::reclaim_target_valid(*e)) {
            continue;
        }
        const sim::Vector3 pos = view_.position(*e);
        const f32 d2 = (pos.x - wx) * (pos.x - wx) + (pos.z - wz) * (pos.z - wz);
        const f32 reach =
            std::max(1.0f, 0.5f * std::max(e->footprint_size_x(), e->footprint_size_z()) + 0.5f);
        if (d2 > reach * reach || d2 >= best) {
            continue;
        }
        best = d2;
        best_id = id;
    }
    return best_id;
}

Sight InputHandler::sight(const sim::Entity& e) const {
    const sim::EntityRecord* record = recon_ ? view_.find(e.entity_id()) : nullptr;
    return record ? recon_->sight(*record) : Sight::Seen;
}

bool InputHandler::shown(const sim::Entity& e) const {
    return shows_icon(sight(e));
}

std::array<f32, 2> InputHandler::footprint_of(const std::string& bp) const {
    // (Not the sim's placement-rules cache: a lookup there would store a
    // default for a blueprint it hasn't read, which can_place would then use.)
    if (mode_hooks_.footprint) return mode_hooks_.footprint(bp);
    return {1.0f, 1.0f};
}

std::vector<TemplateSite> InputHandler::template_sites_for(const CommandMode& mode, f32 x0, f32 z0,
                                                           f32 x1, f32 z1) const {
    // A drag lays copies only when its lead (the mode's structure) is
    // DRAGBUILD, as Moho's ReleaseDrag allows.
    return template_sites(build_template_, x0, z0, x1, z1, mode.drag_build,
                          [&](const std::string& bp) { return footprint_of(bp); });
}

BuildGhost InputHandler::ghost_at(const sim::SimState& sim, f32 wx, f32 wz) const {
    return ghost_at(sim, sim.build_ghost_bp(), sim.build_ghost_foot_x(), sim.build_ghost_foot_z(),
                    wx, wz);
}

BuildGhost InputHandler::ghost_at(const sim::SimState& sim, const std::string& bp, f32 size_x,
                                  f32 size_z, f32 wx, f32 wz) const {
    BuildGhost ghost;
    ghost.blueprint_id = bp;
    ghost.x = wx;
    ghost.y = sim.terrain() ? sim.terrain()->get_terrain_height(wx, wz) : 0.0f;
    ghost.z = wz;
    ghost.cursor_x = wx;
    ghost.cursor_z = wz;
    sim::StructureSite pad = sim::StructureSite::of(wx, wz, size_x, size_z);
    if (mode_hooks_.can_place) {
        ghost.valid = mode_hooks_.can_place(player_army_, bp, wx, wz, 0);
        // can_place has read the blueprint's rules
        const auto& rules = sim.placement_rules(bp, [] { return sim::PlacementRules{}; });
        pad = sim::StructureSite::of(rules, wx, wz);
        ghost.y = sim::structure_elevation(sim, rules, wx, wz);
    } else if (const auto* grid = sim.pathfinding_grid()) {
        // Buildable unless the footprint covers impassable ground
        u32 gx0, gz0, gx1, gz1;
        grid->world_to_grid(wx - size_x * 0.5f, wz - size_z * 0.5f, gx0, gz0);
        grid->world_to_grid(wx + size_x * 0.5f, wz + size_z * 0.5f, gx1, gz1);
        for (u32 gz = gz0; gz <= gz1 && ghost.valid; ++gz) {
            for (u32 gx = gx0; gx <= gx1 && ghost.valid; ++gx) {
                ghost.valid = grid->get(gx, gz) != map::CellPassability::Impassable;
            }
        }
    }
    ghost.pad_x0 = pad.x0;
    ghost.pad_z0 = pad.z0;
    ghost.pad_x1 = pad.x1;
    ghost.pad_z1 = pad.z1;
    return ghost;
}

std::optional<BuildGhost> InputHandler::template_ghost(const sim::SimState& sim,
                                                       const CommandMode& mode, f32 x0, f32 z0,
                                                       f32 x1, f32 z1) const {
    std::optional<BuildGhost> ghost;
    for (const TemplateSite& site : template_sites_for(mode, x0, z0, x1, z1)) {
        if (sim.has_playable_rect() && (site.x < sim.playable_x0() || site.x > sim.playable_x1() ||
                                        site.z < sim.playable_z0() || site.z > sim.playable_z1()))
            continue;
        const auto f = footprint_of(site.blueprint_id);
        BuildGhost g = ghost_at(sim, site.blueprint_id, f[0], f[1], site.x, site.z);
        if (ghost) ghost->line.push_back(std::move(g));
        else ghost = std::move(g);
    }
    return ghost;
}

std::optional<BuildGhost> InputHandler::build_ghost(const Renderer& renderer,
                                                    const sim::SimState& sim) const {
    if (sim.build_ghost_bp().empty() || !sim.terrain()) return std::nullopt;
    const f32 size_x = sim.build_ghost_foot_x();
    const f32 size_z = sim.build_ghost_foot_z();

    // The cursor on the ground, where the range overlays are drawn
    f64 mx = 0, my = 0;
    renderer.mouse_position(mx, my);
    f32 wx = 0, wz = 0;
    const bool on_ground =
        world_at(renderer, sim, static_cast<f32>(mx), static_cast<f32>(my), wx, wz);
    // A build template: a ghost of each of its structures, each green or red
    // on its own, none for one off the playable area (Moho's
    // CBuildDragPreview), and no deposit snap.
    const CommandMode mode = mode_hooks_.current ? mode_hooks_.current() : CommandMode{};
    if (placing_template(mode) && (build_line_ || on_ground)) {
        const auto line = build_line_ ? *build_line_ : std::array<f32, 4>{wx, wz, wx, wz};
        auto ghost = template_ghost(sim, mode, line[0], line[1], line[2], line[3]);
        if (ghost) {
            ghost->cursor_x = on_ground ? wx : ghost->x;
            ghost->cursor_z = on_ground ? wz : ghost->z;
        }
        return ghost;
    }
    if (build_line_ && line_drag_build_) {
        const auto& l = *build_line_;
        const auto sites =
            sim::structure_line_sites(l[0], l[1], l[2], l[3], size_x, size_z, line_spacing_);
        BuildGhost ghost = ghost_at(sim, sites.front().first, sites.front().second);
        for (size_t i = 1; i < sites.size(); ++i) {
            ghost.line.push_back(ghost_at(sim, sites[i].first, sites[i].second));
        }
        ghost.cursor_x = on_ground ? wx : ghost.x;
        ghost.cursor_z = on_ground ? wz : ghost.z;
        return ghost;
    }
    if (!on_ground) return std::nullopt;
    const f32 cursor_x = wx;
    const f32 cursor_z = wz;
    snap_to_deposit(sim, mode_hooks_.current ? mode_hooks_.current() : CommandMode{}, wx, wz);
    // Where a build order at the cursor would place it
    sim::snap_structure_center(wx, wz, size_x, size_z);
    BuildGhost ghost = ghost_at(sim, wx, wz);
    ghost.cursor_x = cursor_x;
    ghost.cursor_z = cursor_z;
    return ghost;
}

} // namespace osc::renderer
