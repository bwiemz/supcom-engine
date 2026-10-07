#pragma once

// Moho's path clusters (roadmap item 4c, faf-re Cluster.cpp, ClusterMap.h,
// BitArray2D.cpp): the graph its pathfinder searches between a path's ends.
// A footprint class's map is cut into 8 x 8 cell clusters, each 4 x 4 of
// those into a 32 x 32 one; a cluster keeps only its portals (one where each
// open run crosses its edge) and the cost between each pair inside it,
// quantised. Identical clusters share one payload, across every class's map.
// See docs/plans/2026-10-07-per-class-pathing-design.md.

#include "core/types.hpp"
#include "sim/occupancy.hpp" // OccupancyRect

#include <array>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace osc::sim::path {

/// Cells a cluster spans by level (Moho's sClusterSize); level 0 is a cell.
/// Moho's maps are two levels deep.
constexpr u32 kClusterLevels = 2;
constexpr std::array<i32, kClusterLevels + 1> kClusterSize = {1, 8, 32};
constexpr std::array<i32, kClusterLevels + 1> kClusterShift = {0, 3, 5};
/// What building or fetching one cluster costs a budget (WorkOnCluster).
constexpr i32 kClusterCost = 10;
/// The sim's background budget a tick (path_BackgroundBudget).
constexpr i32 kBackgroundBudget = 1000;

/// Moho's OccupationData: a level-1 cluster's 9 x 9 window, its own 8 x 8
/// cells and the far boundary lines it shares with its neighbours. Bit x of
/// row z is set when the class may stand with its corner on that cell.
struct OccupationWindow {
    static constexpr i32 kSpan = 9;
    std::array<u16, kSpan> rows{};

    bool open(i32 x, i32 z) const { return ((rows[static_cast<size_t>(z)] >> x) & 1U) != 0; }
    bool operator==(const OccupationWindow& o) const { return rows == o.rows; }
};

/// A portal, in its cluster's cells (0 to the cluster's size, inclusive).
struct ClusterNode {
    u8 x = 0;
    u8 z = 0;
    bool operator==(const ClusterNode& o) const { return x == o.x && z == o.z; }
};

/// Moho's Cluster::Data: a cluster's portals, on its boundary lines and in
/// the order of `x | z << 8`, and the quantised cost between each pair
/// inside it.
struct Cluster {
    std::vector<ClusterNode> nodes;
    /// Pair (s, t) at pair_index(s, t); -1 where s can't reach t inside.
    std::vector<i8> edges;
    /// The cache's name for this payload; never reused.
    u64 serial = 0;

    /// Moho's triangular layout: (s < t) at s + t(t - 1) / 2.
    static u32 pair_index(u32 a, u32 b) {
        if (a > b) std::swap(a, b);
        return a + ((b * (b - 1)) >> 1);
    }
    i8 edge(u32 a, u32 b) const { return edges[pair_index(a, b)]; }
    bool same_content(const Cluster& o) const { return nodes == o.nodes && edges == o.edges; }
};
using ClusterRef = std::shared_ptr<const Cluster>;

/// The octile distance between two cells, max + 0.41421354 min.
f32 octile_distance(i32 dx, i32 dz);
/// Moho's QuantizeEdgeCost: ceil(6 ln(cost / octile)) clamped to 0-31,
/// found as the least bucket whose scale reaches the ratio.
i8 quantize_edge_cost(f32 cost, f32 octile);
/// Moho's DequantizeEdgeCost: octile x exp(bucket / 6), from its table.
f32 dequantize_edge_cost(i32 bucket, f32 octile);

/// Moho's ClusterBuild(OccupationData): a level-1 cluster from its window.
Cluster build_cluster(const OccupationWindow& window);
/// Moho's ClusterBuild(SubclusterData): a cluster from its 4 x 4 children
/// (x + 4z), the clusters of level `child_level`; a null child is empty.
Cluster build_cluster(const std::array<const Cluster*, 16>& children, u32 child_level);

/// Moho's ClusterCache: built clusters by what they were built from, held
/// while any map holds them.
class ClusterCache {
public:
    ClusterRef fetch(const OccupationWindow& window);
    ClusterRef fetch(const std::array<ClusterRef, 16>& children, u32 child_level);
    /// Payloads still held (tests).
    size_t live_payloads() const;
    /// Clusters built rather than found (tests).
    u64 builds() const { return builds_; }

private:
    struct WindowHash {
        size_t operator()(const OccupationWindow& w) const;
    };
    struct ChildrenKey {
        u32 level = 0;
        std::array<u64, 16> serials{};
        bool operator==(const ChildrenKey& o) const {
            return level == o.level && serials == o.serials;
        }
    };
    struct ChildrenHash {
        size_t operator()(const ChildrenKey& k) const;
    };

    ClusterRef keep(Cluster cluster);
    /// Drop the dead entries once a table reaches `at`, then wait for it to
    /// double.
    template <typename Map> static void sweep(Map& entries, size_t& at);

    std::unordered_map<OccupationWindow, std::weak_ptr<const Cluster>, WindowHash> by_window_;
    std::unordered_map<ChildrenKey, std::weak_ptr<const Cluster>, ChildrenHash> by_children_;
    size_t sweep_windows_at_ = 1024;
    size_t sweep_children_at_ = 1024;
    u64 next_serial_ = 1;
    u64 builds_ = 0;
};

/// Moho's IOccupationSource: a class's window for the level-1 cluster whose
/// corner is cell (x, z).
class OccupationSource {
public:
    virtual ~OccupationSource() = default;
    virtual OccupationWindow window(i32 x, i32 z) const = 0;
};

/// Moho's gpg::BitArray2D: bit (x, z) in word x + (z / 32) * width, bit
/// z % 32, so a scan finds the dirty clusters in Moho's order.
class BitGrid {
public:
    BitGrid() = default;
    BitGrid(i32 width, i32 height);
    i32 width() const { return width_; }
    i32 height() const { return height_; }
    bool test(i32 x, i32 z) const;
    void set(i32 x, i32 z, bool on);
    /// Clipped to the grid.
    void fill(const OccupancyRect& r, bool on);
    /// Moho's AnyBitSet: from word `progress` on, wrapping, the first set
    /// bit (the lowest in its word); `progress` stays on that word.
    bool find_set(u32& progress, i32& x, i32& z) const;
    bool any() const;

private:
    i32 width_ = 0;
    i32 height_ = 0;
    std::vector<u32> words_;
};

/// Moho's gpg::HaStar::ClusterMap: one class's clusters, levels 1 and 2,
/// each rebuilt when dirty as a search or the background work reaches it.
class ClusterMap {
public:
    /// A map of `width` x `height` cells, rounded up to whole top-level
    /// clusters, for a class `size_x` x `size_z`. Every cluster starts dirty.
    ClusterMap(const OccupationSource& source, ClusterCache& cache, u32 width, u32 height,
               i32 size_x, i32 size_z);

    /// Cells, rounded up to whole top-level clusters.
    i32 width() const { return width_; }
    i32 height() const { return height_; }
    /// Clusters across and down at `level`.
    i32 clusters_x(u32 level) const { return width_ >> kClusterShift[level]; }
    i32 clusters_z(u32 level) const { return height_ >> kClusterShift[level]; }

    /// Cells [r.x0, r.x1) x [r.z0, r.z1) changed: dirty every cluster whose
    /// window reads one (Moho's DirtyRect; see the design doc for its pad).
    void dirty_rect(const OccupancyRect& r);
    /// Moho's WorkOnCluster: make cluster (cx, cz) of `level` current,
    /// children first, at kClusterCost each from `budget`. False, with the
    /// cluster still dirty, when the budget ran out first.
    bool work_on_cluster(i32 cx, i32 cz, u32 level, i32& budget);
    /// Moho's BackgroundWork: rebuild dirty top-level clusters, scanning on
    /// from where the last call stopped, until done or out of budget.
    void background_work(i32& budget);

    /// The cluster as last built (null before its first build).
    const Cluster* cluster(u32 level, i32 cx, i32 cz) const {
        return levels_[level][index(level, cx, cz)].get();
    }
    /// A hold on it, kept across further builds.
    ClusterRef cluster_ref(u32 level, i32 cx, i32 cz) const {
        return levels_[level][index(level, cx, cz)];
    }
    /// Moho's ClusterRect: the world rect of the `level` cluster(s) holding
    /// cell (x, z), a cell beyond on the far sides (both clusters for a
    /// cell on a line between them); at level 0, the 3 x 3 around it.
    OccupancyRect cluster_rect(i32 x, i32 z, u32 level) const;
    /// Moho's ClusterIndexRect: the `level` clusters holding cell (x, z) (two
    /// for a cell on a line between them, four at a corner).
    OccupancyRect cluster_index_rect(i32 x, i32 z, u32 level) const;
    bool dirty(u32 level, i32 cx, i32 cz) const { return dirty_[level].test(cx, cz); }
    /// Nothing dirty since the background work last looked.
    bool done() const { return done_; }

private:
    size_t index(u32 level, i32 cx, i32 cz) const {
        return static_cast<size_t>(cz) * static_cast<size_t>(clusters_x(level)) +
               static_cast<size_t>(cx);
    }

    const OccupationSource& source_;
    ClusterCache& cache_;
    i32 width_ = 0;
    i32 height_ = 0;
    i32 size_x_ = 1;
    i32 size_z_ = 1;
    std::array<std::vector<ClusterRef>, kClusterLevels + 1> levels_; ///< [0] unused
    std::array<BitGrid, kClusterLevels + 1> dirty_;
    u32 progress_ = 0;
    bool done_ = false;
};

} // namespace osc::sim::path
