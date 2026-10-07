#pragma once

// Moho's CAiPathNavigator (roadmap item 4c-2b, faf-re CAiPathNavigator.cpp):
// a ground unit's path, asked of its army's path queue and followed cell by
// cell. Each tick it notes the unit's cell, drops the cells behind it, and
// picks the farthest cell ahead its footprint can slide straight to (Moho's
// string-pulling: the path itself is never smoothed). It asks again for the
// way to its next cell when it strays, stalls, or changes layer, merges the
// answer into what is left, and gives up after enough failures.

#include "blueprints/footprint.hpp"
#include "core/types.hpp"
#include "sim/path_finder.hpp"
#include "sim/path_search.hpp"

#include <limits>
#include <vector>

namespace osc::sim::path {

/// What the navigator reads of its unit each tick.
struct NavUnit {
    f32 x = 0;
    f32 y = 0;
    f32 z = 0;
    bool moved = false;                 ///< its position changed since the last tick
    bool immobile = false;              ///< UNITSTATE_Immobile
    bool attacking = false;             ///< UNITSTATE_Attacking
    u32 layer = 0;                      ///< its layer, as a token
    bool waiting_for_transport = false; ///< UNITSTATE_WaitingForTransport
};

class PathNavigator final : public PathListener {
    friend struct osc::sim::StateIO; // snapshots (state_io.hpp)
public:
    /// Moho's EAiPathNavigatorState, in its order (Execute compares it).
    enum class State : u8 {
        Idle,           ///< arrived, or never sent
        Failed,         ///< gave up
        Thinking,       ///< a goal, its search asked next tick
        Searching,      ///< PathEvent3: waiting for the first answer
        Continuing,     ///< PathEvent4: waiting for the way to its next cell
        HasPath,        ///< following its path
        FollowingLeader ///< (formations: not yet)
    };

    PathNavigator() = default;
    PathNavigator(const PathNavigator&) = delete;
    PathNavigator& operator=(const PathNavigator&) = delete;

    void set_unit(const blueprints::Footprint& fp, i32 footprint_class, bool on_water);
    /// CAiNavigatorLand::ApplyGoalAndStartPathing: drop the old path, take
    /// `goal`, note the unit's cell at (x, z), and ask next tick.
    void set_goal(const NavGoal& goal, f32 x, f32 z);
    /// Moho's UpdateCurrentPosition, once a tick: `world` and `queue` are
    /// also where any search it asks for goes.
    void update(const NavUnit& unit, const PathWorld& world, PathQueue& queue);
    /// Steering found the way blocked (Moho's CAiNavigatorLand::Func1, which
    /// CAiSteeringImpl calls once a unit pushed back off a cell it won't fit
    /// settles): the next update asks the way to its next cell afresh.
    void request_repath() { repath_requested_ = true; }
    /// Moho's ResetPathState: no path, no listening, Idle. A search already
    /// queued stays queued, unheard, as Moho's does.
    void reset();

    State state() const { return state_; }
    /// Everything that decides its next step (its path, cells, clocks,
    /// counts and its finder's), for the sync checksum.
    void fingerprint(Fnv& f) const;
    const NavGoal& goal() const { return goal_; }
    Cell current() const { return current_; }
    /// The cell it heads for now.
    Cell target() const { return target_; }
    /// GetTargetPos: the target cell's centre for its footprint.
    f32 target_x() const;
    f32 target_z() const;
    /// The path ahead, the target first once it has one.
    const std::vector<Cell>& path() const { return path_; }
    bool cell_in_goal(Cell c) const;
    PathFinder& finder() { return finder_; }
    const PathFinder& finder() const { return finder_; }

    // PathListener: Moho's OnEvent.
    void on_path_event(bool reached, const std::vector<Cell>& cells) override;

private:
    // Moho's helpers (CAiPathNavigator.cpp's file-local functions).
    bool can_occupy(Cell from, Cell to) const;
    bool can_transition(Cell from, Cell to) const;
    bool can_reach_from_current(Cell to) const;
    bool update_forward_probe();
    i32 direct_prefix_span();
    i32 consume_prefix(i32 count);
    void set_current(f32 x, f32 z);
    void set_target_point(i32 index);
    bool try_advance_target_point();
    void request_path(i32 mode);
    void request_continuation(i32 mode);
    void queue_search(SearchType type);

    PathFinder finder_;
    blueprints::Footprint footprint_;
    bool on_water_ = false;
    // The world, queue and unit of the last update: an answer can come
    // between updates, and may ask again.
    PathWorld world_;
    PathQueue* queue_ = nullptr;
    NavUnit unit_;

    State state_ = State::Idle;
    NavGoal goal_;
    std::vector<Cell> path_;
    Cell current_;
    Cell target_;
    u32 last_blocked_cell_ = 0; ///< packed; 0 none
    u32 last_layer_ = 0;
    i32 last_node_index_ = -1;
    i32 search_fail_count_ = 0;
    i32 retry_delay_ = 0;
    i32 no_forward_fail_count_ = 0;
    f32 repath_threshold_ = std::numeric_limits<f32>::infinity();
    i32 no_progress_ticks_ = 0;
    bool forward_probe_ = false;
    bool repath_requested_ = false;
    bool extended_probe_ = false;
    bool target_within_one_cell_ = false;
    i32 request_mode_ = 0;
    i32 request_countdown_ = 0;
};

} // namespace osc::sim::path
