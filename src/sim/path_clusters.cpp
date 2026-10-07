#include "sim/path_clusters.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <functional>
#include <queue>

namespace osc::sim::path {

namespace {

constexpr f32 kDiagonalStep = 1.41421354f;
constexpr f32 kOctileFactor = 0.41421354f;

/// exp(q / 6) for q in 0-31: Moho's table (0x00E35DA0), the bits both ways
/// through a bucket on every platform.
constexpr std::array<f32, 32> kBucketScale = {
    1.0f,        1.18136041f, 1.39561241f, 1.64872122f, 1.94773400f, 2.30097604f, 2.71828175f,
    3.21127081f, 3.79366802f, 4.48168898f, 5.29449005f, 6.25470114f, 7.38905621f, 8.72913837f,
    10.3122587f, 12.1824942f, 14.3919163f, 17.0020409f, 20.0855370f, 23.7282581f, 28.0316257f,
    33.1154518f, 39.1212845f, 46.2163353f, 54.5981483f, 64.5000916f, 76.1978607f, 90.0171280f,
    106.342674f, 125.629028f, 148.413162f, 175.329437f,
};

// The edge search's neighbours in Moho's order (0x00D49430-0x00D49460): the
// four sides, then the diagonals, each only once both of its sides proved
// open in this expansion.
constexpr std::array<i32, 8> kStepZ = {-1, 0, 1, 0, -1, 1, 1, -1};
constexpr std::array<i32, 8> kStepX = {0, -1, 0, 1, -1, -1, 1, 1};
constexpr std::array<u32, 8> kSidesNeeded = {0, 0, 0, 0, 3, 6, 12, 9};
constexpr std::array<f32, 8> kStepCost = {
    1.0f, 1.0f, 1.0f, 1.0f, kDiagonalStep, kDiagonalStep, kDiagonalStep, kDiagonalStep,
};

/// Cell (x, z) of a grid `width` across, row by row.
size_t grid_index(i32 x, i32 z, i32 width) {
    return static_cast<size_t>(z) * static_cast<size_t>(width) + static_cast<size_t>(x);
}

u16 packed(ClusterNode n) {
    return static_cast<u16>(n.x | (n.z << 8));
}

/// Sorted by `x | z << 8`, each once (Moho's sort + unique).
void sort_nodes(std::vector<ClusterNode>& nodes) {
    std::sort(nodes.begin(), nodes.end(),
              [](ClusterNode a, ClusterNode b) { return packed(a) < packed(b); });
    nodes.erase(std::unique(nodes.begin(), nodes.end()), nodes.end());
}

f32 node_octile(ClusterNode a, ClusterNode b) {
    return octile_distance(static_cast<i32>(a.x) - static_cast<i32>(b.x),
                           static_cast<i32>(a.z) - static_cast<i32>(b.z));
}

/// Moho's EraseUnconnectedNodes: the nodes with no edge go, and their rows
/// and columns of the triangle with them.
void erase_unconnected(Cluster& c) {
    const auto n = static_cast<u32>(c.nodes.size());
    std::vector<bool> reached(n, false);
    u32 count = 0;
    for (u32 high = 1; high < n; ++high) {
        for (u32 low = 0; low < high; ++low) {
            if (c.edges[Cluster::pair_index(low, high)] < 0) continue;
            count += reached[low] ? 0 : 1;
            count += reached[high] ? 0 : 1;
            reached[low] = true;
            reached[high] = true;
        }
    }
    if (count == n) return;
    std::vector<ClusterNode> nodes;
    std::vector<i8> edges;
    nodes.reserve(count);
    edges.reserve(count > 1 ? static_cast<size_t>(count) * (count - 1) / 2 : 0);
    for (u32 high = 0; high < n; ++high) {
        if (!reached[high]) continue;
        for (u32 low = 0; low < high; ++low)
            if (reached[low]) edges.push_back(c.edges[Cluster::pair_index(low, high)]);
        nodes.push_back(c.nodes[high]);
    }
    c.nodes = std::move(nodes);
    c.edges = std::move(edges);
}

/// The edge search's open list: Moho's FIFO ring over the window's cells,
/// where a cell whose cost improves moves to the back.
class CellRing {
public:
    static constexpr i32 kCells = OccupationWindow::kSpan * OccupationWindow::kSpan;

    CellRing() {
        for (i32 i = 0; i <= kCells; ++i) prev_[i] = next_[i] = static_cast<u8>(i);
    }
    bool empty() const { return next_[kHead] == kHead; }
    i32 front() const { return next_[kHead]; }
    void unlink(i32 c) {
        prev_[next_[c]] = prev_[c];
        next_[prev_[c]] = next_[c];
        prev_[c] = next_[c] = static_cast<u8>(c);
    }
    void push_back(i32 c) {
        prev_[c] = prev_[kHead];
        next_[c] = kHead;
        next_[prev_[kHead]] = static_cast<u8>(c);
        prev_[kHead] = static_cast<u8>(c);
    }

private:
    static constexpr i32 kHead = kCells;
    std::array<u8, kCells + 1> prev_{};
    std::array<u8, kCells + 1> next_{};
};

} // namespace

f32 octile_distance(i32 dx, i32 dz) {
    const f32 x = std::fabs(static_cast<f32>(dx));
    const f32 z = std::fabs(static_cast<f32>(dz));
    return z <= x ? z * kOctileFactor + x : x * kOctileFactor + z;
}

i8 quantize_edge_cost(f32 cost, f32 octile) {
    // ceil(6 ln r) <= q exactly when r <= exp(q / 6).
    const f32 ratio = cost / octile;
    size_t q = 0;
    while (q < 31 && ratio > kBucketScale[q]) ++q;
    return static_cast<i8>(q);
}

f32 dequantize_edge_cost(i32 bucket, f32 octile) {
    return kBucketScale[static_cast<size_t>(bucket)] * octile;
}

Cluster build_cluster(const OccupationWindow& window) {
    constexpr i32 kLast = OccupationWindow::kSpan - 1;
    Cluster c;
    // Portals: each open run along an edge (row 0, row 8, column 0, column
    // 8) gives one, at its middle as Moho places it.
    for (i32 edge = 0; edge < 4; ++edge) {
        i32 start = -1;
        i32 end = -1;
        for (i32 step = 0; step <= kLast; ++step) {
            const i32 x = edge < 2 ? step : (edge == 2 ? 0 : kLast);
            const i32 z = edge < 2 ? (edge == 0 ? 0 : kLast) : step;
            if (window.open(x, z)) {
                end = step;
                if (start < 0) start = step;
            }
            if (start < 0 || (step != kLast && step == end)) continue;
            i32 mid = 0;
            if (start <= 4 && end >= 4) mid = 4;
            else if (start == 0) mid = 0;
            else if (end == kLast) mid = kLast;
            else mid = (start + end + (end < 4 ? 1 : 0)) >> 1;
            c.nodes.push_back(edge < 2 ? ClusterNode{static_cast<u8>(mid), static_cast<u8>(z)}
                                       : ClusterNode{static_cast<u8>(x), static_cast<u8>(mid)});
            start = -1;
            end = -1;
        }
    }
    sort_nodes(c.nodes);

    // Costs: from each node, a label-correcting search over the window;
    // each later node it reached gets the cost of the way there, quantised.
    const auto n = static_cast<u32>(c.nodes.size());
    if (n >= 2) c.edges.assign(static_cast<size_t>(n) * (n - 1) / 2, -1);
    for (u32 s = 0; s + 1 < n; ++s) {
        std::array<f32, CellRing::kCells> cost{};
        std::array<bool, CellRing::kCells> seen{};
        CellRing ring;
        const ClusterNode from = c.nodes[s];
        const i32 seed = from.z * OccupationWindow::kSpan + from.x;
        seen[static_cast<size_t>(seed)] = true;
        ring.push_back(seed);
        while (!ring.empty()) {
            const i32 cur = ring.front();
            ring.unlink(cur);
            const i32 cz = cur / OccupationWindow::kSpan;
            const i32 cx = cur % OccupationWindow::kSpan;
            u32 open_sides = 0;
            for (size_t k = 0; k < 8; ++k) {
                if ((open_sides & kSidesNeeded[k]) != kSidesNeeded[k]) continue;
                const i32 nz = cz + kStepZ[k];
                const i32 nx = cx + kStepX[k];
                if (nx < 0 || nz < 0 || nx > kLast || nz > kLast || !window.open(nx, nz)) continue;
                const f32 reached = kStepCost[k] + cost[static_cast<size_t>(cur)];
                open_sides |= 1U << k;
                const size_t cell = grid_index(nx, nz, OccupationWindow::kSpan);
                if (seen[cell] && reached >= cost[cell]) continue;
                ring.unlink(static_cast<i32>(cell));
                seen[cell] = true;
                cost[cell] = reached;
                ring.push_back(static_cast<i32>(cell));
            }
        }
        for (u32 t = s + 1; t < n; ++t) {
            const ClusterNode to = c.nodes[t];
            const size_t cell = grid_index(to.x, to.z, OccupationWindow::kSpan);
            if (!seen[cell]) continue;
            c.edges[Cluster::pair_index(s, t)] =
                quantize_edge_cost(cost[cell], node_octile(from, to));
        }
    }
    erase_unconnected(c);
    return c;
}

Cluster build_cluster(const std::array<const Cluster*, 16>& children, u32 child_level) {
    const i32 shift = kClusterShift[child_level];
    const i32 span = kClusterSize[child_level + 1];
    Cluster c;
    // Its portals: its children's that lie on its boundary lines.
    for (i32 cz = 0; cz < 4; ++cz) {
        for (i32 cx = 0; cx < 4; ++cx) {
            const Cluster* child = children[grid_index(cx, cz, 4)];
            if (!child) continue;
            for (const ClusterNode& node : child->nodes) {
                const i32 x = (cx << shift) + node.x;
                const i32 z = (cz << shift) + node.z;
                if ((x & (span - 1)) != 0 && (z & (span - 1)) != 0) continue;
                c.nodes.push_back({static_cast<u8>(x), static_cast<u8>(z)});
            }
        }
    }
    sort_nodes(c.nodes);

    const auto n = static_cast<u32>(c.nodes.size());
    if (n >= 2) c.edges.assign(static_cast<size_t>(n) * (n - 1) / 2, -1);
    // From each, a Dijkstra over the children's edges (Moho's
    // SubclusterSearch: no heuristic), to every portal before it.
    const i32 cells = span + 1;
    const auto at = [cells](i32 x, i32 z) { return grid_index(x, z, cells); };
    std::vector<f32> dist(static_cast<size_t>(cells * cells));
    std::vector<u8> state(dist.size()); // 0 unseen, 1 open, 2 closed
    using Open = std::pair<f32, u16>;   // cost, x | z << 8
    for (u32 high = 1; high < n; ++high) {
        std::fill(state.begin(), state.end(), u8{0});
        std::priority_queue<Open, std::vector<Open>, std::greater<>> open;
        const ClusterNode start = c.nodes[high];
        dist[at(start.x, start.z)] = 0.0f;
        state[at(start.x, start.z)] = 1;
        open.emplace(0.0f, packed(start));
        while (!open.empty()) {
            const auto [d, key] = open.top();
            open.pop();
            const i32 x = key & 0xFF;
            const i32 z = key >> 8;
            if (state[at(x, z)] == 2 || d > dist[at(x, z)]) continue;
            state[at(x, z)] = 2;
            // Every child holding it (two along a shared line, four at a
            // corner), each portal of that child it reaches.
            const i32 x0 = std::max((x - 1) >> shift, 0);
            const i32 z0 = std::max((z - 1) >> shift, 0);
            const i32 x1 = std::min((x >> shift) + 1, 4);
            const i32 z1 = std::min((z >> shift) + 1, 4);
            for (i32 cz = z0; cz < z1; ++cz) {
                for (i32 cx = x0; cx < x1; ++cx) {
                    const Cluster* child = children[grid_index(cx, cz, 4)];
                    if (!child) continue;
                    const i32 ox = cx << shift;
                    const i32 oz = cz << shift;
                    const ClusterNode local{static_cast<u8>(x - ox), static_cast<u8>(z - oz)};
                    const auto it = std::find(child->nodes.begin(), child->nodes.end(), local);
                    if (it == child->nodes.end()) continue;
                    const auto from = static_cast<u32>(it - child->nodes.begin());
                    for (u32 to = 0; to < child->nodes.size(); ++to) {
                        if (to == from) continue;
                        const i8 bucket = child->edge(from, to);
                        if (bucket < 0) continue;
                        const ClusterNode other = child->nodes[to];
                        const f32 reached =
                            d + dequantize_edge_cost(bucket, node_octile(local, other));
                        const size_t cell = at(ox + other.x, oz + other.z);
                        if (state[cell] == 2 || (state[cell] == 1 && reached >= dist[cell]))
                            continue;
                        state[cell] = 1;
                        dist[cell] = reached;
                        open.emplace(reached,
                                     static_cast<u16>((ox + other.x) | ((oz + other.z) << 8)));
                    }
                }
            }
        }
        for (u32 low = 0; low < high; ++low) {
            const ClusterNode to = c.nodes[low];
            if (state[at(to.x, to.z)] == 0) continue;
            c.edges[Cluster::pair_index(low, high)] =
                quantize_edge_cost(dist[at(to.x, to.z)], node_octile(to, start));
        }
    }
    erase_unconnected(c);
    return c;
}

size_t ClusterCache::WindowHash::operator()(const OccupationWindow& w) const {
    size_t h = 0;
    for (const u16 row : w.rows) h = h * 1000003U ^ row;
    return h;
}

size_t ClusterCache::ChildrenHash::operator()(const ChildrenKey& k) const {
    size_t h = k.level;
    for (const u64 s : k.serials) h = h * 1000003U ^ static_cast<size_t>(s ^ (s >> 32));
    return h;
}

ClusterRef ClusterCache::keep(Cluster cluster) {
    cluster.serial = next_serial_++;
    ++builds_;
    return std::make_shared<const Cluster>(std::move(cluster));
}

template <typename Map> void ClusterCache::sweep(Map& entries, size_t& at) {
    if (entries.size() < at) return;
    for (auto it = entries.begin(); it != entries.end();)
        it = it->second.expired() ? entries.erase(it) : std::next(it);
    at = std::max<size_t>(1024, entries.size() * 2);
}

ClusterRef ClusterCache::fetch(const OccupationWindow& window) {
    if (const auto it = by_window_.find(window); it != by_window_.end())
        if (ClusterRef live = it->second.lock()) return live;
    sweep(by_window_, sweep_windows_at_);
    ClusterRef built = keep(build_cluster(window));
    by_window_[window] = built;
    return built;
}

ClusterRef ClusterCache::fetch(const std::array<ClusterRef, 16>& children, u32 child_level) {
    ChildrenKey key;
    key.level = child_level;
    std::array<const Cluster*, 16> raw{};
    for (size_t i = 0; i < children.size(); ++i) {
        raw[i] = children[i].get();
        key.serials[i] = raw[i] ? raw[i]->serial : 0;
    }
    if (const auto it = by_children_.find(key); it != by_children_.end())
        if (ClusterRef live = it->second.lock()) return live;
    sweep(by_children_, sweep_children_at_);
    ClusterRef built = keep(build_cluster(raw, child_level));
    by_children_[key] = built;
    return built;
}

size_t ClusterCache::live_payloads() const {
    size_t live = 0;
    for (const auto& [key, ref] : by_window_) live += ref.expired() ? 0 : 1;
    for (const auto& [key, ref] : by_children_) live += ref.expired() ? 0 : 1;
    return live;
}

BitGrid::BitGrid(i32 width, i32 height)
    : width_(width), height_(height),
      words_(static_cast<size_t>(width) * static_cast<size_t>((height + 31) >> 5), 0) {}

bool BitGrid::test(i32 x, i32 z) const {
    if (x < 0 || z < 0 || x >= width_ || z >= height_) return false;
    return ((words_[grid_index(x, z >> 5, width_)] >> (z & 31)) & 1U) != 0;
}

void BitGrid::set(i32 x, i32 z, bool on) {
    if (x < 0 || z < 0 || x >= width_ || z >= height_) return;
    u32& word = words_[grid_index(x, z >> 5, width_)];
    const u32 bit = 1U << (z & 31);
    word = on ? (word | bit) : (word & ~bit);
}

void BitGrid::fill(const OccupancyRect& r, bool on) {
    const i32 x0 = std::max(r.x0, 0);
    const i32 z0 = std::max(r.z0, 0);
    const i32 x1 = std::min(r.x1, width_);
    const i32 z1 = std::min(r.z1, height_);
    for (i32 z = z0; z < z1; ++z)
        for (i32 x = x0; x < x1; ++x) set(x, z, on);
}

bool BitGrid::find_set(u32& progress, i32& x, i32& z) const {
    const auto size = static_cast<u32>(words_.size());
    if (size == 0) return false;
    u32 cur = progress < size ? progress : 0;
    for (u32 looked = 0; words_[cur] == 0;) {
        if (++cur == size) cur = 0;
        if (++looked == size) return false;
    }
    const u32 word = words_[cur];
    i32 bit = 0;
    while (((word >> bit) & 1U) == 0) ++bit;
    x = static_cast<i32>(cur % static_cast<u32>(width_));
    z = bit + 32 * static_cast<i32>(cur / static_cast<u32>(width_));
    progress = cur;
    return true;
}

bool BitGrid::any() const {
    return std::any_of(words_.begin(), words_.end(), [](u32 w) { return w != 0; });
}

ClusterMap::ClusterMap(const OccupationSource& source, ClusterCache& cache, u32 width, u32 height,
                       i32 size_x, i32 size_z)
    : source_(source), cache_(cache), size_x_(std::max(size_x, 1)), size_z_(std::max(size_z, 1)) {
    const i32 top = kClusterSize[kClusterLevels];
    width_ = (static_cast<i32>(width) + top - 1) & ~(top - 1);
    height_ = (static_cast<i32>(height) + top - 1) & ~(top - 1);
    for (u32 level = 1; level <= kClusterLevels; ++level) {
        levels_[level].resize(static_cast<size_t>(clusters_x(level)) *
                              static_cast<size_t>(clusters_z(level)));
        dirty_[level] = BitGrid(clusters_x(level), clusters_z(level));
        dirty_[level].fill({0, 0, clusters_x(level), clusters_z(level)}, true);
    }
}

void ClusterMap::dirty_rect(const OccupancyRect& r) {
    done_ = false;
    if (r.x1 <= r.x0 || r.z1 <= r.z0) return;
    // Level-1 cluster c reads cells [8c, 8c + 7 + size]: the clusters from
    // the one reading x0 at its far end to the one holding x1 - 1.
    OccupancyRect clusters{
        (r.x0 - size_x_) >> kClusterShift[1], (r.z0 - size_z_) >> kClusterShift[1],
        ((r.x1 - 1) >> kClusterShift[1]) + 1, ((r.z1 - 1) >> kClusterShift[1]) + 1};
    for (u32 level = 1; level <= kClusterLevels; ++level) {
        if (level > 1) {
            // Their parents: a cluster is built from its children alone.
            const i32 s = kClusterShift[level] - kClusterShift[level - 1];
            clusters = {clusters.x0 >> s, clusters.z0 >> s, ((clusters.x1 - 1) >> s) + 1,
                        ((clusters.z1 - 1) >> s) + 1};
        }
        dirty_[level].fill(clusters, true);
    }
}

OccupancyRect ClusterMap::cluster_rect(i32 x, i32 z, u32 level) const {
    const i32 size = kClusterSize[level];
    const i32 mask = -size;
    return {std::max(mask & (x - 1), 0), std::max(mask & (z - 1), 0),
            std::min((mask & (size + x)) + 1, width_), std::min((mask & (size + z)) + 1, height_)};
}

OccupancyRect ClusterMap::cluster_index_rect(i32 x, i32 z, u32 level) const {
    const i32 shift = kClusterShift[level];
    return {std::max((x - 1) >> shift, 0), std::max((z - 1) >> shift, 0),
            std::min((x >> shift) + 1, width_ >> shift),
            std::min((z >> shift) + 1, height_ >> shift)};
}

bool ClusterMap::work_on_cluster(i32 cx, i32 cz, u32 level, i32& budget) {
    if (!dirty_[level].test(cx, cz)) return true;
    if (budget <= 0) return false;
    ClusterRef built;
    if (level == 1) {
        built = cache_.fetch(source_.window(cx << kClusterShift[1], cz << kClusterShift[1]));
    } else {
        std::array<ClusterRef, 16> children;
        for (i32 z = 0; z < 4; ++z) {
            for (i32 x = 0; x < 4; ++x) {
                const i32 ccx = cx * 4 + x;
                const i32 ccz = cz * 4 + z;
                if (!work_on_cluster(ccx, ccz, level - 1, budget)) return false;
                children[grid_index(x, z, 4)] = levels_[level - 1][index(level - 1, ccx, ccz)];
            }
        }
        built = cache_.fetch(children, level - 1);
    }
    budget -= kClusterCost;
    levels_[level][index(level, cx, cz)] = std::move(built);
    dirty_[level].set(cx, cz, false);
    return true;
}

void ClusterMap::rebuild_clean() {
    for (u32 level = 1; level <= kClusterLevels; ++level) {
        for (i32 cz = 0; cz < clusters_z(level); ++cz) {
            for (i32 cx = 0; cx < clusters_x(level); ++cx) {
                if (dirty_[level].test(cx, cz)) continue;
                if (level == 1) {
                    levels_[1][index(1, cx, cz)] = cache_.fetch(
                        source_.window(cx << kClusterShift[1], cz << kClusterShift[1]));
                    continue;
                }
                std::array<ClusterRef, 16> children;
                for (i32 z = 0; z < 4; ++z)
                    for (i32 x = 0; x < 4; ++x)
                        children[grid_index(x, z, 4)] =
                            levels_[level - 1][index(level - 1, cx * 4 + x, cz * 4 + z)];
                levels_[level][index(level, cx, cz)] = cache_.fetch(children, level - 1);
            }
        }
    }
}

void ClusterMap::background_work(i32& budget) {
    const bool unlimited = budget == INT_MAX;
    while (!done_ && budget > 0) {
        i32 cx = 0;
        i32 cz = 0;
        if (!dirty_[kClusterLevels].find_set(progress_, cx, cz)) {
            done_ = true;
            break;
        }
        if (unlimited) budget = INT_MAX;
        work_on_cluster(cx, cz, kClusterLevels, budget);
    }
}

} // namespace osc::sim::path
