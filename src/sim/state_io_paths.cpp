// Snapshots of Moho pathing (roadmap item 4c-2c): each unit's path
// navigator and finder, each army's path queue with its search in flight,
// and each footprint class's cluster map's dirty bits. Moho restarts a
// search in flight on a load; the engine resumes it, so a loaded game goes
// on exactly as the saved one would (the save/load oracle compares them).

#include "sim/army_brain.hpp"
#include "sim/entity_registry.hpp"
#include "sim/navigator.hpp"
#include "sim/path_clusters.hpp"
#include "sim/path_finder.hpp"
#include "sim/path_navigator.hpp"
#include "sim/path_search.hpp"
#include "sim/path_tables.hpp"
#include "sim/sim_state.hpp"
#include "sim/state_io.hpp"
#include "sim/unit.hpp"

namespace osc::sim {

namespace {

void save_rect(StateWriter& w, const OccupancyRect& r) {
    w.i32v(r.x0);
    w.i32v(r.z0);
    w.i32v(r.x1);
    w.i32v(r.z1);
}

OccupancyRect load_rect(StateReader& r) {
    OccupancyRect o;
    o.x0 = r.i32v();
    o.z0 = r.i32v();
    o.x1 = r.i32v();
    o.z1 = r.i32v();
    return o;
}

void save_cell(StateWriter& w, path::Cell c) {
    w.i32v(c.x);
    w.i32v(c.z);
}

path::Cell load_cell(StateReader& r) {
    path::Cell c;
    c.x = r.i32v();
    c.z = r.i32v();
    return c;
}

void save_cells(StateWriter& w, const std::vector<path::Cell>& cells) {
    w.size(cells.size());
    for (const path::Cell c : cells) save_cell(w, c);
}

std::vector<path::Cell> load_cells(StateReader& r) {
    std::vector<path::Cell> cells(r.size(8));
    for (path::Cell& c : cells) c = load_cell(r);
    return cells;
}

void save_footprint(StateWriter& w, const blueprints::Footprint& f) {
    w.u8v(f.size_x);
    w.u8v(f.size_z);
    w.u8v(f.caps);
    w.u8v(f.flags);
    w.f32v(f.max_slope);
    w.f32v(f.min_water_depth);
    w.f32v(f.max_water_depth);
}

blueprints::Footprint load_footprint(StateReader& r) {
    blueprints::Footprint f;
    f.size_x = r.u8v();
    f.size_z = r.u8v();
    f.caps = r.u8v();
    f.flags = r.u8v();
    f.max_slope = r.f32v();
    f.min_water_depth = r.f32v();
    f.max_water_depth = r.f32v();
    return f;
}

void save_goal(StateWriter& w, const path::NavGoal& g) {
    save_rect(w, g.outer);
    save_rect(w, g.inner);
}

path::NavGoal load_goal(StateReader& r) {
    path::NavGoal g;
    g.outer = load_rect(r);
    g.inner = load_rect(r);
    return g;
}

// A PathWorld's terrain and grid are the sim's own, set again on a load.
void save_world(StateWriter& w, const path::PathWorld& world) {
    save_rect(w, world.playable);
    w.b(world.use_whole_map);
    w.i32v(world.pathcap);
}

path::PathWorld load_world(StateReader& r, const SimState& sim) {
    path::PathWorld world;
    world.terrain = sim.terrain();
    world.grid = &sim.occupancy();
    world.playable = load_rect(r);
    world.use_whole_map = r.b();
    world.pathcap = r.i32v();
    return world;
}

} // namespace

u32 StateIO::traveler_id(const SimState& sim, const path::Traveler* t) {
    u32 id = 0;
    sim.entity_registry().for_each_unit([&](const Entity& e) {
        if (id == 0 && &static_cast<const Unit&>(e).navigator_.moho_.finder_ == t)
            id = e.entity_id();
    });
    return id;
}

path::PathFinder* StateIO::traveler_of(SimState& sim, u32 id) {
    Entity* e = sim.entity_registry().find(id);
    if (!e || !e->is_unit()) return nullptr;
    return &static_cast<Unit*>(e)->navigator_.moho_.finder_;
}

// ------------------------------------------------------------- PathFinder

void StateIO::save(StateWriter& w, const path::PathFinder& f) {
    // queue_: the army queue's load sets it; unit_blocked: a hook (4c-3)
    save_world(w, f.world_);
    save_footprint(w, f.footprint_);
    w.i32v(f.class_);
    w.b(f.on_water_);
    w.i32v(f.max_span_);
    save_goal(w, f.goal_);
    w.b(f.goal_boundary_blocked_);
    w.u8v(static_cast<u8>(f.type_));
    save_cell(w, f.anchor_);
    w.b(f.inside_playable_);
    w.b(f.has_occupancy_mask_);
    w.size(f.history_.size());
    for (const OccupancyRect& h : f.history_) save_rect(w, h);
    w.b(f.has_result_);
    w.b(f.reached_);
    save_cells(w, f.path_);
    w.b(f.listener_ != nullptr); // only ever its own navigator
}

void StateIO::load(StateReader& r, path::PathFinder& f, const SimState& sim,
                   path::PathListener* owner) {
    f.world_ = load_world(r, sim);
    f.footprint_ = load_footprint(r);
    f.class_ = r.i32v();
    f.on_water_ = r.b();
    f.max_span_ = r.i32v();
    f.goal_ = load_goal(r);
    f.goal_boundary_blocked_ = r.b();
    f.type_ = static_cast<path::SearchType>(r.u8v());
    f.anchor_ = load_cell(r);
    f.inside_playable_ = r.b();
    f.has_occupancy_mask_ = r.b();
    f.history_.resize(r.size(16));
    for (OccupancyRect& h : f.history_) h = load_rect(r);
    f.has_result_ = r.b();
    f.reached_ = r.b();
    f.path_ = load_cells(r);
    f.listener_ = r.b() ? owner : nullptr;
}

// ---------------------------------------------------------- PathNavigator

void StateIO::save(StateWriter& w, const path::PathNavigator& n) {
    // queue_: its unit's army's, set again on a load
    save(w, n.finder_);
    save_footprint(w, n.footprint_);
    w.b(n.on_water_);
    save_world(w, n.world_);
    w.f32v(n.unit_.x);
    w.f32v(n.unit_.z);
    w.b(n.unit_.moved);
    w.b(n.unit_.immobile);
    w.b(n.unit_.attacking);
    w.u32v(n.unit_.layer);
    w.u8v(static_cast<u8>(n.state_));
    save_goal(w, n.goal_);
    save_cells(w, n.path_);
    save_cell(w, n.current_);
    save_cell(w, n.target_);
    w.u32v(n.last_blocked_cell_);
    w.u32v(n.last_layer_);
    w.i32v(n.last_node_index_);
    w.i32v(n.search_fail_count_);
    w.i32v(n.retry_delay_);
    w.i32v(n.no_forward_fail_count_);
    w.f32v(n.repath_threshold_);
    w.i32v(n.no_progress_ticks_);
    w.b(n.forward_probe_);
    w.b(n.repath_requested_);
    w.b(n.extended_probe_);
    w.b(n.target_within_one_cell_);
    w.i32v(n.request_mode_);
    w.i32v(n.request_countdown_);
}

void StateIO::load(StateReader& r, path::PathNavigator& n, SimState& sim, i32 army) {
    load(r, n.finder_, sim, &n);
    n.footprint_ = load_footprint(r);
    n.on_water_ = r.b();
    n.world_ = load_world(r, sim);
    ArmyBrain* a = sim.get_army(army);
    n.queue_ = a ? &a->path_queue() : nullptr;
    n.unit_.x = r.f32v();
    n.unit_.z = r.f32v();
    n.unit_.moved = r.b();
    n.unit_.immobile = r.b();
    n.unit_.attacking = r.b();
    n.unit_.layer = r.u32v();
    n.state_ = static_cast<path::PathNavigator::State>(r.u8v());
    n.goal_ = load_goal(r);
    n.path_ = load_cells(r);
    n.current_ = load_cell(r);
    n.target_ = load_cell(r);
    n.last_blocked_cell_ = r.u32v();
    n.last_layer_ = r.u32v();
    n.last_node_index_ = r.i32v();
    n.search_fail_count_ = r.i32v();
    n.retry_delay_ = r.i32v();
    n.no_forward_fail_count_ = r.i32v();
    n.repath_threshold_ = r.f32v();
    n.no_progress_ticks_ = r.i32v();
    n.forward_probe_ = r.b();
    n.repath_requested_ = r.b();
    n.extended_probe_ = r.b();
    n.target_within_one_cell_ = r.b();
    n.request_mode_ = r.i32v();
    n.request_countdown_ = r.i32v();
}

// --------------------------------------------------------- PathSearch

void StateIO::save(StateWriter& w, const path::PathSearch& s) {
    // traveler_: the queue's save names it; index_: made again from nodes_
    w.size(s.nodes_.size());
    for (const path::PathSearch::Node& n : s.nodes_) {
        save_cell(w, n.cell);
        w.u8v(static_cast<u8>(n.state));
        w.i32v(n.parent);
        w.f32v(n.cost);
        w.f32v(n.estimate);
        w.i32v(n.handle);
    }
    // OpenHeap
    w.size(s.open_.entries_.size());
    for (const path::OpenHeap::Entry& e : s.open_.entries_) {
        w.f32v(e.priority);
        w.u32v(e.node);
        w.i32v(e.handle);
    }
    w.size(s.open_.index_by_handle_.size());
    for (const i32 i : s.open_.index_by_handle_) w.i32v(i);
    w.i32v(s.open_.free_handle_);
    save_cell(w, s.closest_);
    w.f32v(s.closest_distance_);
    w.i32v(s.expand_count_);
    w.i32v(s.pathcap_);
}

void StateIO::load(StateReader& r, path::PathSearch& s) {
    s.nodes_.resize(r.size(25));
    s.index_.clear();
    for (size_t i = 0; i < s.nodes_.size() && r.ok(); ++i) {
        path::PathSearch::Node& n = s.nodes_[i];
        n.cell = load_cell(r);
        n.state = static_cast<path::PathSearch::State>(r.u8v());
        n.parent = r.i32v();
        n.cost = r.f32v();
        n.estimate = r.f32v();
        n.handle = r.i32v();
        s.index_[path::pack_cell(n.cell)] = static_cast<u32>(i);
    }
    std::vector<path::OpenHeap::Entry> entries(r.size(12));
    for (path::OpenHeap::Entry& e : entries) {
        e.priority = r.f32v();
        e.node = r.u32v();
        e.handle = r.i32v();
        if (e.node >= s.nodes_.size()) return r.fail("a path search's heap names no node");
    }
    std::vector<i32> by_handle(r.size(4));
    for (i32& i : by_handle) i = r.i32v();
    s.open_.restore(std::move(entries), std::move(by_handle), r.i32v());
    s.closest_ = load_cell(r);
    s.closest_distance_ = r.f32v();
    s.expand_count_ = r.i32v();
    s.pathcap_ = r.i32v();
}

// ---------------------------------------------------------- PathQueue

void StateIO::save(StateWriter& w, const path::PathQueue& q, const SimState& sim) {
    w.size(q.pending_.size());
    for (const path::Traveler* t : q.pending_) w.u32v(traveler_id(sim, t));
    w.u32v(q.search_.traveler_ ? traveler_id(sim, q.search_.traveler_) : 0);
    save(w, q.search_);
}

void StateIO::load(StateReader& r, path::PathQueue& q, SimState& sim) {
    for (path::Traveler* t : q.pending_) t->queue_ = nullptr;
    if (q.search_.traveler_) q.search_.traveler_->queue_ = nullptr;
    q.pending_.clear();
    q.search_.traveler_ = nullptr;
    const size_t pending = r.size(4);
    for (size_t i = 0; i < pending && r.ok(); ++i) {
        path::PathFinder* f = traveler_of(sim, r.u32v());
        if (!f) return r.fail("a path queue names no unit");
        q.pending_.push_back(f);
        f->queue_ = &q;
    }
    if (const u32 id = r.u32v(); id != 0) {
        path::PathFinder* f = traveler_of(sim, id);
        if (!f) return r.fail("a path search names no unit");
        q.search_.traveler_ = f;
        f->queue_ = &q;
    }
    load(r, q.search_);
}

// ------------------------------------------------------- Cluster maps

void StateIO::save_path_maps(StateWriter& w, const SimState& sim) {
    // ClusterMap: source_, cache_, width_, height_, size_x_, size_z_ and
    // levels_ follow from the map and the classes (levels_ rebuilt below)
    const PathTables* tables = sim.path_tables_.get();
    w.size(tables ? tables->size() : 0);
    if (!tables) return;
    for (size_t m = 0; m < tables->size(); ++m) {
        const path::ClusterMap& map = tables->map(m);
        for (u32 level = 1; level <= path::kClusterLevels; ++level) {
            const path::BitGrid& bits = map.dirty_[level];
            w.i32v(bits.width_);
            w.i32v(bits.height_);
            w.size(bits.words_.size());
            for (const u32 word : bits.words_) w.u32v(word);
        }
        w.u32v(map.progress_);
        w.b(map.done_);
    }
}

void StateIO::load_path_maps(StateReader& r, SimState& sim) {
    PathTables* tables = sim.path_tables_.get();
    const size_t maps = r.size(4);
    if (maps != (tables ? tables->size() : 0)) return r.fail("another count of path maps");
    for (size_t m = 0; m < maps && r.ok(); ++m) {
        path::ClusterMap& map = tables->map(m);
        for (u32 level = 1; level <= path::kClusterLevels; ++level) {
            path::BitGrid& bits = map.dirty_[level];
            const i32 width = r.i32v();
            const i32 height = r.i32v();
            if (width != bits.width_ || height != bits.height_)
                return r.fail("a path map of another size");
            bits.words_.resize(r.size(4));
            for (u32& word : bits.words_) word = r.u32v();
        }
        map.progress_ = r.u32v();
        map.done_ = r.b();
        map.rebuild_clean();
    }
}

} // namespace osc::sim
