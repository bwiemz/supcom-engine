#pragma once

#include <functional>

#include "core/types.hpp"
#include "sim/entity.hpp" // Vector3
#include "sim/world_snapshot.hpp"

#include <array>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace osc::sim {
class SimState;
}

namespace osc::renderer {

class Camera;
class Renderer;
struct BuildGhost;

/// FA's command mode (/lua/ui/game/commandmode.lua): what a world click
/// does after the player picked a build icon or an order button.
struct CommandMode {
    std::string mode; ///< "build", "order", "ping"...; empty for none
    std::string name; ///< blueprint id (build) or order cap, e.g. RULEUCC_Attack
    f32 footprint_x = 1.0f; ///< build: the structure's footprint
    f32 footprint_z = 1.0f;
};

/// A command a command-mode click issued, as FA's OnCommandIssued sees it.
struct IssuedCommand {
    std::string type;      ///< "BuildMobile", "Move", "Attack", ...
    sim::Vector3 position; ///< target point (build: the snapped center)
    u32 target_id = 0;     ///< target entity, if any
    std::string blueprint; ///< build: blueprint id
    bool clear = true;     ///< replaced the units' queues (no Shift)
};

/// The engine side of FA's command mode, provided by the game loop (the
/// renderer layer does not talk to Lua): the current mode, the report of an
/// issued command (commandmode.OnCommandIssued), and a cancel
/// (EndCommandMode) for a right-click.
struct CommandModeHooks {
    std::function<CommandMode()> current;
    std::function<void(const IssuedCommand&)> issued;
    std::function<void()> cancel;
};

/// Handles player input: unit selection, command dispatch, drag selection box,
/// control groups (Ctrl+0-9), and camera bookmarks (Ctrl+Shift+0-9).
class InputHandler {
public:
    /// Set which army the player controls (0-based).
    void set_player_army(i32 army) { player_army_ = army; }
    i32 player_army() const { return player_army_; }

    /// Process input each frame. Call after poll_events.
    /// `mouse_over_ui` says whether the cursor is over FA's UI rather than
    /// the world (a WorldView or nothing); it is asked only when a button
    /// goes down. A press that starts over the UI is the UI's and never
    /// selects, drags or orders in the world.
    void update(Renderer& renderer, sim::SimState& sim, f64 dt,
                const std::function<bool()>& mouse_over_ui = {});

    /// Currently selected unit IDs.
    const std::unordered_set<u32>& selected() const { return selected_; }

    /// The structure being placed (the sim's build ghost), at the snapped
    /// spot under the cursor with whether it can be built there, or
    /// nothing when no structure is being placed.
    std::optional<BuildGhost> build_ghost(const Renderer& renderer,
                                          const sim::SimState& sim) const;

    /// Where this frame draws the world: clicks pick the unit the player
    /// sees under the cursor, not its position at the last tick. Without
    /// one (headless clicks) the live sim is used.
    void set_frame_view(const sim::FrameView& view) { view_ = view; }

    void set_command_mode_hooks(CommandModeHooks hooks) { mode_hooks_ = std::move(hooks); }

    /// A left-click at world (wx, wz) under command mode `mode`: route its
    /// order to the selected units. Build places `mode.name` at the snapped
    /// point (builders only); an order issues that cap, at the unit under the
    /// click when the order targets one. Nothing when the mode has no world
    /// click (none, ping) or nothing takes the order.
    std::optional<IssuedCommand> click_in_command_mode(sim::SimState& sim,
                                                       const CommandMode& mode,
                                                       f32 wx, f32 wz, bool shift);

    /// A plain right-click at world (wx, wz): each selected unit's default
    /// order there, as FA gives it. On an enemy: Attack (Capture for a unit
    /// that can capture but not attack). On an ally: Assist (Guard); Repair
    /// for an engineer on one under construction; a load for a unit it can
    /// carry; a Dock for an aircraft at a staging platform. On a wreck:
    /// Reclaim for an engineer. Units that can't take the order, and every
    /// unit on open ground, move there. One order per kind, routed to its
    /// units; what was issued comes back (headless clicks and tests).
    std::vector<IssuedCommand> right_click_at(sim::SimState& sim, f32 wx, f32 wz, bool shift);

    /// Replace the current selection (called from Lua SelectUnits).
    void set_selected(const std::unordered_set<u32>& sel) {
        selected_ = sel;
        selection_event_ = true;
    }

    /// Whether a selection action happened since the last call (and reset).
    /// Moho reports every selection action to the UI, unchanged or not:
    /// retail's OnSelectionChanged checks for an unchanged selection itself.
    bool take_selection_event() {
        const bool e = selection_event_;
        selection_event_ = false;
        return e;
    }

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
    sim::FrameView view_;
    std::unordered_set<u32> selected_;
    bool selection_event_ = false;

    // Left mouse state (selection)
    bool lmb_was_pressed_ = false;
    bool lmb_on_ui_ = false;     // current left press began over the UI
    bool lmb_raw_prev_ = false;  // left button last frame, whoever owned it
    bool lmb_on_minimap_ = false; // current left press began on the minimap
    bool dragging_ = false;
    f32 drag_start_x_ = 0, drag_start_y_ = 0;
    f32 drag_end_x_ = 0, drag_end_y_ = 0;
    f32 drag_world_x0_ = 0, drag_world_z0_ = 0;
    f32 drag_world_x1_ = 0, drag_world_z1_ = 0;
    static constexpr f32 DRAG_THRESHOLD = 5.0f; // pixels before drag starts

    // Right mouse state (commands)
    bool rmb_was_pressed_ = false;
    bool rmb_on_ui_ = false;     // current right press began over the UI
    bool rmb_raw_prev_ = false;

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
    /// The live unit of any army nearest (wx, wz) within `radius`, or 0.
    /// With `reclaim`, the nearest thing a Reclaim order takes: a unit or a
    /// prop (tree, rock, wreck) that is reclaimable.
    u32 pick_any_unit(sim::SimState& sim, f32 wx, f32 wz, f32 radius,
                      bool reclaim = false) const;
    CommandModeHooks mode_hooks_;
};

} // namespace osc::renderer
