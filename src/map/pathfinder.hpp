#pragma once

#include "core/types.hpp"
#include "sim/entity.hpp" // Vector3

#include <string>
#include <utility>
#include <vector>

namespace osc::map {

class PathfindingGrid;

struct PathResult {
    bool found = false;
    bool throttled = false;  // true if request was deferred (budget exhausted)
    /// found, but the goal itself is unreachable: the path ends at the
    /// reachable cell closest to it (as FA units stop at the cliff edge).
    bool partial = false;
    std::vector<sim::Vector3> waypoints; // world-space positions
};

/// Answer to "can this unit get there at all?" (see Pathfinder::reachability).
struct Reachability {
    bool reachable = false;
    /// The goal when reachable; otherwise the reachable point closest to it,
    /// searched out to BEST_POINT_SEARCH_RADIUS cells from the goal, else
    /// the start (as retail's own graph fallback does).
    f32 best_x = 0;
    f32 best_z = 0;
};

class Pathfinder {
public:
    explicit Pathfinder(const PathfindingGrid& grid);

    /// Find a path from start to goal for the given movement layer.
    /// Returns smoothed waypoints in world coordinates.
    PathResult find_path(f32 start_x, f32 start_z,
                         f32 goal_x, f32 goal_z,
                         const std::string& layer,
                         f32 draft = 0, bool amphibious = false) const;

    bool can_pathfind() const { return requests_this_tick_ < MAX_REQUESTS_PER_TICK; }
    void increment_request_count() const { ++requests_this_tick_; }
    void reset_request_count() const { requests_this_tick_ = 0; }
    int requests_this_tick() const { return requests_this_tick_; }
    static constexpr int MAX_REQUESTS_PER_TICK = 8;

    /// Can a unit on `layer` get from start to goal at all? Answered from
    /// connected-component labels of the passability grid (cached per
    /// passability class, rebuilt when obstacles change) rather than a path
    /// search, so it neither uses nor is refused by the per-tick path budget.
    /// Script queries (unit:CanPathTo) need that: an AI told "unreachable"
    /// because movement orders spent the budget calls for transports instead.
    /// A goal on an impassable cell counts as reached from the nearest ring of
    /// passable cells around it, as find_path() retargets such goals.
    Reachability reachability(f32 start_x, f32 start_z, f32 goal_x, f32 goal_z,
                              const std::string& layer, f32 draft = 0,
                              bool amphibious = false) const;
    bool reachable(f32 start_x, f32 start_z, f32 goal_x, f32 goal_z,
                   const std::string& layer, f32 draft = 0,
                   bool amphibious = false) const {
        return reachability(start_x, start_z, goal_x, goal_z, layer, draft,
                            amphibious).reachable;
    }

    /// How far (in cells) reachability() looks around an unreachable goal
    /// for the closest point the unit can reach. Bounded so a query across
    /// open water stays cheap.
    static constexpr i32 BEST_POINT_SEARCH_RADIUS = 64;

private:
    /// Connected components of one passability class; 0 = impassable cell.
    /// Movement is 8-way without corner cutting, which connects exactly what
    /// 4-way movement does, so the labels use 4-connectivity.
    struct ComponentLabels {
        bool amphibious = false;
        bool naval = false;
        f32 draft = 0;
        u64 grid_version = 0;
        u64 last_used = 0;
        std::vector<u32> labels;
    };
    const ComponentLabels& labels_for(const std::string& layer, f32 draft,
                                      bool amphibious) const;
    void build_labels(ComponentLabels& set) const;

    mutable std::vector<ComponentLabels> label_cache_;
    mutable u64 label_uses_ = 0;
    /// Land, amphibious and one per naval draft bucket. Retail uses five
    /// drafts (0, 1.5, 2, 3.6, 5), so 7 sets are live at once; fewer slots
    /// would evict and relabel (a full flood fill) on every query.
    static constexpr size_t MAX_LABEL_SETS = 10;
    /// Draft keys are rounded up to this step so modded drafts share sets.
    /// Rounding up keeps the answer conservative: never "reachable" where
    /// the unit's own draft can't pass.
    static constexpr f32 DRAFT_KEY_STEP = 0.5f;

    struct GridPath {
        std::vector<std::pair<u32, u32>> cells; ///< start -> end, inclusive
        bool reached_goal = false;
    };

    /// Raw A* on the grid. If the goal is unreachable (or the search limit
    /// is hit) the path leads to the explored cell closest to the goal
    /// instead; empty only when no cell other than the start is reachable.
    /// From a start cell inside a structure's footprint, or when `closed_in`,
    /// it may cross the obstacle cells joined to the start, at a cost, to
    /// get out.
    GridPath astar(u32 sx, u32 sz, u32 gx, u32 gz, const std::string& layer, f32 draft = 0,
                   bool amphibious = false, bool closed_in = false) const;

    /// Smooth path by removing redundant waypoints via line-of-sight.
    std::vector<std::pair<u32, u32>> smooth_path(
        const std::vector<std::pair<u32, u32>>& path,
        const std::string& layer, f32 draft = 0, bool amphibious = false) const;

    /// Line-of-sight check on the passability grid (Bresenham).
    bool has_line_of_sight(u32 x0, u32 z0, u32 x1, u32 z1,
                           const std::string& layer,
                           f32 draft = 0, bool amphibious = false) const;

    const PathfindingGrid& grid_;

    static constexpr u32 MAX_NODES_EXPLORED = 50000;
    mutable int requests_this_tick_ = 0;

    // Reusable buffers for A* to avoid per-call heap allocations.
    mutable std::vector<f32> g_cost_buf_;
    mutable std::vector<u32> parent_buf_;
    mutable std::vector<bool> closed_buf_;
    mutable std::vector<u8> escape_buf_; ///< the footprint a start is inside
};

} // namespace osc::map
