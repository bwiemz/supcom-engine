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
/// A point in the world (x, height, z).
struct WorldPoint {
    f32 x = 0;
    f32 y = 0;
    f32 z = 0;
};

/// Mobile units in a unit's way (4c-3), answered by the sim: Moho's
/// COGrid::UnitIsBlocked and SweptPathBlockedByUnit. `mode` 1 plans (units
/// that moved last tick don't count, and of the rest only those that
/// outrank the unit); 2 is a leader's search or the extended probe's
/// (every unit the unit doesn't ignore).
class MobileBlockers {
public:
    virtual ~MobileBlockers() = default;
    /// One stands where `unit`'s footprint would at `cell` (its corner).
    virtual bool unit_blocked(u32 unit, Cell cell, i32 mode) const = 0;
    /// The same at the cell a footprint centred at (x, z) takes (Moho's
    /// UnitIsBlockedAt).
    virtual bool unit_blocked_at(u32 unit, f32 x, f32 z, i32 mode) const = 0;
    /// One stands across the straight way from `from` to `to`.
    virtual bool swept_blocked(u32 unit, const WorldPoint& from, const WorldPoint& to,
                               i32 mode) const = 0;
};

struct PathWorld {
    const map::Terrain* terrain = nullptr;
    const OccupancyGrid* grid = nullptr;
    OccupancyRect playable; ///< Moho's mPlayableRect
    bool use_whole_map = false;
    i32 pathcap = 0;
    /// The mobile units in the way, and the unit whose way it is (its id).
    const MobileBlockers* blockers = nullptr;
    u32 owner = 0;
};

/// Who hears a finder's answer (Moho's Listener<const SNavPath&>).
class PathListener {
public:
    virtual ~PathListener() = default;
    virtual void on_path_event(bool reached, const std::vector<Cell>& cells) = 0;
};

class PathFinder final : public Traveler {
    friend struct osc::sim::StateIO; // snapshots (state_io.hpp)
public:
    /// Hear the next answer, once (Moho's AddListener; the listener
    /// unlinks as it hears).
    void listen(PathListener* l) { listener_ = l; }
    PathListener* listener() const { return listener_; }
    /// The search type for the next prepare (the navigator sets it back to
    /// None as an answer comes, as Moho's does).
    void set_type(SearchType type) { type_ = type; }
    SearchType type() const { return type_; }
    const NavGoal& goal() const { return goal_; }
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
    PathListener* listener_ = nullptr;
};

} // namespace osc::sim::path
