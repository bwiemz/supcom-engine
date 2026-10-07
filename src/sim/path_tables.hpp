#pragma once

// Moho's PathTables (roadmap item 4c, faf-re PathTables.cpp): one cluster
// map a footprint class, in spec order, all sharing one cache, each fed
// windows of where its class may stand.

#include "blueprints/footprint.hpp"
#include "core/types.hpp"
#include "sim/occupancy.hpp"
#include "sim/path_clusters.hpp"

#include <memory>
#include <vector>

namespace osc::map {
class Terrain;
}

namespace osc::sim {

/// Moho's OccupySourceBinding: one class's occupation windows, from the
/// map and what stands on it (OCCUPY_Filter, cell by cell).
class FootprintOccupation final : public path::OccupationSource {
public:
    FootprintOccupation(const blueprints::Footprint& fp, const map::Terrain& terrain,
                        const OccupancyGrid& grid);
    path::OccupationWindow window(i32 x, i32 z) const override;

private:
    blueprints::Footprint fp_;
    blueprints::Footprint cell_; ///< the class's tests, on one cell
    const map::Terrain& terrain_;
    const OccupancyGrid& grid_;
};

class PathTables {
public:
    /// For `terrain` and `grid`, which must outlive the tables.
    PathTables(const std::vector<blueprints::NamedFootprint>& classes, const map::Terrain& terrain,
               const OccupancyGrid& grid);
    PathTables(const PathTables&) = delete;
    PathTables& operator=(const PathTables&) = delete;

    size_t size() const { return maps_.size(); }
    /// Class `index`'s map (Moho's ClusterMapForFootprint).
    path::ClusterMap& map(size_t index) { return *maps_[index]; }
    const path::ClusterMap& map(size_t index) const { return *maps_[index]; }
    const path::ClusterCache& cache() const { return cache_; }

    /// Cells `r` changed what stands on them: dirty every class's clusters
    /// over them (Moho's DirtyClusters).
    void dirty(const OccupancyRect& r);
    /// Rebuild dirty clusters, the maps in class order spending one budget
    /// (Moho's UpdateBackground).
    void update_background(i32 budget);
    /// Every map caught up.
    bool done() const;

private:
    path::ClusterCache cache_;
    std::vector<std::unique_ptr<FootprintOccupation>> sources_;
    std::vector<std::unique_ptr<path::ClusterMap>> maps_;
};

} // namespace osc::sim
