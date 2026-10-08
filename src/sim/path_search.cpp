#include "sim/path_search.hpp"

#include "sim/path_clusters.hpp"
#include "sim/path_tables.hpp"

#include <algorithm>
#include <array>
#include <limits>

namespace osc::sim::path {

namespace {

// The cells around a cell in Moho's order (PathTables.cpp): the sides,
// then the diagonals, each only once both its sides were taken.
constexpr std::array<i32, 8> kStepX = {0, -1, 0, 1, -1, -1, 1, 1};
constexpr std::array<i32, 8> kStepZ = {-1, 0, 1, 0, -1, 1, 1, -1};
constexpr std::array<u32, 8> kStepGate = {0, 0, 0, 0, 3, 6, 12, 9};
constexpr std::array<f32, 8> kStepCost = {1.0f, 1.0f, 1.0f, 1.0f, 1.414f, 1.414f, 1.414f, 1.414f};

/// Moho reads a cell's coordinates through their 16 bits unsigned.
i32 wide(i32 v) {
    return static_cast<i32>(static_cast<u16>(v));
}

/// And stores a cell as 16 bits signed.
Cell narrow(i32 x, i32 z) {
    return {static_cast<i16>(x), static_cast<i16>(z)};
}

} // namespace

i32 OpenHeap::push(f32 priority, u32 node) {
    const size_t at = entries_.size();
    i32 handle = 0;
    if (free_handle_ == -1) {
        handle = static_cast<i32>(index_by_handle_.size());
        index_by_handle_.push_back(static_cast<i32>(at));
    } else {
        handle = free_handle_;
        free_handle_ = index_by_handle_[static_cast<size_t>(handle)];
        index_by_handle_[static_cast<size_t>(handle)] = static_cast<i32>(at);
    }
    entries_.push_back({priority, node, handle});
    sift_up(at);
    return handle;
}

void OpenHeap::pop() {
    const size_t count = entries_.size();
    if (count == 0) return;
    const size_t last = count - 1;
    if (count != 1) {
        swap(last, 0);
        sift_down(0, last);
    }
    const i32 released = entries_[last].handle;
    index_by_handle_[static_cast<size_t>(released)] = free_handle_;
    free_handle_ = released;
    entries_.resize(last);
}

void OpenHeap::update(i32 handle, f32 priority) {
    const auto i = static_cast<size_t>(index_by_handle_[static_cast<size_t>(handle)]);
    const f32 previous = entries_[i].priority;
    entries_[i].priority = priority;
    if (previous > priority) sift_up(i);
    else sift_down(i, entries_.size());
}

void OpenHeap::clear() {
    entries_.clear();
    index_by_handle_.clear();
    free_handle_ = -1;
}

void OpenHeap::restore(std::vector<Entry> entries, std::vector<i32> index_by_handle,
                       i32 free_handle) {
    entries_ = std::move(entries);
    index_by_handle_ = std::move(index_by_handle);
    free_handle_ = free_handle;
}

void OpenHeap::swap(size_t a, size_t b) {
    std::swap(entries_[a], entries_[b]);
    index_by_handle_[static_cast<size_t>(entries_[a].handle)] = static_cast<i32>(a);
    index_by_handle_[static_cast<size_t>(entries_[b].handle)] = static_cast<i32>(b);
}

void OpenHeap::sift_up(size_t i) {
    while (i != 0) {
        const size_t parent = (i - 1) / 2;
        if (entries_[i].priority > entries_[parent].priority) break;
        swap(parent, i);
        i = parent;
    }
}

void OpenHeap::sift_down(size_t i, size_t count) {
    for (;;) {
        const size_t left = 2 * i + 1;
        if (left >= count) break;
        size_t smallest = i;
        if (entries_[i].priority > entries_[left].priority) smallest = left;
        const size_t right = left + 1;
        if (right < count && entries_[smallest].priority > entries_[right].priority)
            smallest = right;
        if (smallest == i) break;
        swap(i, smallest);
        i = smallest;
    }
}

u32 PathSearch::find_or_create(Cell c) {
    const auto [it, made] = index_.try_emplace(pack_cell(c), static_cast<u32>(nodes_.size()));
    if (made) {
        Node n;
        n.cell = c;
        nodes_.push_back(n);
    }
    return it->second;
}

void PathSearch::note_candidate(Cell c, f32 estimate) {
    if (closest_distance_ > estimate) {
        closest_ = c;
        closest_distance_ = estimate;
    }
}

void PathSearch::begin(Traveler& t) {
    traveler_ = &t;
    expand_count_ = 0;
    pathcap_ = t.pathcap();
    closest_ = t.anchor();
    closest_distance_ = std::numeric_limits<f32>::max();
    nodes_.clear();
    index_.clear();
    open_.clear();
    // Moho's AddStartNode.
    const u32 start = find_or_create(closest_);
    Node& n = nodes_[start];
    n.state = State::Open;
    const f32 estimate = t.heuristic(closest_);
    note_candidate(closest_, estimate);
    n.estimate = estimate;
    n.parent = -1;
    n.cost = 0;
    n.handle = open_.push(estimate, start);
}

PathSearch::Step PathSearch::run(PathTables& tables, i32& budget) {
    if (!traveler_) return Step::Continue;
    const i32 cls = traveler_->footprint_class();
    if (cls < 0 || static_cast<size_t>(cls) >= tables.size()) return Step::PathCapExceeded;
    ClusterMap& map = tables.map(static_cast<size_t>(cls));
    std::vector<Neighbour> neighbours;
    while (!open_.empty()) {
        const u32 cur = open_.top();
        neighbours.clear();
        const Step step = expand(nodes_[cur].cell, map, budget, neighbours);
        if (step != Step::Continue) return step;
        nodes_[cur].state = State::Closed;
        open_.pop();
        for (const Neighbour& nb : neighbours) {
            const u32 idx = find_or_create(nb.cell);
            const f32 reached = nodes_[cur].cost + nb.cost;
            Node& node = nodes_[idx];
            if (node.state == State::Unvisited) {
                node.state = State::Open;
                const f32 estimate = traveler_->heuristic(nb.cell);
                note_candidate(nb.cell, estimate);
                node.estimate = estimate;
                node.parent = static_cast<i32>(cur);
                node.cost = reached;
                node.handle = open_.push(reached + estimate, idx);
            } else if (node.state == State::Open && node.cost > reached) {
                node.parent = static_cast<i32>(cur);
                node.cost = reached;
                open_.update(node.handle, node.estimate + reached);
            }
            // Closed: never opened again.
        }
    }
    return Step::Continue;
}

PathSearch::Step PathSearch::expand(Cell c, ClusterMap& map, i32& budget,
                                    std::vector<Neighbour>& out) {
    // The budget is spent before the goal test: reaching the goal on the
    // last unit still costs it.
    --budget;
    ++expand_count_;
    if (budget <= 0) return Step::BudgetExhausted;
    if (traveler_->is_goal(c)) {
        closest_ = c;
        return Step::GoalReached;
    }
    if (expand_count_ > pathcap_) return Step::PathCapExceeded;
    return collect(c, map, budget, out) ? Step::Continue : Step::BudgetExhausted;
}

bool PathSearch::collect(Cell c, ClusterMap& map, i32& budget, std::vector<Neighbour>& out) {
    const i32 x = wide(c.x);
    const i32 z = wide(c.z);
    // Near enough to search cell by cell, or from the top of the
    // hierarchy down, as long as the cluster around it is wanted.
    i32 level = traveler_->should_search_rect(map.cluster_rect(x, z, 0))
                    ? 0
                    : static_cast<i32>(kClusterLevels);
    for (; level >= 0; --level) {
        const i32 mask = kClusterSize[static_cast<size_t>(level)] - 1;
        if ((x & mask) != 0 && (z & mask) != 0) continue; // not on its grid
        const bool ok = level == 0
                            ? add_adjacent(c, map, out)
                            : add_cluster_edges(c, static_cast<u32>(level), map, budget, out);
        if (!ok) return false;
        if (level > 0 &&
            !traveler_->should_search_rect(map.cluster_rect(x, z, static_cast<u32>(level))))
            return true;
    }
    return true;
}

bool PathSearch::add_adjacent(Cell c, const ClusterMap& map, std::vector<Neighbour>& out) {
    u32 taken = 0;
    for (size_t step = 0; step < 8; ++step) {
        if ((taken & kStepGate[step]) != kStepGate[step]) continue;
        const i32 x = wide(c.x) + kStepX[step];
        const i32 z = wide(c.z) + kStepZ[step];
        if (!traveler_->should_search_rect(map.cluster_rect(x, z, 1))) continue;
        const Cell to = narrow(x, z);
        if (!traveler_->can_traverse(to)) continue;
        f32 cost = kStepCost[step];
        if (!traveler_->in_bounds(c, to, cost)) continue;
        out.push_back({to, cost});
        taken |= 1U << step;
    }
    return true;
}

bool PathSearch::add_cluster_edges(Cell c, u32 level, ClusterMap& map, i32& budget,
                                   std::vector<Neighbour>& out) {
    const i32 shift = kClusterShift[level];
    const i32 x = wide(c.x);
    const i32 z = wide(c.z);
    const OccupancyRect clusters = map.cluster_index_rect(x, z, level);
    for (i32 cx = clusters.x0; cx < clusters.x1; ++cx) {
        const i32 ox = cx << shift;
        for (i32 cz = clusters.z0; cz < clusters.z1; ++cz) {
            const i32 oz = cz << shift;
            if (!map.work_on_cluster(cx, cz, level, budget)) return false;
            const ClusterRef held = map.cluster_ref(level, cx, cz);
            if (!held) continue;
            const Cluster& cl = *held;
            const auto n = static_cast<u32>(cl.nodes.size());
            u32 from = 0;
            while (from < n && !(cl.nodes[from].x == static_cast<u8>(x - ox) &&
                                 cl.nodes[from].z == static_cast<u8>(z - oz)))
                ++from;
            if (from == n) continue;
            for (u32 to = 0; to < n; ++to) {
                if (to == from) continue;
                const i8 bucket = cl.edge(from, to);
                if (bucket < 0) continue;
                const Cell cand = narrow(ox + cl.nodes[to].x, oz + cl.nodes[to].z);
                // Below the top, only into a parent cluster still wanted.
                if (level != kClusterLevels &&
                    !traveler_->should_search_rect(map.cluster_rect(cand.x, cand.z, level + 1)))
                    continue;
                f32 cost = dequantize_edge_cost(
                    bucket, octile_distance(static_cast<i32>(cl.nodes[to].x) - cl.nodes[from].x,
                                            static_cast<i32>(cl.nodes[to].z) - cl.nodes[from].z));
                if (!traveler_->in_bounds(c, cand, cost)) continue;
                out.push_back({cand, cost});
            }
        }
    }
    return true;
}

void PathSearch::finish(bool reached) {
    std::vector<Cell> cells;
    if (const auto it = index_.find(pack_cell(closest_)); it != index_.end()) {
        for (i32 i = static_cast<i32>(it->second); i >= 0;
             i = nodes_[static_cast<size_t>(i)].parent)
            cells.push_back(nodes_[static_cast<size_t>(i)].cell);
        std::reverse(cells.begin(), cells.end());
    }
    Traveler* t = traveler_;
    traveler_ = nullptr;
    if (t) t->on_path(reached, std::move(cells));
}

void PathSearch::fingerprint(Fnv& f) const {
    if (!traveler_) {
        f.mix(0);
        return;
    }
    // Its nodes are what its expansions made from the anchor; how many,
    // how many are open, and the nearest found tell a search's progress.
    f.mix(static_cast<u64>(traveler_->owner()) + 1);
    f.mix(nodes_.size());
    f.mix(open_.entries().size());
    f.mix(static_cast<u32>(expand_count_));
    f.mix(static_cast<u32>(pathcap_));
    f.mix(pack_cell(closest_));
    f.mix_f32(closest_distance_);
}

Traveler::~Traveler() {
    if (queue_) queue_->cancel(*this);
}

PathQueue::~PathQueue() {
    for (Traveler* t : pending_) t->queue_ = nullptr;
    if (Traveler* t = search_.traveler()) t->queue_ = nullptr;
}

void PathQueue::queue(Traveler& t) {
    // Moho's DList push_back: one already queued, or in flight, moves to
    // the back (an abandoned search starts again when its turn comes).
    if (t.queue_) t.queue_->cancel(t);
    pending_.push_back(&t);
    t.queue_ = this;
}

void PathQueue::cancel(Traveler& t) {
    if (t.queue_ != this) return;
    if (search_.traveler() == &t) search_.drop();
    pending_.erase(std::remove(pending_.begin(), pending_.end(), &t), pending_.end());
    t.queue_ = nullptr;
}

bool PathQueue::queued(const Traveler& t) const {
    return search_.traveler() == &t ||
           std::find(pending_.begin(), pending_.end(), &t) != pending_.end();
}

void PathQueue::work(PathTables& tables, i32& budget) {
    while (budget > 0) {
        if (!search_.traveler()) {
            if (pending_.empty()) return; // the rest goes unspent
            Traveler* t = pending_.front();
            pending_.pop_front();
            search_.begin(*t);
        }
        const PathSearch::Step step = search_.run(tables, budget);
        // Out of budget, it stays in flight for the next tick.
        if (step != PathSearch::Step::BudgetExhausted) {
            ++searches_done_;
            expansions_done_ += search_.expansions();
            // Off the queue before it hears: hearing, it may queue again.
            search_.traveler()->queue_ = nullptr;
            search_.finish(step == PathSearch::Step::GoalReached);
        }
    }
}

void PathQueue::fingerprint(Fnv& f) const {
    f.mix(pending_.size());
    for (const Traveler* t : pending_) f.mix(t->owner());
    search_.fingerprint(f);
}

} // namespace osc::sim::path
