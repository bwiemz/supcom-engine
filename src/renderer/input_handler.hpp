#pragma once

#include "core/types.hpp"
#include "sim/entity.hpp" // Vector3

#include <array>
#include <unordered_set>
#include <vector>

namespace osc::sim {
class SimState;
}

namespace osc::renderer {

class Camera;
class Renderer;

/// Handles player input: unit selection, command dispatch, drag selection box,
/// control groups (Ctrl+0-9), and camera bookmarks (Ctrl+Shift+0-9).
class InputHandler {
public:
    /// Set which army the player controls (0-based).
    void set_player_army(i32 army) { player_army_ = army; }
    i32 player_army() const { return player_army_; }

    /// Process input each frame. Call after poll_events.
    void update(Renderer& renderer, sim::SimState& sim, f64 dt);

    /// Currently selected unit IDs.
    const std::unordered_set<u32>& selected() const { return selected_; }

    /// Whether a drag-selection box is active.
    bool is_dragging() const { return dragging_; }

    /// Drag box corners in screen pixels (valid when is_dragging).
    void drag_rect(f32& x0, f32& y0, f32& x1, f32& y1) const {
        x0 = drag_start_x_; y0 = drag_start_y_;
        x1 = drag_end_x_; y1 = drag_end_y_;
    }

    /// Drag box corners in world XZ (for rendering).
    void drag_world_rect(f32& x0, f32& z0, f32& x1, f32& z1) const {
        x0 = drag_world_x0_; z0 = drag_world_z0_;
        x1 = drag_world_x1_; z1 = drag_world_z1_;
    }

    static constexpr u32 NUM_GROUPS = 10; // 0-9

private:
    i32 player_army_ = 0;
    std::unordered_set<u32> selected_;

    // Left mouse state (selection)
    bool lmb_was_pressed_ = false;
    bool dragging_ = false;
    f32 drag_start_x_ = 0, drag_start_y_ = 0;
    f32 drag_end_x_ = 0, drag_end_y_ = 0;
    f32 drag_world_x0_ = 0, drag_world_z0_ = 0;
    f32 drag_world_x1_ = 0, drag_world_z1_ = 0;
    static constexpr f32 DRAG_THRESHOLD = 5.0f; // pixels before drag starts

    // Right mouse state (commands)
    bool rmb_was_pressed_ = false;

    // Control groups (Ctrl+0-9 to assign, 0-9 to recall)
    std::array<std::unordered_set<u32>, NUM_GROUPS> control_groups_;
    std::array<bool, NUM_GROUPS> number_was_pressed_{};

    // Camera bookmarks (Ctrl+Shift+0-9 to save, Shift+0-9 to recall)
    struct CameraBookmark {
        f32 x = 0, z = 0;
        bool valid = false;
    };
    std::array<CameraBookmark, NUM_GROUPS> camera_bookmarks_{};

    void handle_left_click(Renderer& renderer, sim::SimState& sim,
                           f32 mx, f32 my);
    void handle_drag_select(Renderer& renderer, sim::SimState& sim);
    void handle_right_click(Renderer& renderer, sim::SimState& sim,
                            f32 mx, f32 my);
    void handle_groups_and_bookmarks(Renderer& renderer,
                                     sim::SimState& sim);

    /// Find the nearest player-owned unit to a world XZ point within radius.
    u32 pick_unit(sim::SimState& sim, f32 wx, f32 wz, f32 radius) const;
};

} // namespace osc::renderer
