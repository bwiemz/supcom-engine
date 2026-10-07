#pragma once

// Moho's CAiPathFinder (roadmap item 4c-2, faf-re CAiPathFinder.cpp): one
// unit's searches, as the path queue asks them. It decides where a search
// may look cell by cell (near its start and its goal, or round the unit when
// it repaths), which cells its footprint may stand on, which edges leave the
// playable area, and what counts as the goal.

#include "blueprints/footprint.hpp"
#include "core/types.hpp"
#include "sim/occupancy.hpp"
#include "sim/path_search.hpp"

#include <deque>
#include <functional>
#include <vector>

namespace osc::map {
class Terrain;
}

namespace osc::sim::path {

/// Moho's EAiPathSearchType.
enum class SearchType : u8 { None = 0, Initial = 1, Repath = 2, Leader = 3 };

/// Moho's SNavGoal: a cell of `outer` that is not in `inner` (an empty
/// inner rect: any cell of `outer`).
struct NavGoal {
    OccupancyRect outer;
    OccupancyRect inner;
};

/// What a search sees of the world.
struct PathWorld {
    const map::Terrain* terrain = nullptr;
    const OccupancyGrid* grid = nullptr;
    OccupancyRect playable; ///< Moho's mPlayableRect
    bool use_whole_map = false;
    i32 pathcap = 0;
};

class PathFinder final : public Traveler {
public:
    /// The unit it searches for: its footprint (resolved to its class, or
    /// its alt one), and whether it is on the Water layer (where SUB
    /// doesn't count).
    void set_unit(const blueprints::Footprint& fp, i32 footprint_class, bool on_water);
    /// Moho's SetGoal: also whether the goal's edge leaves the playable
    /// area. Uses the world's playable area as last prepared.
    void set_goal(const NavGoal& goal);
    /// Moho's QueueSearch, less the queueing: a search of `type` from
    /// `anchor` for the unit standing at (x, z). Its footprint test runs only
    /// if the unit fits where it stands (so one inside an obstacle can walk
    /// out); a repath remembers the 16 x 16 round the unit, searched cell by
    /// cell.
    void prepare(const PathWorld& world, SearchType type, Cell anchor, f32 x, f32 z);

    /// Mobile units in the way, for repaths (4c-3): (cell, mode 1 or 2 for
    /// a leader). None yet.
    std::function<bool(Cell, i32)> unit_blocked;

    // Traveler
    i32 footprint_class() const override { return class_; }
    Cell anchor() const override { return anchor_; }
    i32 pathcap() const override { return world_.pathcap; }
    f32 heuristic(Cell c) const override;
    bool is_goal(Cell c) const override;
    bool should_search_rect(const OccupancyRect& r) const override;
    bool can_traverse(Cell c) const override;
    bool in_bounds(Cell from, Cell to, f32& cost) const override;
    void on_path(bool reached, std::vector<Cell> cells) override;

    /// The last search's answer, once it has one.
    bool has_result() const { return has_result_; }
    bool reached() const { return reached_; }
    const std::vector<Cell>& path() const { return path_; }
    bool goal_boundary_blocked() const { return goal_boundary_blocked_; }
    const std::deque<OccupancyRect>& history() const { return history_; }

private:
    PathWorld world_;
    blueprints::Footprint footprint_;
    i32 class_ = -1;
    bool on_water_ = false;
    i32 max_span_ = 1;
    NavGoal goal_;
    bool goal_boundary_blocked_ = false;
    SearchType type_ = SearchType::None;
    Cell anchor_;
    bool inside_playable_ = false;
    bool has_occupancy_mask_ = false;
    std::deque<OccupancyRect> history_; ///< newest first, at most 3
    bool has_result_ = false;
    bool reached_ = false;
    std::vector<Cell> path_;
};

} // namespace osc::sim::path
