#pragma once

#include <functional>

#include "core/types.hpp"
#include "renderer/build_template.hpp"
#include "sim/build_placement.hpp"
#include "sim/formation.hpp"
#include "sim/sim_callback_queue.hpp"
#include "sim/entity.hpp" // Vector3
#include "sim/unit_command.hpp"
#include "sim/world_snapshot.hpp"

#include <array>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace osc::sim {
class SimState;
class Unit;
}

namespace osc::renderer {

// renderer/command_graph_renderer.hpp has it, but reaches Vulkan (and VMA), which the Lua
// bindings that include this header don't build with
struct CommandGraphNode;

class Camera;
class ReconView;
class Renderer;
struct BuildGhost;

/// FA's command mode (/lua/ui/game/commandmode.lua): what a world click
/// does after the player picked a build icon or an order button.
/// Whether (x, z) is inside the convex quad `q` on the ground (corners in
/// order, either way round)
bool inside_ground_quad(const std::array<sim::Vector3, 4>& q, f32 x, f32 z);

/// A ray: where it starts and its direction, of unit length.
struct PickRay {
    sim::Vector3 origin;
    sim::Vector3 dir;
};

/// How far along `ray` it first meets the box centred at `centre`, turned
/// by `orient`, with half sizes `half` along its own axes; nothing if it
/// misses (or the box is wholly behind it). From inside, 0.
std::optional<f32> ray_box_distance(const PickRay& ray, const sim::Vector3& centre,
                                    const sim::Quaternion& orient, const sim::Vector3& half);

/// Where world point `p` is on a `width` x `height` screen by the camera's
/// `view_proj` (column-major, as Camera::view_proj), in pixels from the top
/// left; nothing behind the camera.
std::optional<std::array<f32, 2>> screen_point(const std::array<f32, 16>& view_proj,
                                               const sim::Vector3& p, f32 width, f32 height);

/// Rejected by Moho's UserUnit::IsSelectable.
bool aboard(const sim::Unit& unit);

/// Whether the player may select `e`: a live unit not made unselectable
/// (SetUnSelectable), not aboard, not still being built, and not
/// INSIGNIFICANTUNIT (the Cybran build bots, which Moho's selection skips)
bool selectable(const sim::Entity& e);

u32 carrier_of(const sim::Entity& e);

std::vector<u32> highest_selection_priority(const std::vector<std::pair<u32, int>>& units);

struct CommandMode {
    std::string mode; ///< "build", "order", "ping"...; empty for none
    std::string name; ///< blueprint id (build) or order cap, e.g. RULEUCC_Attack
    f32 footprint_x = 1.0f; ///< build: the structure's footprint
    f32 footprint_z = 1.0f;
    bool drag_build = false; ///< build: a drag lays a line of it (DRAGBUILD)
    f32 drag_spacing = 0.0f; ///< build: the line's spacing, its skirt
    /// build: Physics.BuildRestriction's deposit, which the cursor snaps to
    sim::PlacementRules::Deposit deposit = sim::PlacementRules::Deposit::None;
    /// An ability's order (RULEUCC_Script, M206w): the Script order's table
    /// for a click at a point -- the mode's own (TaskName, AbilityName) with
    /// the point as Location -- as sim::lua_to_bytes writes it.
    std::function<std::string(const sim::Vector3&)> script_args_at{};
};

/// A command a command-mode click issued, as FA's OnCommandIssued sees it.
struct IssuedCommand {
    std::string type;      ///< "BuildMobile", "Move", "Attack", ...
    sim::Vector3 position; ///< target point (build: the snapped center)
    u32 target_id = 0;     ///< target entity, if any
    std::string blueprint; ///< build: blueprint id
    bool clear = true;     ///< replaced the units' queues (no Shift)
    std::vector<u32> units; ///< the units it was issued to
};

/// The engine side of FA's command mode, provided by the game loop (the
/// renderer layer does not talk to Lua): the current mode, the report of an
/// issued command (commandmode.OnCommandIssued), and a cancel
/// (EndCommandMode) for a right-click.
struct CommandModeHooks {
    std::function<CommandMode()> current;
    std::function<void(const IssuedCommand&)> issued;
    std::function<void()> cancel;
    /// Whether army `army` may build structure `bp` centred at (x, z)
    /// (StructurePlacement's `moving`); none: anywhere
    std::function<bool(i32 army, const std::string& bp, f32 x, f32 z, u32 moving)> can_place;
    /// commandgraph.lua's OnCommandDragBegin() and OnCommandDragEnd(event, cmdId)
    std::function<void()> drag_begin;
    std::function<void(u32 command, f32 mx, f32 my)> drag_end;
    /// A blueprint's footprint, for a build template's structures (none: 1x1)
    FootprintOf footprint;
    /// /lua/formations.lua's AirFormations for `air`, else SurfaceFormations
    std::function<std::vector<std::string>(bool air)> formation_scripts;
    std::function<std::vector<sim::FormationSlot>(const std::vector<sim::FormationMember>&,
                                                  const std::string& script, const sim::Vector3& at,
                                                  f32 facing)>
        formation_slots;
};

/// Moho's CFormation: the right button's drag formation
struct FormationDrag {
    static constexpr f32 kWait = 0.5f;
    sim::Vector3 at;
    sim::Vector3 mouse;
    f32 facing = 0;
    f32 wait = kWait;
    std::vector<u32> units;
    std::string script;
    bool settled() const { return wait <= 0; }
};

/// A unit's place in a settled drag formation (CWldSession::RenderMeshPreviews)
struct FormationGhost {
    std::string blueprint_id;
    sim::Vector3 position;
    f32 heading = 0;
};

/// A command graph waypoint dragged (Moho's UICommandDragger): where it is
/// drawn, and whether it may be dropped there; after the drop, until the
/// view shows the order moved
struct OrderDrag {
    u32 command_id = 0;
    sim::Vector3 at;
    bool valid = true;
    bool held = true;
};

/// Handles player input on the world: unit selection, command dispatch and
/// the drag selection box. The keys are FA's key map's (control groups
/// included).
class InputHandler {
public:
    /// A double-click's second click comes this soon after the first, and
    /// this near on screen (Windows' GetDoubleClickTime and SM_CXDOUBLECLK).
    static constexpr f64 kDoubleClickSeconds = 0.5;
    static constexpr f32 kDoubleClickPixels = 2.0f;

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
    /// The session's active build template (Moho's CWldSession one), empty
    /// when none: GenerateBuildTemplateFromSelection, Set/Get and
    /// ClearBuildTemplates keep it.
    BuildTemplate& build_template() { return build_template_; }

    /// The structure being placed (the sim's build ghost), at the snapped
    /// spot under the cursor with whether it can be built there, or
    /// nothing when no structure is being placed.
    std::optional<BuildGhost> build_ghost(const Renderer& renderer,
                                          const sim::SimState& sim) const;

    /// The active build template's ghosts for a build drag from (x0, z0) to
    /// (x1, z1) (a click: both the same): the first, the rest in its `line`,
    /// each green or red on its own, none for a structure off the playable
    /// area (Moho's CBuildDragPreview); nothing when none is on it.
    std::optional<BuildGhost> template_ghost(const sim::SimState& sim, const CommandMode& mode,
                                             f32 x0, f32 z0, f32 x1, f32 z1) const;

    /// The structure being placed, centred at (wx, wz)
    BuildGhost ghost_at(const sim::SimState& sim, f32 wx, f32 wz) const;
    /// Structure `bp` (footprint `size_x` x `size_z`) centred at (wx, wz)
    BuildGhost ghost_at(const sim::SimState& sim, const std::string& bp, f32 size_x, f32 size_z,
                        f32 wx, f32 wz) const;

    /// Where this frame draws the world: clicks pick the unit the player
    /// sees under the cursor, not its position at the last tick. Without
    /// one (headless clicks) the live sim is used.
    void set_frame_view(const sim::FrameView& view) { view_ = view; }

    /// The player's intel: a click can't target a unit it doesn't show (a
    /// blip or a remembered structure it can; null: everything; M215b).
    void set_recon(const ReconView* recon) { recon_ = recon; }

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

    /// CUIWorldView's right button: the orders picked at the press, issued at the release
    void right_press(sim::SimState& sim, f32 wx, f32 wz, bool shift);
    void right_drag(std::optional<std::array<f32, 2>> cursor, f64 dt);
    std::vector<IssuedCommand> right_release(sim::SimState& sim);
    /// CFormation::LuaFinalize
    bool cycle_formation();
    const std::optional<FormationDrag>& formation_drag() const { return formation_; }
    const std::vector<FormationGhost>& formation_ghosts(const sim::SimState& sim);

    /// A build mode's press from (x0, z0) released at (x1, z1), as Moho lays
    /// a build drag: the structure along the line (at the press alone unless
    /// it is DRAGBUILD), each site it may stand on a build order for the
    /// selection's mobile builders; the first clears their queues unless
    /// `shift`, the rest queue. Reported, only the last clears.
    /// With a build template active, each of its structures instead, copy by
    /// copy (template_sites); one that can't stand where it falls is left
    /// out, the rest still ordered (Moho's IssueBuildDragOrders).
    std::vector<IssuedCommand> build_line(sim::SimState& sim, const CommandMode& mode, f32 x0,
                                          f32 z0, f32 x1, f32 z1, bool shift);

    /// Whether a build mode places the active build template rather than its
    /// one structure.
    bool placing_template(const CommandMode& mode) const {
        return mode.mode == "build" && !build_template_.entries.empty();
    }

    /// The build line being dragged ({x0, z0, x1, z1}), while the button is held
    std::optional<std::array<f32, 4>> build_line_drag() const { return build_line_; }

    /// The orders a right-click at (wx, wz) would give the selection, each
    /// with its units, unissued
    std::vector<std::pair<sim::UnitCommand, std::vector<u32>>>
    right_click_orders(sim::SimState& sim, f32 wx, f32 wz) const;

    /// What the right button would order at (wx, wz), as the world view's
    /// GetRightMouseButtonOrder asks at the cursor: the first order not a
    /// move, else a move; none without a selection
    std::optional<sim::CommandType> right_button_order(sim::SimState& sim, f32 wx, f32 wz) const;

    /// Where the cursor points on the ground, while over the world
    std::optional<std::array<f32, 2>> cursor_world() const { return cursor_world_; }

    void left_click_at(sim::SimState& sim, f32 wx, f32 wz, bool shift);
    /// A left click on the world at (wx, wz) as the release of a click
    /// ends it: a double-click's second selects like units in view, any
    /// other selects (or with Shift toggles) the unit there.
    void world_click(sim::SimState& sim, f32 wx, f32 wz, bool shift, bool double_click,
                     const std::array<f32, 16>& view_proj);
    /// A double-click on the player's unit at (wx, wz): every unit of its
    /// blueprint `view_proj` shows joins the selection (Moho's
    /// CWldSession::HandleDoubleClickSelection). Not on a wall.
    void select_similar_in_view(sim::SimState& sim, f32 wx, f32 wz,
                                const std::array<f32, 16>& view_proj);

    /// Moho's UserUnit::UpdateUnitData: a unit that boards leaves the selection.
    void deselect_aboard(const sim::EntityRegistry& registry);

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

    /// The drag box's corners on the ground, while dragging
    std::optional<std::array<sim::Vector3, 4>> drag_box() const {
        return dragging_ ? std::optional(drag_quad_) : std::nullopt;
    }

    /// The unit under the cursor, of any army (0: none)
    u32 hovered() const { return hovered_; }

    /// The order whose command graph waypoint is under the cursor, while
    /// Shift shows the graph (0: none)
    u32 hovered_command() const { return hovered_command_; }

    std::vector<CommandGraphNode> command_graph_nodes() const;

    bool begin_order_drag(u32 command_id);
    void drag_order_to(sim::SimState& sim, f32 wx, f32 wz);
    /// UICommandDragger::DragRelease: one __osc_SetCommandTarget for the
    /// order, as ProcessCommandDrag resolves the drop; nothing for a drag
    /// that never moved or a build that can't stand there
    std::optional<sim::SimCallbackEntry> release_order_drag(sim::SimState& sim, f32 wx, f32 wz);
    std::optional<OrderDrag> order_drag() const;

    /// Whether a right button over waypoint `hovered` takes its order off
    /// (CUIWorldView::HandleEvent: Shift+Ctrl, not an observer)
    bool removes_order(u32 hovered, bool shift, bool ctrl) const {
        return hovered != 0 && shift && ctrl && player_army_ >= 0;
    }
    /// __osc_RemoveCommand for every unit of order `command_id`
    std::optional<sim::SimCallbackEntry> remove_order(sim::SimState& sim, u32 command_id);

    /// CanRestartMoveCommandAsPatrol: the moves from waypoint `hovered` on
    /// that a click makes a patrol; none if the selection's queues can't
    std::vector<u32> moves_to_patrol(sim::SimState& sim, u32 hovered) const;
    /// RestartMoveCommandAsPatrol: an __osc_SetCommandType for each of them
    std::vector<sim::SimCallbackEntry> restart_as_patrol(sim::SimState& sim, u32 hovered);
    /// Whether the waypoint under the cursor would start a patrol
    /// (CUIWorldView's ShowConvertToPatrolCursor)
    bool converts_to_patrol() const { return converts_to_patrol_; }

    /// The shown unit the cursor is on, as it is drawn: the nearest whose box
    /// (its blueprint's size, turned with it, standing on where it is drawn)
    /// the cursor's ray meets -- an aircraft where it flies, not the ground
    /// under it. For a world point the cursor isn't on (a script's click),
    /// the ray comes straight down onto (wx, wz). 0 for none.
    u32 unit_under(sim::SimState& sim, f32 wx, f32 wz, bool own_only = false) const;
    /// The cursor's ray and the world point it is on, as update() takes them
    /// each frame (tests set it).
    void set_cursor_ray(const PickRay& ray, f32 wx, f32 wz) {
        cursor_ray_ = ray;
        cursor_ray_ground_ = {wx, wz};
    }
    /// Select the player's units drawn inside the screen box (x0, y0)-(x1,
    /// y1) by the camera's `view_proj` on a `width` x `height` screen: those
    /// of the highest selection priority there, or with `shift` all of them
    /// added to the selection.
    void select_in_box(sim::SimState& sim, const std::array<f32, 16>& view_proj, f32 width,
                       f32 height, f32 x0, f32 y0, f32 x1, f32 y1, bool shift);

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

    /// Where the cursor at (mx, my) points on the ground: the terrain, or
    /// the water over it (M217a). Every click and drag resolves through it.
    void snap_to_deposit(const sim::SimState& sim, const CommandMode& mode, f32& x, f32& z) const;
    /// Blueprint `bp`'s footprint for a template's structure: the hook's,
    /// else 1x1
    std::array<f32, 2> footprint_of(const std::string& bp) const;
    /// The active template's structures for a build drag (template_sites)
    std::vector<TemplateSite> template_sites_for(const CommandMode& mode, f32 x0, f32 z0, f32 x1,
                                                 f32 z1) const;
    void measure_snap_radius(const Renderer& renderer, const sim::SimState& sim, f32 mx, f32 my);
    static bool world_at(const Renderer& renderer, const sim::SimState& sim, f32 mx, f32 my,
                         f32& wx, f32& wz);

private:
    i32 player_army_ = 0;
    sim::FrameView view_;
    const ReconView* recon_ = nullptr;
    /// Whether the player's intel shows `e` (anything, without a view).
    bool shown(const sim::Entity& e) const;
    std::unordered_set<u32> selected_;
    BuildTemplate build_template_;
    bool selection_event_ = false;

    // Left mouse state (selection)
    bool lmb_was_pressed_ = false;
    /// The input's clock (the frames' time) and its last world click, which
    /// a second within kDoubleClickSeconds and kDoubleClickPixels makes a
    /// double-click (Windows' defaults, which wx gave Moho).
    f64 clock_ = 0.0;
    f64 last_click_time_ = -1.0;
    f32 last_click_x_ = 0.0f;
    f32 last_click_y_ = 0.0f;
    bool last_click_double_ = false;
    bool lmb_on_ui_ = false;     // current left press began over the UI
    bool lmb_raw_prev_ = false;  // left button last frame, whoever owned it
    bool lmb_on_minimap_ = false; // current left press began on the minimap
    bool dragging_ = false;
    /// This frame's cursor ray, and the world point under it it was made for
    std::optional<PickRay> cursor_ray_;
    std::array<f32, 2> cursor_ray_ground_{};
    f32 drag_start_x_ = 0, drag_start_y_ = 0;
    f32 drag_end_x_ = 0, drag_end_y_ = 0;
    f32 drag_world_x0_ = 0, drag_world_z0_ = 0;
    f32 drag_world_x1_ = 0, drag_world_z1_ = 0;
    std::array<sim::Vector3, 4> drag_quad_{};
    u32 hovered_ = 0;
    u32 hovered_command_ = 0;
    bool converts_to_patrol_ = false;
    std::optional<OrderDrag> order_drag_;
    bool order_drag_moved_ = false;
    std::optional<OrderDrag> dropped_;
    sim::Vector3 dropped_from_;
    f64 dropped_for_ = 0;
    bool rmb_removes_ = false;
    std::optional<CommandGraphNode> graph_node(u32 command_id) const;
    std::vector<u32> own_units(sim::SimState& sim, const std::vector<u32>& ids) const;
    OrderDrag order_drop(sim::SimState& sim, const CommandGraphNode& node, f32 wx, f32 wz,
                         bool released, u32& target) const;
    static constexpr f32 DRAG_THRESHOLD = 5.0f; // pixels before drag starts
    std::optional<std::array<f32, 4>> build_line_;
    f32 snap_radius_ = 4.0f;
    bool line_drag_build_ = false;
    f32 line_spacing_ = 0.0f;
    std::optional<std::array<f32, 2>> cursor_world_;

    // Right mouse state (commands)
    bool rmb_was_pressed_ = false;
    bool rmb_on_ui_ = false;     // current right press began over the UI
    bool rmb_raw_prev_ = false;
    struct PendingRight {
        std::vector<std::pair<sim::UnitCommand, std::vector<u32>>> orders;
        bool shift = false;
    };
    std::optional<PendingRight> pending_right_;
    std::optional<FormationDrag> formation_;
    size_t formation_index_ = 0;
    std::vector<std::string> formation_scripts_;
    std::vector<FormationGhost> formation_ghosts_;
    std::optional<std::pair<std::string, f32>> formation_ghosts_for_;
    std::vector<IssuedCommand>
    issue_right_orders(sim::SimState& sim,
                       const std::vector<std::pair<sim::UnitCommand, std::vector<u32>>>& orders,
                       bool shift, const FormationDrag* formation);

    void handle_drag_select(Renderer& renderer, sim::SimState& sim);

    /// The live unit of any army nearest (wx, wz) within `radius`, or 0.
    /// With `reclaim`, the nearest thing a Reclaim order takes: a unit or a
    /// prop (tree, rock, wreck) that is reclaimable.
    u32 pick_any_unit(sim::SimState& sim, f32 wx, f32 wz, f32 radius,
                      bool reclaim = false) const;
    CommandModeHooks mode_hooks_;
};

} // namespace osc::renderer
