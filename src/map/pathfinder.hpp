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
    std::vector<sim::Vector3> waypoints; // world-space positions
};

class Pathfinder {
public:
    explicit Pathfinder(const PathfindingGrid& grid);

    /// Find a path from start to goal for the given movement layer.
    /// Returns smoothed waypoints in world coordinates.
    PathResult find_path(f32 start_x, f32 start_z,
                         f32 goal_x, f32 goal_z,
                         const std::string& layer) const;

private:
    /// Raw A* on the grid. Returns grid cell path (start→goal).
    std::vector<std::pair<u32, u32>> astar(
        u32 sx, u32 sz, u32 gx, u32 gz,
        const std::string& layer) const;

    /// Smooth path by removing redundant waypoints via line-of-sight.
    std::vector<std::pair<u32, u32>> smooth_path(
        const std::vector<std::pair<u32, u32>>& path,
        const std::string& layer) const;

    /// Line-of-sight check on the passability grid (Bresenham).
    bool has_line_of_sight(u32 x0, u32 z0, u32 x1, u32 z1,
                           const std::string& layer) const;

    const PathfindingGrid& grid_;

    static constexpr u32 MAX_NODES_EXPLORED = 50000;
};

} // namespace osc::map
