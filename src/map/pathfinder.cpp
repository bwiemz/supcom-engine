#include "map/pathfinder.hpp"
#include "map/pathfinding_grid.hpp"

#include <algorithm>
#include <cmath>
#include <cfloat>
#include <functional>
#include <queue>
#include <spdlog/spdlog.h>

namespace osc::map {

static constexpr f32 SQRT2 = 1.41421356f;

/// How far a path looks for open ground from a blocked cell: around an
/// impassable goal (find_path), or out of the footprint a unit stands in
/// (astar, reachability).
static constexpr i32 NEAREST_PASSABLE_RADIUS = 20;

/// What a step across a footprint costs, leaving one, against open ground.
static constexpr f32 ESCAPE_COST = 10.0f;

/// The heuristic is weighted this little over the octile distance, so that
/// among the many cells of equal cost on open ground A* follows the ones
/// nearer the goal instead of flooding them all. A path is then at most 0.1%
/// longer than the shortest; a four-AI game's searches expand 9 times fewer
/// cells.
static constexpr f32 HEURISTIC_WEIGHT = 1.001f;

Pathfinder::Pathfinder(const PathfindingGrid& grid) : grid_(grid) {}

PathResult Pathfinder::find_path(f32 start_x, f32 start_z,
                                  f32 goal_x, f32 goal_z,
                                  const std::string& layer,
                                  f32 draft, bool amphibious) const {
    PathResult result;

    u32 sx, sz, gx, gz;
    grid_.world_to_grid(start_x, start_z, sx, sz);
    grid_.world_to_grid(goal_x, goal_z, gx, gz);

    // If start == goal (same cell), trivial path. It searches nothing, so it
    // spends none of the tick's budget: a unit parked on its patrol point
    // asks every tick, and must not starve everyone else's paths.
    if (sx == gx && sz == gz) {
        result.found = true;
        result.waypoints.push_back({goal_x, 0, goal_z});
        return result;
    }

    if (!can_pathfind()) {
        result.throttled = true;
        return result;
    }
    increment_request_count();

    // If goal cell is impassable, find nearest passable cell
    if (!grid_.is_passable_for(gx, gz, layer, draft, amphibious)) {
        // Spiral search outward from goal for nearest passable cell
        bool found_alt = false;
        for (u32 radius = 1; radius <= 20 && !found_alt; ++radius) {
            i32 igx = static_cast<i32>(gx);
            i32 igz = static_cast<i32>(gz);
            for (i32 dz = -static_cast<i32>(radius); dz <= static_cast<i32>(radius) && !found_alt; ++dz) {
                for (i32 dx = -static_cast<i32>(radius); dx <= static_cast<i32>(radius); ++dx) {
                    if (std::abs(dx) != static_cast<i32>(radius) &&
                        std::abs(dz) != static_cast<i32>(radius))
                        continue; // only check perimeter
                    i32 nx = igx + dx;
                    i32 nz = igz + dz;
                    if (nx < 0 || nz < 0) continue;
                    u32 ux = static_cast<u32>(nx);
                    u32 uz = static_cast<u32>(nz);
                    if (grid_.is_passable_for(ux, uz, layer, draft, amphibious)) {
                        gx = ux;
                        gz = uz;
                        // Update goal world position to cell center
                        grid_.grid_to_world(gx, gz, goal_x, goal_z);
                        found_alt = true;
                        break;
                    }
                }
            }
        }
        if (!found_alt) {
            spdlog::trace("Pathfinder: no passable cell near goal ({}, {})", goal_x, goal_z);
            return result; // found = false
        }
    }

    // A goal moved to the nearest open cell may be the one the unit is on:
    // it is as close as it gets.
    if (sx == gx && sz == gz) {
        result.found = true;
        result.partial = true;
        result.waypoints.push_back({goal_x, 0, goal_z});
        return result;
    }

    // Run A*. A unit that can't take a first step is closed in by buildings
    // (standing in one, or in a gap between them): it leaves across them.
    auto grid_path = astar(sx, sz, gx, gz, layer, draft, amphibious, false);
    if (grid_path.cells.empty() && grid_.get(sx, sz) != CellPassability::Obstacle)
        grid_path = astar(sx, sz, gx, gz, layer, draft, amphibious, true);
    if (grid_path.cells.empty()) {
        spdlog::trace("Pathfinder: A* found no path from ({},{}) to ({},{})", sx, sz, gx, gz);
        return result; // found = false
    }
    result.partial = !grid_path.reached_goal;

    // Smooth path
    auto smoothed = smooth_path(grid_path.cells, layer, draft, amphibious);

    // Convert to world coordinates
    result.found = true;
    for (size_t i = 0; i < smoothed.size(); ++i) {
        f32 wx, wz;
        grid_.grid_to_world(smoothed[i].first, smoothed[i].second, wx, wz);
        result.waypoints.push_back({wx, 0, wz});
    }

    // Replace last waypoint with exact goal position (a partial path ends at
    // the reachable cell, not at the unreachable goal)
    if (!result.waypoints.empty() && !result.partial) {
        result.waypoints.back().x = goal_x;
        result.waypoints.back().z = goal_z;
    }

    return result;
}

Pathfinder::GridPath Pathfinder::astar(u32 sx, u32 sz, u32 gx, u32 gz, const std::string& layer,
                                       f32 draft, bool amphibious, bool closed_in) const {
    const u32 w = grid_.grid_width();
    const u32 h = grid_.grid_height();
    const u32 total = w * h;
    const f32 cs = static_cast<f32>(grid_.cell_size());

    auto idx = [w](u32 x, u32 z) -> u32 { return z * w + x; };

    // A new stamp, instead of clearing the buffers (see seen_stamp_).
    if (seen_stamp_.size() != total) {
        g_cost_buf_.assign(total, FLT_MAX);
        parent_buf_.assign(total, UINT32_MAX);
        seen_stamp_.assign(total, 0);
        closed_stamp_.assign(total, 0);
        stamp_ = 0;
    }
    if (++stamp_ == 0) { // wrapped: old stamps could look current
        std::fill(seen_stamp_.begin(), seen_stamp_.end(), 0u);
        std::fill(closed_stamp_.begin(), closed_stamp_.end(), 0u);
        stamp_ = 1;
    }
    const u32 stamp = stamp_;
    auto g_cost = [&](u32 i) { return seen_stamp_[i] == stamp ? g_cost_buf_[i] : FLT_MAX; };
    auto reach = [&](u32 i, f32 g, u32 from) {
        g_cost_buf_[i] = g;
        parent_buf_[i] = from;
        seen_stamp_[i] = stamp;
    };
    auto closed = [&](u32 i) { return closed_stamp_[i] == stamp; };
    const PathfindingGrid::MoveClass mover = PathfindingGrid::classify(layer, draft, amphibious);

    // Priority queue: (f_cost, node_index)
    using PQEntry = std::pair<f32, u32>;
    std::priority_queue<PQEntry, std::vector<PQEntry>, std::greater<PQEntry>> open;

    // Octile heuristic
    auto heuristic = [&](u32 x, u32 z) -> f32 {
        f32 dx = static_cast<f32>(x > gx ? x - gx : gx - x);
        f32 dz = static_cast<f32>(z > gz ? z - gz : gz - z);
        f32 mn = std::min(dx, dz);
        f32 mx = std::max(dx, dz);
        return (mx + (SQRT2 - 1.0f) * mn) * cs * HEURISTIC_WEIGHT;
    };

    // A unit inside a structure's footprint -- a factory's new unit, a
    // builder its own buildings closed in -- or `closed_in` by them leaves
    // across them: the obstacle cells joined to its start count as open, out
    // to the radius reachability() looks for a way out. Only where the ground
    // beneath is open: a footprint over a cliff's foot doesn't lead up it.
    auto& escape = escape_buf_;
    const bool escaping = closed_in || grid_.get(sx, sz) == CellPassability::Obstacle;
    if (escaping) {
        escape.assign(total, 0);
        std::vector<u32> frontier{idx(sx, sz)};
        escape[idx(sx, sz)] = 1;
        while (!frontier.empty()) {
            const u32 cur = frontier.back();
            frontier.pop_back();
            const i32 cx = static_cast<i32>(cur % w);
            const i32 cz = static_cast<i32>(cur / w);
            for (const auto& d :
                 {std::pair{1, 0}, std::pair{-1, 0}, std::pair{0, 1}, std::pair{0, -1}}) {
                const i32 nx = cx + d.first;
                const i32 nz = cz + d.second;
                if (nx < 0 || nz < 0 || static_cast<u32>(nx) >= w || static_cast<u32>(nz) >= h)
                    continue;
                if (std::abs(nx - static_cast<i32>(sx)) > NEAREST_PASSABLE_RADIUS ||
                    std::abs(nz - static_cast<i32>(sz)) > NEAREST_PASSABLE_RADIUS)
                    continue;
                const u32 n = idx(static_cast<u32>(nx), static_cast<u32>(nz));
                const u32 ux = static_cast<u32>(nx);
                const u32 uz = static_cast<u32>(nz);
                if (escape[n] || grid_.get(ux, uz) != CellPassability::Obstacle ||
                    !grid_.terrain_passable_at(n, mover))
                    continue;
                escape[n] = 1;
                frontier.push_back(n);
            }
        }
    }
    auto passable = [&](u32 x, u32 z) {
        const u32 i = idx(x, z);
        return grid_.passable_at(i, mover) || (escaping && escape[i] != 0);
    };

    u32 start_idx = idx(sx, sz);
    reach(start_idx, 0, UINT32_MAX);
    open.push({heuristic(sx, sz), start_idx});

    // 8 directions: dx, dz pairs
    static constexpr i32 dirs[8][2] = {
        {1, 0}, {-1, 0}, {0, 1}, {0, -1},
        {1, 1}, {1, -1}, {-1, 1}, {-1, -1}
    };

    u32 nodes_explored = 0;
    u32 goal_idx = idx(gx, gz);
    // Closest explored cell to the goal (by heuristic, then by path cost),
    // the fallback destination when the goal cannot be reached.
    u32 best_idx = start_idx;
    f32 best_h = heuristic(sx, sz);

    while (!open.empty()) {
        auto [f, cur_idx] = open.top();
        open.pop();

        if (cur_idx == goal_idx) break; // found path

        if (closed(cur_idx)) continue;
        closed_stamp_[cur_idx] = stamp;

        {
            const f32 h_cur = heuristic(cur_idx % w, cur_idx / w);
            if (h_cur < best_h || (h_cur == best_h && g_cost(cur_idx) < g_cost(best_idx))) {
                best_h = h_cur;
                best_idx = cur_idx;
            }
        }

        if (++nodes_explored > MAX_NODES_EXPLORED) {
            spdlog::trace("Pathfinder: A* hit search limit ({} nodes)", MAX_NODES_EXPLORED);
            break; // fall through to the closest cell found so far
        }

        u32 cx = cur_idx % w;
        u32 cz = cur_idx / w;

        for (auto& dir : dirs) {
            i32 nx = static_cast<i32>(cx) + dir[0];
            i32 nz = static_cast<i32>(cz) + dir[1];
            if (nx < 0 || nz < 0 || static_cast<u32>(nx) >= w || static_cast<u32>(nz) >= h)
                continue;

            u32 unx = static_cast<u32>(nx);
            u32 unz = static_cast<u32>(nz);
            u32 n_idx = idx(unx, unz);

            if (closed(n_idx)) continue;
            if (!passable(unx, unz)) continue;

            // Diagonal: also check that both cardinal neighbors are passable
            // (prevent cutting corners through walls)
            if (dir[0] != 0 && dir[1] != 0) {
                i32 card_x_i = static_cast<i32>(cx) + dir[0];
                i32 card_z_i = static_cast<i32>(cz) + dir[1];
                if (card_x_i < 0 || static_cast<u32>(card_x_i) >= w ||
                    card_z_i < 0 || static_cast<u32>(card_z_i) >= h)
                    continue;
                u32 card_x = static_cast<u32>(card_x_i);
                u32 card_z = static_cast<u32>(card_z_i);
                if (!passable(card_x, cz) || !passable(cx, card_z)) continue;
            }

            bool diagonal = (dir[0] != 0 && dir[1] != 0);
            f32 move_cost = diagonal ? SQRT2 * cs : cs;
            // Blocked ground is left by the shortest way: across the unit's
            // own footprint, not through the buildings packed beside it
            // (whose cells the escape can't tell from its own).
            if (escaping && escape[n_idx] != 0) move_cost *= ESCAPE_COST;
            f32 new_g = g_cost(cur_idx) + move_cost;

            if (new_g < g_cost(n_idx)) {
                reach(n_idx, new_g, cur_idx);
                f32 f_new = new_g + heuristic(unx, unz);
                open.push({f_new, n_idx});
            }
        }
    }

    last_nodes_explored_ = nodes_explored;

    // Reconstruct the path to the goal, or else to the closest cell reached.
    GridPath result;
    result.reached_goal = g_cost(goal_idx) != FLT_MAX;
    const u32 end_idx = result.reached_goal ? goal_idx : best_idx;
    if (end_idx == start_idx) return result; // nowhere to go

    u32 cur = end_idx;
    while (cur != UINT32_MAX) {
        result.cells.push_back({cur % w, cur / w});
        cur = parent_buf_[cur];
    }
    std::reverse(result.cells.begin(), result.cells.end());
    return result;
}

std::vector<std::pair<u32, u32>> Pathfinder::smooth_path(
    const std::vector<std::pair<u32, u32>>& path,
    const std::string& layer, f32 draft, bool amphibious) const {
    if (path.size() <= 2) return path;

    std::vector<std::pair<u32, u32>> smoothed;
    smoothed.push_back(path.front());

    size_t current = 0;
    while (current < path.size() - 1) {
        // Find farthest visible waypoint from current
        size_t farthest = current + 1;
        for (size_t i = current + 2; i < path.size(); ++i) {
            if (has_line_of_sight(path[current].first, path[current].second,
                                  path[i].first, path[i].second, layer,
                                  draft, amphibious)) {
                farthest = i;
            }
        }
        smoothed.push_back(path[farthest]);
        current = farthest;
    }

    return smoothed;
}

bool Pathfinder::has_line_of_sight(u32 x0, u32 z0, u32 x1, u32 z1,
                                    const std::string& layer,
                                    f32 draft, bool amphibious) const {
    // Bresenham's line algorithm
    i32 dx = static_cast<i32>(x1) - static_cast<i32>(x0);
    i32 dz = static_cast<i32>(z1) - static_cast<i32>(z0);
    i32 sx = dx > 0 ? 1 : (dx < 0 ? -1 : 0);
    i32 sz = dz > 0 ? 1 : (dz < 0 ? -1 : 0);
    dx = std::abs(dx);
    dz = std::abs(dz);

    i32 x = static_cast<i32>(x0);
    i32 z = static_cast<i32>(z0);
    const PathfindingGrid::MoveClass mover = PathfindingGrid::classify(layer, draft, amphibious);
    const u32 w = grid_.grid_width();
    const auto open_at = [&](i32 cx, i32 cz) {
        return cx >= 0 && cz >= 0 && static_cast<u32>(cx) < w &&
               static_cast<u32>(cz) < grid_.grid_height() &&
               grid_.passable_at(static_cast<u32>(cz) * w + static_cast<u32>(cx), mover);
    };

    if (dx >= dz) {
        i32 err = dx / 2;
        for (i32 i = 0; i <= dx; ++i) {
            if (!open_at(x, z)) return false;
            err -= dz;
            if (err < 0) {
                z += sz;
                err += dx;
            }
            x += sx;
        }
    } else {
        i32 err = dz / 2;
        for (i32 i = 0; i <= dz; ++i) {
            if (!open_at(x, z)) return false;
            err -= dx;
            if (err < 0) {
                x += sx;
                err += dz;
            }
            z += sz;
        }
    }
    return true;
}

// --- Reachability ----------------------------------------------------------

namespace {

bool is_naval_layer(const std::string& layer) {
    return layer == "Water" || layer == "Seabed" || layer == "Sub";
}

/// Visit the cells at Chebyshev distance exactly `r` from (cx, cz) that lie
/// inside a w x h grid.
template <typename Fn>
void for_ring(i32 cx, i32 cz, i32 r, u32 w, u32 h, Fn&& fn) {
    auto visit = [&](i32 x, i32 z) {
        if (x >= 0 && z >= 0 && static_cast<u32>(x) < w && static_cast<u32>(z) < h)
            fn(static_cast<u32>(x), static_cast<u32>(z));
    };
    if (r == 0) {
        visit(cx, cz);
        return;
    }
    for (i32 dx = -r; dx <= r; ++dx) {
        visit(cx + dx, cz - r);
        visit(cx + dx, cz + r);
    }
    for (i32 dz = -r + 1; dz <= r - 1; ++dz) {
        visit(cx - r, cz + dz);
        visit(cx + r, cz + dz);
    }
}


} // namespace

const Pathfinder::ComponentLabels& Pathfinder::labels_for(
    const std::string& layer, f32 draft, bool amphibious) const {
    const bool naval = !amphibious && is_naval_layer(layer);
    const f32 key_draft =
        naval && draft > 0 ? std::ceil(draft / DRAFT_KEY_STEP) * DRAFT_KEY_STEP : 0.0f;

    ComponentLabels* set = nullptr;
    for (auto& s : label_cache_) {
        if (s.amphibious == amphibious && s.naval == naval && s.draft == key_draft) {
            set = &s;
            break;
        }
    }
    if (!set) {
        if (label_cache_.size() < MAX_LABEL_SETS) {
            set = &label_cache_.emplace_back();
        } else {
            set = &*std::min_element(label_cache_.begin(), label_cache_.end(),
                                     [](const ComponentLabels& a, const ComponentLabels& b) {
                                         return a.last_used < b.last_used;
                                     });
        }
        set->amphibious = amphibious;
        set->naval = naval;
        set->draft = key_draft;
        set->labels.clear(); // built below
    }
    set->last_used = ++label_uses_;
    if (set->labels.empty() || set->grid_version != grid_.version()) build_labels(*set);
    return *set;
}

void Pathfinder::build_labels(ComponentLabels& set) const {
    static const std::string kLand = "Land";
    static const std::string kWater = "Water";
    const std::string& layer = set.naval ? kWater : kLand;
    const u32 w = grid_.grid_width();
    const u32 h = grid_.grid_height();
    const PathfindingGrid::MoveClass mover =
        PathfindingGrid::classify(layer, set.draft, set.amphibious);
    auto passable = [&](u32 x, u32 z) { return grid_.passable_at(z * w + x, mover); };

    set.labels.assign(static_cast<size_t>(w) * h, 0);
    std::vector<u32> stack;
    u32 next_label = 0;
    for (u32 z = 0; z < h; ++z) {
        for (u32 x = 0; x < w; ++x) {
            const u32 seed = z * w + x;
            if (set.labels[seed] != 0 || !passable(x, z)) continue;
            set.labels[seed] = ++next_label;
            stack.push_back(seed);
            while (!stack.empty()) {
                const u32 cur = stack.back();
                stack.pop_back();
                const u32 cx = cur % w;
                const u32 cz = cur / w;
                auto spread = [&](u32 nx, u32 nz) {
                    const u32 n = nz * w + nx;
                    if (set.labels[n] == 0 && passable(nx, nz)) {
                        set.labels[n] = next_label;
                        stack.push_back(n);
                    }
                };
                if (cx > 0) spread(cx - 1, cz);
                if (cx + 1 < w) spread(cx + 1, cz);
                if (cz > 0) spread(cx, cz - 1);
                if (cz + 1 < h) spread(cx, cz + 1);
            }
        }
    }
    set.grid_version = grid_.version();
}

Reachability Pathfinder::reachability(f32 start_x, f32 start_z,
                                      f32 goal_x, f32 goal_z,
                                      const std::string& layer,
                                      f32 draft, bool amphibious) const {
    Reachability r;
    r.best_x = start_x;
    r.best_z = start_z;

    u32 sx, sz, gx, gz;
    grid_.world_to_grid(start_x, start_z, sx, sz);
    grid_.world_to_grid(goal_x, goal_z, gx, gz);
    if (layer == "Air" || (sx == gx && sz == gz)) {
        r.reachable = true;
        r.best_x = goal_x;
        r.best_z = goal_z;
        return r;
    }

    const auto& set = labels_for(layer, draft, amphibious);
    const u32 w = grid_.grid_width();
    const u32 h = grid_.grid_height();

    // Components on the nearest ring around (cx, cz) that has any passable
    // cell: the cell's own component when it is passable.
    auto nearest_components = [&](u32 cx, u32 cz) {
        std::vector<u32> found;
        for (i32 radius = 0; radius <= NEAREST_PASSABLE_RADIUS && found.empty(); ++radius) {
            for_ring(static_cast<i32>(cx), static_cast<i32>(cz), radius, w, h,
                     [&](u32 x, u32 z) {
                         const u32 label = set.labels[z * w + x];
                         if (label != 0 &&
                             std::find(found.begin(), found.end(), label) == found.end())
                             found.push_back(label);
                     });
        }
        return found;
    };

    // A unit standing on an obstacle (inside a factory footprint) leaves by
    // the nearest passable cells, as A* expands from an impassable start.
    const std::vector<u32> from = nearest_components(sx, sz);
    if (from.empty()) return r; // enclosed: nothing is reachable
    auto is_from = [&](u32 label) {
        return label != 0 && std::find(from.begin(), from.end(), label) != from.end();
    };

    for (u32 label : nearest_components(gx, gz)) {
        if (is_from(label)) {
            r.reachable = true;
            r.best_x = goal_x;
            r.best_z = goal_z;
            return r;
        }
    }

    // Unreachable: the closest cell the unit can get to instead, if one is
    // near the goal; else the start.
    for (i32 radius = 1; radius <= BEST_POINT_SEARCH_RADIUS; ++radius) {
        f32 best_d2 = FLT_MAX;
        for_ring(static_cast<i32>(gx), static_cast<i32>(gz), radius, w, h,
                 [&](u32 x, u32 z) {
                     if (!is_from(set.labels[z * w + x])) return;
                     f32 wx, wz;
                     grid_.grid_to_world(x, z, wx, wz);
                     const f32 d2 = (wx - goal_x) * (wx - goal_x) + (wz - goal_z) * (wz - goal_z);
                     if (d2 < best_d2) {
                         best_d2 = d2;
                         r.best_x = wx;
                         r.best_z = wz;
                     }
                 });
        if (best_d2 != FLT_MAX) break;
    }
    return r;
}

} // namespace osc::map
