#include "sim/path_navigator.hpp"

#include "map/terrain.hpp"
#include "sim/occupancy.hpp"
#include "sim/path_walk.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace osc::sim::path {

namespace {

constexpr f32 kInfinity = std::numeric_limits<f32>::infinity();

f32 cell_distance(Cell a, Cell b) {
    const auto dx = static_cast<f32>(a.x - b.x);
    const auto dz = static_cast<f32>(a.z - b.z);
    return std::sqrt(dx * dx + dz * dz);
}

i32 manhattan(Cell a, Cell b) {
    return std::abs(a.x - b.x) + std::abs(a.z - b.z);
}

NavGoal single_cell_goal(Cell c) {
    return {{c.x, c.z, c.x + 1, c.z + 1}, {}};
}

Cell goal_centre(const NavGoal& g) {
    return {static_cast<i16>((g.outer.x0 + g.outer.x1) / 2),
            static_cast<i16>((g.outer.z0 + g.outer.z1) / 2)};
}

SearchType search_type(i32 mode) {
    switch (mode) {
    case 1: return SearchType::Initial;
    case 2: return SearchType::Repath;
    case 3: return SearchType::Leader;
    default: return SearchType::None;
    }
}

} // namespace

void PathNavigator::set_unit(const blueprints::Footprint& fp, i32 footprint_class, bool on_water) {
    footprint_ = fp;
    on_water_ = on_water;
    finder_.set_unit(fp, footprint_class, on_water);
}

void PathNavigator::set_goal(const NavGoal& goal, f32 x, f32 z) {
    reset();
    // Moho's ConfigureGoal (formation and leader state aside).
    goal_ = goal;
    last_layer_ = 0;
    extended_probe_ = false;
    target_within_one_cell_ = false;
    set_current(x, z);
    // BeginThinking: the search is asked on the next update.
    state_ = State::Thinking;
    request_mode_ = 0;
    request_countdown_ = 1;
}

void PathNavigator::reset() {
    target_ = current_;
    path_.clear();
    if (finder_.listener() == this) finder_.listen(nullptr);
    finder_.set_type(SearchType::None);
    state_ = State::Idle;
    retry_delay_ = 0;
    search_fail_count_ = 0;
    no_forward_fail_count_ = 0;
    last_layer_ = 0;
    forward_probe_ = false;
    repath_requested_ = false;
    extended_probe_ = false;
    target_within_one_cell_ = false;
    request_countdown_ = 0;
    last_node_index_ = -1;
    last_blocked_cell_ = 0;
    repath_threshold_ = kInfinity;
    no_progress_ticks_ = 0;
}

f32 PathNavigator::target_x() const {
    return static_cast<f32>(target_.x) + static_cast<f32>(footprint_.size_x) * 0.5f;
}

f32 PathNavigator::target_z() const {
    return static_cast<f32>(target_.z) + static_cast<f32>(footprint_.size_z) * 0.5f;
}

bool PathNavigator::cell_in_goal(Cell c) const {
    const OccupancyRect& o = goal_.outer;
    return o.x0 <= c.x && c.x < o.x1 && o.z0 <= c.z && c.z < o.z1;
}

void PathNavigator::set_current(f32 x, f32 z) {
    const OccupancyRect at = footprint_rect(footprint_, x, z);
    current_ = {static_cast<i16>(at.x0), static_cast<i16>(at.z0)};
}

bool PathNavigator::can_occupy(Cell from, Cell to) const {
    if (!world_.terrain || !world_.grid) return false;
    // A longer step can't be answered by one cell: sweep it.
    if (manhattan(from, to) > 1)
        return cell_step_clear(footprint_, on_water_, *world_.terrain, *world_.grid, from, to);
    u8 caps = map_caps(footprint_, *world_.terrain, to.x, to.z);
    if (on_water_) caps = static_cast<u8>(caps & ~blueprints::occupancy::kSub);
    return footprint_fits(footprint_, *world_.terrain, *world_.grid, to.x, to.z, caps) != 0;
}

bool PathNavigator::can_transition(Cell from, Cell to) const {
    if (!world_.grid) return false;
    // Moho's CanPathCellTransition, which asks only about mobile units
    // (PathTransitionBlocked; the extended probe minds them all): on the
    // spot, whether one stands there; across, whether one stands between
    // the two cells' centres.
    if (!world_.blockers) return true;
    const i32 mode = extended_probe_ ? 2 : 1;
    if (from == to) return !world_.blockers->unit_blocked(world_.owner, from, mode);
    const f32 hx = static_cast<f32>(footprint_.size_x) * 0.5f;
    const f32 hz = static_cast<f32>(footprint_.size_z) * 0.5f;
    const WorldPoint a{static_cast<f32>(from.x) + hx, 0.0f, static_cast<f32>(from.z) + hz};
    const WorldPoint b{static_cast<f32>(to.x) + hx, 0.0f, static_cast<f32>(to.z) + hz};
    return !world_.blockers->swept_blocked(world_.owner, a, b, mode);
}

bool PathNavigator::can_reach_from_current(Cell to) const {
    // Moho's CanReachCellFromCurrent: no mobile unit across the way from
    // where the unit stands to the cell's centre (on the ground there: the
    // seabed for a footprint that takes it or on a dry map, else the
    // water's surface), then the cell's own transition.
    if (world_.blockers && world_.terrain) {
        const map::Terrain& t = *world_.terrain;
        const f32 x = static_cast<f32>(to.x) + static_cast<f32>(footprint_.size_x) * 0.5f;
        const f32 z = static_cast<f32>(to.z) + static_cast<f32>(footprint_.size_z) * 0.5f;
        const f32 ground = t.get_terrain_height(x, z);
        const bool seabed = (footprint_.caps & blueprints::occupancy::kSeabed) != 0;
        const f32 y = seabed || !t.has_water() ? ground : std::max(t.water_elevation(), ground);
        if (world_.blockers->swept_blocked(world_.owner, {unit_.x, unit_.y, unit_.z}, {x, y, z},
                                           extended_probe_ ? 2 : 1))
            return false;
    }
    return can_transition(to, to);
}

bool PathNavigator::update_forward_probe() {
    forward_probe_ = can_occupy(current_, current_) && can_reach_from_current(current_);
    return forward_probe_;
}

i32 PathNavigator::consume_prefix(i32 count) {
    const auto size = static_cast<i32>(path_.size());
    const i32 n = std::clamp(count, 0, size);
    path_.erase(path_.begin(), path_.begin() + n);
    if (path_.empty()) retry_delay_ = 0;
    const i32 updated = last_node_index_ - n;
    last_node_index_ = updated <= -1 ? -1 : updated;
    return updated;
}

i32 PathNavigator::direct_prefix_span() {
    if (path_.empty()) return 0;
    if (current_ == path_[0] && path_.size() > 1) consume_prefix(1);
    if (path_.empty()) return 0;
    const Cell first = path_[0];
    if (std::abs(first.x - current_.x) > 1 || std::abs(first.z - current_.z) > 1 ||
        !can_occupy(current_, first) || !can_reach_from_current(first))
        return 0;
    // The run of cells on, in the first step's direction, it can reach.
    const i32 hx = first.x - current_.x;
    const i32 hz = first.z - current_.z;
    i32 best = 0;
    for (size_t i = 1; i < path_.size(); ++i) {
        const Cell prev = path_[i - 1];
        const Cell cell = path_[i];
        if (cell.x - prev.x != hx || cell.z - prev.z != hz) break;
        if (!can_occupy(current_, cell) || !can_reach_from_current(cell)) break;
        best = static_cast<i32>(i);
    }
    return best;
}

void PathNavigator::set_target_point(i32 index) {
    consume_prefix(index);
    if (path_.empty()) return;
    target_ = path_[0];
    state_ = State::HasPath;
    retry_delay_ = 0;
    target_within_one_cell_ = manhattan(current_, target_) <= 1;
}

bool PathNavigator::try_advance_target_point() {
    const i32 first = forward_probe_ ? std::max(0, direct_prefix_span()) : 0;
    const auto size = static_cast<i32>(path_.size());
    if (size <= 0) return false;
    // The farthest of the next ten (or the straight run) under 50 cells off
    // it can slide straight to; with no forward probe, only the next one.
    const i32 furthest =
        forward_probe_ ? std::min(size - 1, std::max(10, first)) : std::min(size - 1, 1);
    i32 selected = -1;
    for (i32 i = furthest; i >= first; --i) {
        const Cell cand = path_[static_cast<size_t>(i)];
        if (i != first && cell_distance(current_, cand) >= 50.0f) continue;
        const bool occupy = can_occupy(current_, cand);
        const bool clear = manhattan(current_, cand) > 1 ? can_reach_from_current(cand)
                                                         : can_transition(cand, cand);
        if (occupy && clear) {
            forward_probe_ = true;
            selected = i;
            break;
        }
        if (!forward_probe_ && !repath_requested_) {
            selected = i;
            break;
        }
    }
    if (selected < first) {
        if (extended_probe_ && furthest > 0) {
            set_target_point(0);
            extended_probe_ = false;
            return true;
        }
        if (first > 0) consume_prefix(first - 1);
        return false;
    }
    if (selected == 0 && furthest > first && path_.size() > 1 &&
        !can_transition(path_[0], path_[1]) && cell_distance(current_, target_) < 10.0f) {
        consume_prefix(1);
        return false;
    }
    if (no_progress_ticks_ <= 30 || selected > 0) {
        set_target_point(selected);
        target_within_one_cell_ = false;
        return true;
    }
    return false;
}

void PathNavigator::queue_search(SearchType type) {
    finder_.prepare(world_, type, current_, unit_.x, unit_.z);
    if (queue_) queue_->queue(finder_);
    finder_.listen(this);
}

void PathNavigator::request_path(i32 mode) {
    request_mode_ = mode;
    last_blocked_cell_ = 0;
    retry_delay_ = 0;
    repath_requested_ = false;
    // (Moho's FAVORSWATER alt footprint choice is left to the unit.)
    finder_.set_goal(goal_);
    queue_search(search_type(mode));
    state_ = State::Searching;
    target_within_one_cell_ = false;
    update_forward_probe();
}

void PathNavigator::request_continuation(i32 mode) {
    if (path_.empty()) {
        reset();
        return;
    }
    if (current_ == path_[0]) {
        consume_prefix(1);
        if (path_.empty()) {
            reset();
            return;
        }
    }
    // The cells it is on or beside go.
    while (path_.size() > 1) {
        const Cell first = path_[0];
        if (!can_transition(first, first) || manhattan(first, current_) > 1) break;
        consume_prefix(1);
    }
    update_forward_probe();
    if (unit_.attacking) mode = 3;
    if (mode == 3) extended_probe_ = true;
    target_within_one_cell_ = false;
    if (path_.empty()) return;
    // The way from here to its next cell.
    finder_.set_goal(single_cell_goal(path_[0]));
    queue_search(search_type(mode));
    state_ = State::Continuing;
    retry_delay_ = 0;
}

void PathNavigator::update(const NavUnit& unit, const PathWorld& world, PathQueue& queue) {
    unit_ = unit;
    world_ = world;
    queue_ = &queue;
    set_current(unit.x, unit.z);
    if (request_countdown_ > 0) {
        if (--request_countdown_ == 0) request_path(request_mode_);
        target_ = current_;
        return;
    }
    if (retry_delay_ > 0) {
        if (--retry_delay_ == 0) request_continuation(2);
        return;
    }
    // Cells behind it go.
    while (path_.size() > 1) {
        const Cell first = path_[0];
        const Cell second = path_[1];
        if (cell_distance(current_, first) < cell_distance(current_, second) ||
            !can_reach_from_current(second))
            break;
        consume_prefix(1);
    }
    if (state_ != State::HasPath && state_ != State::FollowingLeader) {
        target_ = current_;
        return;
    }
    if (path_.empty()) {
        state_ = State::Failed;
        retry_delay_ = 0;
        target_ = current_;
        return;
    }
    if (current_ == path_.back()) {
        path_.clear();
        state_ = State::Idle;
        retry_delay_ = 0;
        target_ = current_;
        return;
    }
    if (!try_advance_target_point()) target_ = path_[0];
    // A new layer: the way on, asked again. (A new goal clears the layer it
    // last pathed on, so its first step with a path asks once.)
    if (unit.layer != last_layer_) {
        last_layer_ = unit.layer;
        request_continuation(2);
        return;
    }
    if (!unit.moved && !unit.immobile) ++no_progress_ticks_;
    else no_progress_ticks_ = 0;
    if (repath_threshold_ < cell_distance(current_, target_) || repath_requested_ ||
        no_progress_ticks_ > 30) {
        const Cell aimed = target_; // as it was before the advance
        if (try_advance_target_point()) {
            repath_threshold_ = cell_distance(current_, target_) * 0.5f;
            repath_requested_ = false;
            return;
        }
        // Stuck a second and a half: it stops there, as arrived.
        if (no_progress_ticks_ > 30) {
            path_.clear();
            state_ = State::Idle;
            retry_delay_ = 0;
            target_ = current_;
            return;
        }
        if (repath_requested_) {
            repath_requested_ = false;
            request_continuation(3);
            return;
        }
        // Making for its last cell with a unit standing there, it waits 10
        // ticks rather than ask each tick. FAF's exe reads the target cell
        // here (0x005AEB48, mTargetPos before TryAdvanceTargetPoint; faf-re's
        // text has the unit's own cell, which never gets this far), and
        // passes it as a world point, as is.
        if (!path_.empty() && aimed == path_.back() && !unit.waiting_for_transport &&
            world_.blockers &&
            world_.blockers->unit_blocked_at(world_.owner,
                                             static_cast<f32>(static_cast<u16>(aimed.x)),
                                             static_cast<f32>(static_cast<u16>(aimed.z)), 1)) {
            retry_delay_ = 10;
            return;
        }
        request_continuation(2);
    }
}

void PathNavigator::on_path_event(bool /*reached*/, const std::vector<Cell>& cells) {
    if (state_ == State::Searching) {
        path_ = cells;
        if (path_.empty()) {
            state_ = State::Failed;
            retry_delay_ = 0;
            return;
        }
        // Short of the goal: its centre goes on the end, if no unit holds it.
        if (!cell_in_goal(path_.back())) {
            const Cell centre = goal_centre(goal_);
            if (can_transition(centre, centre)) path_.push_back(centre);
        }
        search_fail_count_ = 0;
        retry_delay_ = 0;
        repath_threshold_ = kInfinity;
        state_ = State::HasPath;
        finder_.set_type(SearchType::None);
        return;
    }
    if (state_ != State::Continuing) return;

    const auto incoming = static_cast<i32>(cells.size());
    if (incoming == 0) {
        if (search_fail_count_ >= 3) {
            ++no_forward_fail_count_;
            if (no_forward_fail_count_ >= 3) {
                state_ = State::Failed;
                retry_delay_ = 0;
            } else {
                search_fail_count_ = 0;
                request_path(1);
            }
            return;
        }
        ++search_fail_count_;
        retry_delay_ = 10;
        return;
    }
    // It ends on the path's next cell: the way there goes in front.
    bool merged = false;
    if (!path_.empty() && cells.back() == path_[0]) {
        last_blocked_cell_ = 0;
        search_fail_count_ = 0;
        if (incoming > 1) path_.insert(path_.begin(), cells.begin(), cells.end() - 1);
        merged = true;
    }
    if (!merged) {
        if (!path_.empty() && incoming <= 2) {
            const Cell first = path_[0];
            if (can_transition(first, first)) {
                // Short of a cell it could stand on twice running: give up.
                const u32 packed = pack_cell(first);
                if (last_blocked_cell_ == packed) {
                    state_ = State::Failed;
                    retry_delay_ = 0;
                    search_fail_count_ = 0;
                    return;
                }
                last_blocked_cell_ = packed;
            } else {
                if (search_fail_count_ < 3) {
                    ++search_fail_count_;
                    retry_delay_ = 10;
                    return;
                }
                state_ = State::Failed;
                retry_delay_ = 0;
                search_fail_count_ = 0;
                return;
            }
        }
        path_.insert(path_.begin(), cells.begin(), cells.end());
    }
    no_forward_fail_count_ = 0;
    retry_delay_ = 0;
    repath_threshold_ = kInfinity;
    finder_.set_type(SearchType::None);
    if (last_node_index_ < 0) last_node_index_ = static_cast<i32>(path_.size()) - 1;
    else last_node_index_ += incoming - (merged ? 1 : 0);
    state_ = State::HasPath;
}

} // namespace osc::sim::path
