#pragma once

// Moho's hierarchical path search (roadmap item 4c-2, faf-re PathTables.cpp
// PathQueue, AStarSearch.h): an A* over cells near a path's ends and over
// the cluster maps' portals between them, a little at a time from an army's
// budget, one traveler after another. See
// docs/plans/2026-10-07-per-class-pathing-design.md.

#include "core/types.hpp"
#include "sim/occupancy.hpp" // OccupancyRect

#include <deque>
#include <unordered_map>
#include <vector>

namespace osc::sim {
class PathTables;
struct StateIO;
} // namespace osc::sim

namespace osc::sim::path {

class ClusterMap;
class PathQueue;

/// Moho's SOCellPos: a footprint's corner cell.
struct Cell {
    i32 x = 0;
    i32 z = 0;
    bool operator==(const Cell& o) const { return x == o.x && z == o.z; }
    bool operator!=(const Cell& o) const { return !(*this == o); }
};

/// Moho's IPathTraveler: what a search asks of whoever it searches for.
class Traveler {
    friend struct osc::sim::StateIO; // snapshots (state_io.hpp)
public:
    Traveler() = default;
    Traveler(const Traveler&) = delete;
    Traveler& operator=(const Traveler&) = delete;
    /// Leaves the queue it is on, waiting or in flight (Moho's intrusive
    /// list node unlinks as it goes, a fix faf-re's ~CAiPathFinder notes).
    virtual ~Traveler();
    /// The queue it is on, if any.
    const PathQueue* queue() const { return queue_; }
    /// Its footprint class (which cluster map it searches).
    virtual i32 footprint_class() const = 0;
    /// Where the search starts.
    virtual Cell anchor() const = 0;
    /// The most cells one search may expand (Moho's pathcap).
    virtual i32 pathcap() const = 0;
    virtual f32 heuristic(Cell c) const = 0;
    virtual bool is_goal(Cell c) const = 0;
    /// Whether to search this world rect at full resolution (or, for a
    /// cluster's rect, through its finer portals).
    virtual bool should_search_rect(const OccupancyRect& r) const = 0;
    virtual bool can_traverse(Cell c) const = 0;
    /// Whether to take the edge to `to`; may change its cost.
    virtual bool in_bounds(Cell from, Cell to, f32& cost) const = 0;
    /// The search ended: the cells from its anchor to the goal (`reached`),
    /// else to the cell it found nearest the goal.
    virtual void on_path(bool reached, std::vector<Cell> cells) = 0;

private:
    friend class PathQueue;
    PathQueue* queue_ = nullptr;
};

/// Moho's AStarOpenHeap: a binary min-heap of handles, ties broken as
/// Moho's are (an equal priority rises past its parent).
class OpenHeap {
    friend struct osc::sim::StateIO; // snapshots (state_io.hpp)
public:
    struct Entry {
        f32 priority = 0;
        u32 node = 0; ///< the search's node index
        i32 handle = 0;
    };

    bool empty() const { return entries_.empty(); }
    u32 top() const { return entries_[0].node; }
    i32 push(f32 priority, u32 node);
    void pop();
    void update(i32 handle, f32 priority);
    void clear();

    const std::vector<Entry>& entries() const { return entries_; }
    const std::vector<i32>& index_by_handle() const { return index_by_handle_; }
    i32 free_handle() const { return free_handle_; }
    /// Restore exactly as saved.
    void restore(std::vector<Entry> entries, std::vector<i32> index_by_handle, i32 free_handle);

private:
    void swap(size_t a, size_t b);
    void sift_up(size_t i);
    void sift_down(size_t i, size_t count);

    std::vector<Entry> entries_;
    std::vector<i32> index_by_handle_;
    i32 free_handle_ = -1;
};

/// One search, as Moho's PathQueue::ImplBase runs it over AStarSearch.
class PathSearch {
    friend struct osc::sim::StateIO; // snapshots (state_io.hpp)
public:
    enum class Step { Continue, GoalReached, BudgetExhausted, PathCapExceeded };
    enum class State : u8 { Unvisited, Open, Closed };
    struct Node {
        Cell cell;
        State state = State::Unvisited;
        i32 parent = -1; ///< node index
        f32 cost = 0;
        f32 estimate = 0;
        i32 handle = 0;
    };

    /// Start a search for `t` (Moho's BeginQuery).
    void begin(Traveler& t);
    /// Expand from `budget`, building the clusters it reads from it too,
    /// until it ends or the budget is spent (Moho's WorkOnce).
    Step run(PathTables& tables, i32& budget);
    /// Hand `t` the path to the goal or the nearest cell (FinishQuery).
    void finish(bool reached);

    Traveler* traveler() const { return traveler_; }
    void drop() { traveler_ = nullptr; }
    u32 expansions() const { return static_cast<u32>(expand_count_); }
    const std::vector<Node>& nodes() const { return nodes_; }
    const OpenHeap& open() const { return open_; }

private:
    struct Neighbour {
        Cell cell;
        f32 cost = 0;
    };

    u32 find_or_create(Cell c);
    void note_candidate(Cell c, f32 estimate);
    Step expand(Cell c, ClusterMap& map, i32& budget, std::vector<Neighbour>& out);
    bool collect(Cell c, ClusterMap& map, i32& budget, std::vector<Neighbour>& out);
    bool add_adjacent(Cell c, const ClusterMap& map, std::vector<Neighbour>& out);
    bool add_cluster_edges(Cell c, u32 level, ClusterMap& map, i32& budget,
                           std::vector<Neighbour>& out);

    Traveler* traveler_ = nullptr;
    std::vector<Node> nodes_;
    std::unordered_map<u32, u32> index_; ///< packed cell -> node (lookup only)
    OpenHeap open_;
    Cell closest_;
    f32 closest_distance_ = 0;
    i32 expand_count_ = 0;
    i32 pathcap_ = 0;
};

/// Moho's PathQueue: one army's searches, first come first served, one in
/// flight, from the budget it is given each tick.
class PathQueue {
    friend struct osc::sim::StateIO; // snapshots (state_io.hpp)
public:
    PathQueue() = default;
    PathQueue(const PathQueue&) = delete;
    PathQueue& operator=(const PathQueue&) = delete;
    /// Lets go of its travelers, who no longer point at it.
    ~PathQueue();

    /// To the back of the queue, from wherever it was in it.
    void queue(Traveler& t);
    /// Take `t` off the queue, finished or not (it hears nothing).
    void cancel(Traveler& t);
    bool queued(const Traveler& t) const;
    /// Moho's Work: search until the budget is spent or nobody waits.
    void work(PathTables& tables, i32& budget);

    const std::deque<Traveler*>& pending() const { return pending_; }
    const PathSearch& search() const { return search_; }

private:
    std::deque<Traveler*> pending_;
    PathSearch search_;
};

/// Cell `c` packed for a lookup, as Moho's int16 x and z.
inline u32 pack_cell(Cell c) {
    return static_cast<u32>(static_cast<u16>(c.x)) |
           (static_cast<u32>(static_cast<u16>(c.z)) << 16);
}

} // namespace osc::sim::path
