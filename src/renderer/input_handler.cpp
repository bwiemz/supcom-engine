#include "renderer/input_handler.hpp"
#include "sim/build_placement.hpp"
#include "renderer/recon_view.hpp"
#include "renderer/renderer.hpp"

#include "sim/army_brain.hpp"
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

} // namespace

bool selectable(const sim::Entity& e) {
    if (!e.is_unit() || e.destroyed() || e.unselectable()) {
        return false;
    }
    const auto& unit = static_cast<const sim::Unit&>(e);
    return !unit.is_being_built() && !unit.has_category("INSIGNIFICANTUNIT");
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

void InputHandler::update(Renderer& renderer, sim::SimState& sim,
                          f64 /*dt*/, const std::function<bool()>& mouse_over_ui) {
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
    hovered_ = 0;
    if (!on_minimap && !(mouse_over_ui && mouse_over_ui())) {
        f32 wx = 0;
        f32 wz = 0;
        if (world_at(renderer, sim, mx, my, wx, wz)) {
            cursor_world_ = std::array<f32, 2>{wx, wz};
            if (!dragging_) {
                hovered_ = unit_under(sim, wx, wz);
            }
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
    }

    if (lmb && build_line_) {
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
        if (build_line_) {
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
        } else {
            handle_right_click(renderer, sim, mx, my);
        }
    }
    rmb_was_pressed_ = rmb;
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

void InputHandler::handle_left_click(Renderer& renderer,
                                     sim::SimState& sim,
                                     f32 mx, f32 my) {
    f32 wx, wz;
    if (!world_at(renderer, sim, mx, my, wx, wz)) return;

    const bool shift = renderer.is_key_pressed(GLFW_KEY_LEFT_SHIFT) ||
                       renderer.is_key_pressed(GLFW_KEY_RIGHT_SHIFT);
    left_click_at(sim, wx, wz, shift);
}

void InputHandler::left_click_at(sim::SimState& sim, f32 wx, f32 wz, bool shift) {
    const u32 picked = unit_under(sim, wx, wz, true);

    if (!shift)
        selected_.clear();

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

void InputHandler::handle_drag_select(Renderer& renderer,
                                      sim::SimState& sim) {
    f32 wx0 = drag_quad_[0].x;
    f32 wx1 = wx0;
    f32 wz0 = drag_quad_[0].z;
    f32 wz1 = wz0;
    for (const sim::Vector3& p : drag_quad_) {
        wx0 = std::min(wx0, p.x);
        wx1 = std::max(wx1, p.x);
        wz0 = std::min(wz0, p.z);
        wz1 = std::max(wz1, p.z);
    }

    bool shift = renderer.is_key_pressed(GLFW_KEY_LEFT_SHIFT) ||
                 renderer.is_key_pressed(GLFW_KEY_RIGHT_SHIFT);
    if (!shift)
        selected_.clear();

    std::vector<std::pair<u32, int>> boxed;
    for (u32 id : sim.entity_registry().collect_in_rect(wx0, wz0, wx1, wz1)) {
        auto* e = sim.entity_registry().find(id);
        if (!e || !selectable(*e)) continue;
        if (e->army() != player_army_) continue;
        const sim::Vector3 pos = view_.position(*e);
        if (inside_ground_quad(drag_quad_, pos.x, pos.z)) {
            boxed.emplace_back(id, static_cast<const sim::Unit*>(e)->selection_priority());
        }
    }
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
    spdlog::debug("Drag select: {} units in ({:.0f},{:.0f})-({:.0f},{:.0f})", selected_.size(), wx0,
                  wz0, wx1, wz1);
}

void InputHandler::handle_right_click(Renderer& renderer,
                                      sim::SimState& sim,
                                      f32 mx, f32 my) {
    if (selected_.empty()) return;

    f32 wx, wz;
    if (!world_at(renderer, sim, mx, my, wx, wz)) return;

    // Check if Shift is held (queue commands without clearing)
    const bool shift = renderer.is_key_pressed(GLFW_KEY_LEFT_SHIFT) ||
                       renderer.is_key_pressed(GLFW_KEY_RIGHT_SHIFT);
    const auto issued = right_click_at(sim, wx, wz, shift);
    spdlog::debug("Right-click: {} order(s) for {} units at ({:.0f},{:.0f})", issued.size(),
                  selected_.size(), wx, wz);
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
    // What the click is on: an enemy within 5 (the one drawn nearest), else
    // an ally or a wreck the click falls on (within its footprint).
    enum class On : u8 { Ground, Enemy, Ally, Wreck };
    On on = On::Ground;
    u32 target = 0;
    {
        f32 best = 25.0f;
        for (u32 id : registry.collect_in_radius(wx, wz, 5.0f)) {
            const sim::Entity* e = live(id);
            if (!e || !e->is_unit() || e->army() < 0 || allied(e->army()) || !shown(*e)) continue;
            const sim::Vector3 pos = view_.position(*e);
            const f32 d2 = (pos.x - wx) * (pos.x - wx) + (pos.z - wz) * (pos.z - wz);
            if (d2 < best) {
                best = d2;
                target = id;
                on = On::Enemy;
            }
        }
    }
    if (on == On::Ground) {
        f32 best = std::numeric_limits<f32>::max();
        for (u32 id : registry.collect_in_radius(wx, wz, 16.0f)) {
            const sim::Entity* e = live(id);
            if (!e) continue;
            const bool ally = e->is_unit() && allied(e->army());
            const bool wreck = targetable_prop(*e) && e->reclaimable();
            if (!ally && !wreck) continue;
            const sim::Vector3 pos = view_.position(*e);
            const f32 d2 = (pos.x - wx) * (pos.x - wx) + (pos.z - wz) * (pos.z - wz);
            const f32 reach = std::max(
                1.0f, 0.5f * std::max(e->footprint_size_x(), e->footprint_size_z()) + 0.5f);
            if (d2 > reach * reach || d2 >= best) continue;
            best = d2;
            target = id;
            on = ally ? On::Ally : On::Wreck;
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
            if (u.has_command_cap("RULEUCC_Attack")) aim(sim::CommandType::Attack);
            else if (u.has_command_cap("RULEUCC_Capture")) aim(sim::CommandType::Capture);
        } else if (on == On::Ally && tu) {
            if (tu->has_category("AIRSTAGINGPLATFORM") && u.has_command_cap("RULEUCC_Dock") &&
                u.is_air_unit())
                aim(sim::CommandType::Dock);
            else if (tu->has_category("TRANSPORTATION") &&
                     u.has_command_cap("RULEUCC_CallTransport") && !u.is_air_unit())
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
    std::vector<IssuedCommand> issued;
    for (const auto& [cmd, units] : right_click_orders(sim, wx, wz)) {
        // Player-issued: routed so it applies inside a tick (and a networked
        // match broadcasts it); a move goes to factories as their rally point.
        sim.set_human_input_active(true);
        sim.route_player_command(units, cmd, !shift);
        sim.set_human_input_active(false);
        IssuedCommand out;
        out.position = cmd.target_pos;
        out.target_id = cmd.target_id;
        out.clear = !shift;
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
        if (mode_hooks_.can_place && !mode_hooks_.can_place(player_army_, mode.name, wx, wz)) {
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
        cmd.target_id = pick_any_unit(sim, wx, wz, 5.0f,
                                      spec->type == sim::CommandType::Reclaim);
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
    return out;
}

std::vector<IssuedCommand> InputHandler::build_line(sim::SimState& sim, const CommandMode& mode,
                                                    f32 x0, f32 z0, f32 x1, f32 z1, bool shift) {
    std::vector<IssuedCommand> issued;
    if (mode.mode != "build" || mode.name.empty()) {
        return issued;
    }
    snap_to_deposit(sim, mode, x0, z0);
    snap_to_deposit(sim, mode, x1, z1);
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
    if (!mode.drag_build) {
        x1 = x0;
        z1 = z0;
    }
    for (const auto& [x, z] : sim::structure_line_sites(x0, z0, x1, z1, mode.footprint_x,
                                                        mode.footprint_z, mode.drag_spacing)) {
        if (mode_hooks_.can_place && !mode_hooks_.can_place(player_army_, mode.name, x, z)) {
            continue;
        }
        const bool clear = issued.empty() && !shift;
        sim::UnitCommand cmd;
        cmd.type = sim::CommandType::BuildMobile;
        cmd.blueprint_id = mode.name;
        cmd.target_pos = {x, sim.terrain() ? sim.terrain()->get_surface_height(x, z) : 0.0f, z};
        // Player-issued order: routed so a networked match broadcasts it.
        sim.set_human_input_active(true);
        sim.route_player_command(ids, cmd, clear);
        sim.set_human_input_active(false);
        IssuedCommand out;
        out.type = "BuildMobile";
        out.blueprint = mode.name;
        out.position = cmd.target_pos;
        out.clear = false;
        issued.push_back(out);
    }
    // The last one tells commandmode.lua the line is done, ending the mode
    // without Shift
    if (!issued.empty()) {
        issued.back().clear = !shift;
    }
    return issued;
}

u32 InputHandler::unit_under(sim::SimState& sim, f32 wx, f32 wz, bool own_only) const {
    u32 best_id = 0;
    f32 best_d2 = std::numeric_limits<f32>::max();
    for (u32 id : sim.entity_registry().collect_in_radius(wx, wz, 16.0f)) {
        const auto* e = sim.entity_registry().find(id);
        if (!e || e->destroyed() || !e->is_unit() || !shown(*e)) {
            continue;
        }
        if (own_only && (e->army() != player_army_ || !selectable(*e))) {
            continue;
        }
        const auto& unit = static_cast<const sim::Unit&>(*e);
        const sim::Vector3 pos = view_.position(*e);
        const sim::Vector3 right = sim::quat_rotate(view_.orientation(*e), {1, 0, 0});
        const f32 len = std::sqrt(right.x * right.x + right.z * right.z);
        const f32 rx = len > 1e-4f ? right.x / len : 1.0f;
        const f32 rz = len > 1e-4f ? right.z / len : 0.0f;
        const f32 dx = wx - pos.x;
        const f32 dz = wz - pos.z;
        const f32 along_x = dx * rx + dz * rz;
        const f32 along_z = -dx * rz + dz * rx;
        if (std::abs(along_x) > std::max(unit.size_x(), 0.5f) * 0.5f ||
            std::abs(along_z) > std::max(unit.size_z(), 0.5f) * 0.5f) {
            continue;
        }
        const f32 d2 = dx * dx + dz * dz;
        if (d2 < best_d2) {
            best_d2 = d2;
            best_id = id;
        }
    }
    return best_id;
}

u32 InputHandler::pick_any_unit(sim::SimState& sim, f32 wx, f32 wz,
                                f32 radius, bool reclaim) const {
    u32 best_id = 0;
    f32 best_dist2 = radius * radius;
    for (u32 id : sim.entity_registry().collect_in_radius(wx, wz, radius)) {
        auto* e = sim.entity_registry().find(id);
        if (!e || e->destroyed() || !shown(*e)) continue;
        if (reclaim ? !((e->is_unit() || targetable_prop(*e)) && e->reclaimable()) : !e->is_unit())
            continue;
        const sim::Vector3 pos = view_.position(*e);
        const f32 dx = pos.x - wx;
        const f32 dz = pos.z - wz;
        const f32 d2 = dx * dx + dz * dz;
        if (d2 <= best_dist2) {
            best_dist2 = d2;
            best_id = id;
        }
    }
    return best_id;
}

bool InputHandler::shown(const sim::Entity& e) const {
    if (!recon_) return true;
    const sim::EntityRecord* record = view_.find(e.entity_id());
    return !record || shows_icon(recon_->sight(*record));
}

BuildGhost InputHandler::ghost_at(const sim::SimState& sim, f32 wx, f32 wz) const {
    const auto& bp = sim.build_ghost_bp();
    const f32 size_x = sim.build_ghost_foot_x();
    const f32 size_z = sim.build_ghost_foot_z();
    BuildGhost ghost;
    ghost.blueprint_id = bp;
    ghost.x = wx;
    ghost.y = sim.terrain() ? sim.terrain()->get_terrain_height(wx, wz) : 0.0f;
    ghost.z = wz;
    sim::StructureSite pad = sim::StructureSite::of(wx, wz, size_x, size_z);
    if (mode_hooks_.can_place) {
        ghost.valid = mode_hooks_.can_place(player_army_, bp, wx, wz);
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

std::optional<BuildGhost> InputHandler::build_ghost(const Renderer& renderer,
                                                    const sim::SimState& sim) const {
    if (sim.build_ghost_bp().empty() || !sim.terrain()) return std::nullopt;
    const f32 size_x = sim.build_ghost_foot_x();
    const f32 size_z = sim.build_ghost_foot_z();

    if (build_line_ && line_drag_build_) {
        const auto& l = *build_line_;
        const auto sites =
            sim::structure_line_sites(l[0], l[1], l[2], l[3], size_x, size_z, line_spacing_);
        BuildGhost ghost = ghost_at(sim, sites.front().first, sites.front().second);
        for (size_t i = 1; i < sites.size(); ++i) {
            ghost.line.push_back(ghost_at(sim, sites[i].first, sites[i].second));
        }
        return ghost;
    }
    f64 mx = 0, my = 0;
    renderer.mouse_position(mx, my);
    f32 wx = 0, wz = 0;
    if (!world_at(renderer, sim, static_cast<f32>(mx), static_cast<f32>(my), wx, wz)) {
        return std::nullopt;
    }
    snap_to_deposit(sim, mode_hooks_.current ? mode_hooks_.current() : CommandMode{}, wx, wz);
    // Where a build order at the cursor would place it
    sim::snap_structure_center(wx, wz, size_x, size_z);
    return ghost_at(sim, wx, wz);
}

} // namespace osc::renderer
