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

Pathfinder::Pathfinder(const PathfindingGrid& grid) : grid_(grid) {}

PathResult Pathfinder::find_path(f32 start_x, f32 start_z,
                                  f32 goal_x, f32 goal_z,
                                  const std::string& layer) const {
    PathResult result;

    u32 sx, sz, gx, gz;
    grid_.world_to_grid(start_x, start_z, sx, sz);
    grid_.world_to_grid(goal_x, goal_z, gx, gz);

    // If start == goal (same cell), trivial path
    if (sx == gx && sz == gz) {
        result.found = true;
        result.waypoints.push_back({goal_x, 0, goal_z});
        return result;
    }

    // If goal cell is impassable, find nearest passable cell
    if (!grid_.is_passable_for(gx, gz, layer)) {
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
                    if (grid_.is_passable_for(ux, uz, layer)) {
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
            spdlog::debug("Pathfinder: no passable cell near goal ({}, {})", goal_x, goal_z);
            return result; // found = false
        }
    }

    // Run A*
    auto grid_path = astar(sx, sz, gx, gz, layer);
    if (grid_path.empty()) {
        spdlog::debug("Pathfinder: A* found no path from ({},{}) to ({},{})",
                       sx, sz, gx, gz);
        return result; // found = false
    }

    // Smooth path
    auto smoothed = smooth_path(grid_path, layer);

    // Convert to world coordinates
    result.found = true;
    for (size_t i = 0; i < smoothed.size(); ++i) {
        f32 wx, wz;
        grid_.grid_to_world(smoothed[i].first, smoothed[i].second, wx, wz);
        result.waypoints.push_back({wx, 0, wz});
    }

    // Replace last waypoint with exact goal position
    if (!result.waypoints.empty()) {
        result.waypoints.back().x = goal_x;
        result.waypoints.back().z = goal_z;
    }

    return result;
}

std::vector<std::pair<u32, u32>> Pathfinder::astar(
    u32 sx, u32 sz, u32 gx, u32 gz,
    const std::string& layer) const {

    const u32 w = grid_.grid_width();
    const u32 h = grid_.grid_height();
    const u32 total = w * h;
    const f32 cs = static_cast<f32>(grid_.cell_size());

    auto idx = [w](u32 x, u32 z) -> u32 { return z * w + x; };

    // g_cost and parent arrays
    std::vector<f32> g_cost(total, FLT_MAX);
    std::vector<u32> parent(total, UINT32_MAX);
    std::vector<bool> closed(total, false);

    // Priority queue: (f_cost, node_index)
    using PQEntry = std::pair<f32, u32>;
    std::priority_queue<PQEntry, std::vector<PQEntry>, std::greater<PQEntry>> open;

    // Octile heuristic
    auto heuristic = [&](u32 x, u32 z) -> f32 {
        f32 dx = static_cast<f32>(x > gx ? x - gx : gx - x);
        f32 dz = static_cast<f32>(z > gz ? z - gz : gz - z);
        f32 mn = std::min(dx, dz);
        f32 mx = std::max(dx, dz);
        return (mx + (SQRT2 - 1.0f) * mn) * cs;
    };

    u32 start_idx = idx(sx, sz);
    g_cost[start_idx] = 0;
    open.push({heuristic(sx, sz), start_idx});

    // 8 directions: dx, dz pairs
    static constexpr i32 dirs[8][2] = {
        {1, 0}, {-1, 0}, {0, 1}, {0, -1},
        {1, 1}, {1, -1}, {-1, 1}, {-1, -1}
    };

    u32 nodes_explored = 0;
    u32 goal_idx = idx(gx, gz);

    while (!open.empty()) {
        auto [f, cur_idx] = open.top();
        open.pop();

        if (cur_idx == goal_idx) break; // found path

        if (closed[cur_idx]) continue;
        closed[cur_idx] = true;

        if (++nodes_explored > MAX_NODES_EXPLORED) {
            spdlog::debug("Pathfinder: A* hit search limit ({} nodes)", MAX_NODES_EXPLORED);
            return {}; // give up
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

            if (closed[n_idx]) continue;
            if (!grid_.is_passable_for(unx, unz, layer)) continue;

            // Diagonal: also check that both cardinal neighbors are passable
            // (prevent cutting corners through walls)
            if (dir[0] != 0 && dir[1] != 0) {
                u32 card_x = static_cast<u32>(static_cast<i32>(cx) + dir[0]);
                u32 card_z = static_cast<u32>(static_cast<i32>(cz) + dir[1]);
                if (!grid_.is_passable_for(card_x, cz, layer) ||
                    !grid_.is_passable_for(cx, card_z, layer))
                    continue;
            }

            bool diagonal = (dir[0] != 0 && dir[1] != 0);
            f32 move_cost = diagonal ? SQRT2 * cs : cs;
            f32 new_g = g_cost[cur_idx] + move_cost;

            if (new_g < g_cost[n_idx]) {
                g_cost[n_idx] = new_g;
                parent[n_idx] = cur_idx;
                f32 f_new = new_g + heuristic(unx, unz);
                open.push({f_new, n_idx});
            }
        }
    }

    // Reconstruct path
    if (g_cost[goal_idx] == FLT_MAX) return {}; // no path

    std::vector<std::pair<u32, u32>> path;
    u32 cur = goal_idx;
    while (cur != UINT32_MAX) {
        path.push_back({cur % w, cur / w});
        cur = parent[cur];
    }
    std::reverse(path.begin(), path.end());
    return path;
}

std::vector<std::pair<u32, u32>> Pathfinder::smooth_path(
    const std::vector<std::pair<u32, u32>>& path,
    const std::string& layer) const {
    if (path.size() <= 2) return path;

    std::vector<std::pair<u32, u32>> smoothed;
    smoothed.push_back(path.front());

    size_t current = 0;
    while (current < path.size() - 1) {
        // Find farthest visible waypoint from current
        size_t farthest = current + 1;
        for (size_t i = current + 2; i < path.size(); ++i) {
            if (has_line_of_sight(path[current].first, path[current].second,
                                  path[i].first, path[i].second, layer)) {
                farthest = i;
            }
        }
        smoothed.push_back(path[farthest]);
        current = farthest;
    }

    return smoothed;
}

bool Pathfinder::has_line_of_sight(u32 x0, u32 z0, u32 x1, u32 z1,
                                    const std::string& layer) const {
    // Bresenham's line algorithm
    i32 dx = static_cast<i32>(x1) - static_cast<i32>(x0);
    i32 dz = static_cast<i32>(z1) - static_cast<i32>(z0);
    i32 sx = dx > 0 ? 1 : (dx < 0 ? -1 : 0);
    i32 sz = dz > 0 ? 1 : (dz < 0 ? -1 : 0);
    dx = std::abs(dx);
    dz = std::abs(dz);

    i32 x = static_cast<i32>(x0);
    i32 z = static_cast<i32>(z0);

    if (dx >= dz) {
        i32 err = dx / 2;
        for (i32 i = 0; i <= dx; ++i) {
            if (!grid_.is_passable_for(static_cast<u32>(x), static_cast<u32>(z), layer))
                return false;
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
            if (!grid_.is_passable_for(static_cast<u32>(x), static_cast<u32>(z), layer))
                return false;
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

} // namespace osc::map
