// Path clusters (roadmap item 4c-1): Moho's level-1 and level-2 cluster
// builds (faf-re Cluster.cpp), the cache that shares them, the per-class
// maps with their dirty bits and background budget (ClusterMap.h,
// BitArray2D.cpp, PathTables.cpp), and the sim keeping them current.

#include <catch2/catch_test_macros.hpp>

#include "blueprints/blueprint_store.hpp"
#include "blueprints/footprint.hpp"
#include "map/heightmap.hpp"
#include "map/terrain.hpp"
#include "sim/occupancy.hpp"
#include "sim/path_clusters.hpp"
#include "sim/path_tables.hpp"
#include "sim/saved_game.hpp"
#include "sim/sim_snapshot.hpp"
#include "sim/sim_state.hpp"

extern "C" {
#include <lua.h>
}

#include <array>
#include <climits>
#include <cmath>
#include <memory>
#include <queue>
#include <string>
#include <vector>

using osc::f32;
using osc::i32;
using osc::i8;
using osc::u16;
using osc::u32;
using osc::u8;
using osc::blueprints::NamedFootprint;
using osc::sim::OccupancyGrid;
using osc::sim::OccupancyRect;
using osc::sim::path::Cluster;
using osc::sim::path::ClusterCache;
using osc::sim::path::ClusterMap;
using osc::sim::path::ClusterNode;
using osc::sim::path::ClusterRef;
using osc::sim::path::OccupationWindow;
namespace oc = osc::blueprints::occupancy;
namespace path = osc::sim::path;

namespace {

/// A window from nine strings, row 0 first, '.' open and '#' closed.
OccupationWindow window_of(const std::array<const char*, 9>& rows) {
    OccupationWindow w;
    for (size_t z = 0; z < 9; ++z)
        for (size_t x = 0; x < 9; ++x)
            if (rows[z][x] == '.') w.rows[z] = static_cast<u16>(w.rows[z] | (1U << x));
    return w;
}

OccupationWindow open_window() {
    OccupationWindow w;
    w.rows.fill(0x1FF);
    return w;
}

bool has_node(const Cluster& c, u8 x, u8 z) {
    for (const ClusterNode& n : c.nodes)
        if (n.x == x && n.z == z) return true;
    return false;
}

i32 node_index(const Cluster& c, u8 x, u8 z) {
    for (size_t i = 0; i < c.nodes.size(); ++i)
        if (c.nodes[i].x == x && c.nodes[i].z == z) return static_cast<i32>(i);
    return -1;
}

/// The shortest way between two cells of a window, by an ordinary Dijkstra
/// in double (diagonals only past two open sides): the test's own answer.
double reference_cost(const OccupationWindow& w, ClusterNode from, ClusterNode to) {
    std::array<double, 81> dist{};
    dist.fill(1e30);
    using Item = std::pair<double, int>;
    std::priority_queue<Item, std::vector<Item>, std::greater<>> open;
    dist[static_cast<size_t>(from.z * 9 + from.x)] = 0;
    open.emplace(0.0, from.z * 9 + from.x);
    while (!open.empty()) {
        const auto [d, c] = open.top();
        open.pop();
        if (d > dist[static_cast<size_t>(c)]) continue;
        const int cx = c % 9;
        const int cz = c / 9;
        for (int dz = -1; dz <= 1; ++dz) {
            for (int dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dz == 0) continue;
                const int nx = cx + dx;
                const int nz = cz + dz;
                if (nx < 0 || nz < 0 || nx > 8 || nz > 8 || !w.open(nx, nz)) continue;
                if (dx != 0 && dz != 0 && (!w.open(cx + dx, cz) || !w.open(cx, cz + dz))) continue;
                const double nd = d + ((dx != 0 && dz != 0) ? std::sqrt(2.0) : 1.0);
                const size_t cell = static_cast<size_t>(nz) * 9 + static_cast<size_t>(nx);
                if (nd < dist[cell]) {
                    dist[cell] = nd;
                    open.emplace(nd, nz * 9 + nx);
                }
            }
        }
    }
    return dist[static_cast<size_t>(to.z * 9 + to.x)];
}

/// A flat, dry `size` x `size` map.
std::unique_ptr<osc::map::Terrain> flat_terrain(u32 size) {
    std::vector<u16> heights(static_cast<size_t>(size + 1) * (size + 1), 1280);
    osc::map::Heightmap hm(size, size, 1.0f / 128.0f, std::move(heights));
    return std::make_unique<osc::map::Terrain>(std::move(hm), 0.0f, false);
}

NamedFootprint land_class(int size, const char* name) {
    NamedFootprint f;
    f.name = name;
    f.size_x = static_cast<u8>(size);
    f.size_z = static_cast<u8>(size);
    f.caps = oc::kLand;
    f.max_water_depth = 0.05f;
    f.max_slope = 0.75f;
    return f;
}

/// Every cluster of `a` and `b` alike: built, and the same content.
bool same_clusters(const ClusterMap& a, const ClusterMap& b) {
    for (u32 level = 1; level <= path::kClusterLevels; ++level) {
        for (i32 cz = 0; cz < a.clusters_z(level); ++cz) {
            for (i32 cx = 0; cx < a.clusters_x(level); ++cx) {
                const Cluster* ca = a.cluster(level, cx, cz);
                const Cluster* cb = b.cluster(level, cx, cz);
                if (!ca || !cb || !ca->same_content(*cb)) return false;
            }
        }
    }
    return true;
}

struct LuaGuard {
    lua_State* L = lua_open();
    ~LuaGuard() { lua_close(L); }
};

/// A stand-in source: every window open, counting the windows it is asked
/// for.
class OpenSource final : public path::OccupationSource {
public:
    OccupationWindow window(i32, i32) const override {
        ++asked;
        return open_window();
    }
    mutable int asked = 0;
};

} // namespace

TEST_CASE("Edge costs quantise as Moho's ceil(6 ln r), clamped to 0-31, and come back from "
          "its table",
          "[path_clusters]") {
    CHECK(path::quantize_edge_cost(5.0f, 5.0f) == 0);
    CHECK(path::quantize_edge_cost(4.9f, 5.0f) == 0); // under the straight line: 0
    CHECK(path::quantize_edge_cost(1e6f, 1.0f) == 31);
    for (int i = 0; i < 2000; ++i) {
        const double r = 1.0 + i * 0.0731;
        const double exact = 6.0 * std::log(r);
        if (std::fabs(exact - std::round(exact)) < 1e-4) continue; // on a boundary
        const int want = std::clamp(static_cast<int>(std::ceil(exact)), 0, 31);
        INFO("ratio " << r);
        CHECK(path::quantize_edge_cost(static_cast<f32>(r) * 3.0f, 3.0f) == want);
    }
    CHECK(path::dequantize_edge_cost(0, 7.0f) == 7.0f);
    CHECK(std::fabs(path::dequantize_edge_cost(6, 1.0f) - std::exp(1.0f)) < 1e-6f);
    CHECK(path::octile_distance(4, -3) == 3.0f * 0.41421354f + 4.0f);
}

TEST_CASE("An open cluster has a portal at each edge's middle, each a straight line from "
          "the next",
          "[path_clusters]") {
    const Cluster c = path::build_cluster(open_window());
    REQUIRE(c.nodes.size() == 4);
    // In the order of x | z << 8.
    CHECK((c.nodes[0] == ClusterNode{4, 0}));
    CHECK((c.nodes[1] == ClusterNode{0, 4}));
    CHECK((c.nodes[2] == ClusterNode{8, 4}));
    CHECK((c.nodes[3] == ClusterNode{4, 8}));
    REQUIRE(c.edges.size() == 6);
    for (const i8 e : c.edges) CHECK(e == 0);
}

TEST_CASE("Portals sit where Moho puts them along a run: at 4 if it covers 4, else at a "
          "corner it touches, else its middle, leaning out from 4",
          "[path_clusters]") {
    // Row 0: runs 0-2 (a corner: 0) and 5-8 (a corner: 8). Row 8: runs 1-3
    // ((1 + 3 + 1) / 2 = 2) and 5-7 ((5 + 7) / 2 = 6). Columns 0 and 8 are
    // closed but at their ends. The middle is open, so all connect.
    const Cluster c = path::build_cluster(window_of({
        "...##....",
        "#.......#",
        "#.......#",
        "#.......#",
        "#.......#",
        "#.......#",
        "#.......#",
        "#.......#",
        "#...#...#",
    }));
    CHECK(has_node(c, 0, 0));
    CHECK(has_node(c, 8, 0));
    CHECK(has_node(c, 2, 8));
    CHECK(has_node(c, 6, 8));
    CHECK(c.nodes.size() == 4); // the columns' runs land on the same corners

    // A run 3-6 covers 4.
    const Cluster d = path::build_cluster(window_of({
        "###....##",
        ".........",
        ".........",
        ".........",
        ".........",
        ".........",
        ".........",
        ".........",
        ".........",
    }));
    CHECK(has_node(d, 4, 0));
}

TEST_CASE("A cluster's portals connect only through open cells, diagonals only past two "
          "open sides; one with no way to another goes",
          "[path_clusters]") {
    // A wall down column 4: the halves never meet.
    const Cluster walled = path::build_cluster(window_of({
        "....#....",
        "....#....",
        "....#....",
        "....#....",
        "....#....",
        "....#....",
        "....#....",
        "....#....",
        "....#....",
    }));
    const i32 left = node_index(walled, 0, 4);
    const i32 right = node_index(walled, 8, 4);
    REQUIRE(left >= 0);
    REQUIRE(right >= 0);
    CHECK(walled.edge(static_cast<u32>(left), static_cast<u32>(right)) == -1);

    // Two open blocks meeting only corner to corner, (3, 3) and (4, 4),
    // past closed sides: no way through, so the north-west block's one
    // portal (0, 0) has nobody to reach and goes.
    const Cluster pinched = path::build_cluster(window_of({
        "....#####",
        "....#####",
        "....#####",
        "....#####",
        "####.....",
        "####.....",
        "####.....",
        "####.....",
        "####.....",
    }));
    CHECK_FALSE(has_node(pinched, 0, 0));
    CHECK(has_node(pinched, 8, 4));
    CHECK(has_node(pinched, 4, 8));
    CHECK(pinched.nodes.size() == 2);

    // A single portal has nobody to reach.
    const Cluster lonely = path::build_cluster(window_of({
        "####.####",
        "####.####",
        "#########",
        "#########",
        "#########",
        "#########",
        "#########",
        "#########",
        "#########",
    }));
    CHECK(lonely.nodes.empty());
    CHECK(lonely.edges.empty());
}

TEST_CASE("Random windows: every portal pair costs what a plain Dijkstra finds, quantised",
          "[path_clusters]") {
    u32 seed = 12345;
    const auto next = [&seed] {
        seed = seed * 1664525U + 1013904223U;
        return seed >> 8;
    };
    int pairs = 0;
    for (int trial = 0; trial < 300; ++trial) {
        OccupationWindow w;
        const u32 density = 55 + next() % 40; // percent open
        for (size_t z = 0; z < 9; ++z)
            for (int x = 0; x < 9; ++x)
                if (next() % 100 < density) w.rows[z] = static_cast<u16>(w.rows[z] | (1U << x));
        const Cluster c = path::build_cluster(w);
        REQUIRE(c.edges.size() ==
                c.nodes.size() * (c.nodes.size() - (c.nodes.empty() ? 0 : 1)) / 2);
        for (u32 i = 0; i < c.nodes.size(); ++i) {
            const ClusterNode a = c.nodes[i];
            CHECK(w.open(a.x, a.z));
            CHECK((a.x == 0 || a.x == 8 || a.z == 0 || a.z == 8));
            bool connected = false;
            for (u32 j = 0; j < c.nodes.size(); ++j) {
                if (j == i) continue;
                const ClusterNode b = c.nodes[j];
                const double cost = reference_cost(w, a, b);
                const i8 e = c.edge(i, j);
                connected = connected || e >= 0;
                if (cost > 1e29) {
                    CHECK(e == -1);
                    continue;
                }
                const double exact =
                    6.0 * std::log(cost / static_cast<double>(
                                              path::octile_distance(a.x - b.x, a.z - b.z)));
                if (std::fabs(exact - std::round(exact)) < 1e-3) continue; // on a boundary
                INFO("trial " << trial << " (" << int(a.x) << "," << int(a.z) << ")-(" << int(b.x)
                              << "," << int(b.z) << ") cost " << cost);
                CHECK(e == std::clamp(static_cast<int>(std::ceil(exact)), 0, 31));
                ++pairs;
            }
            CHECK(connected);
        }
    }
    CHECK(pairs > 1000);
}

TEST_CASE("A level-2 cluster's portals are its children's on its edges, its costs their "
          "shortest ways through the children, quantised again",
          "[path_clusters]") {
    const Cluster open = path::build_cluster(open_window());
    std::array<const Cluster*, 16> children{};
    children.fill(&open);
    const Cluster c = path::build_cluster(children, 1);
    // Four a side: each child's mid-edge portal on the boundary.
    REQUIRE(c.nodes.size() == 16);
    CHECK(has_node(c, 4, 0));
    CHECK(has_node(c, 28, 0));
    CHECK(has_node(c, 0, 12));
    CHECK(has_node(c, 32, 20));
    CHECK(has_node(c, 20, 32));
    // (4, 0) to (12, 0) runs through (8, 4): two legs of 4 + 4 x 0.414,
    // over a straight 8, so ceil(6 ln 1.414) = 3.
    const i32 a = node_index(c, 4, 0);
    const i32 b = node_index(c, 12, 0);
    CHECK(c.edge(static_cast<u32>(a), static_cast<u32>(b)) == 3);
    // Straight across, through four children: their portals line up.
    const i32 top = node_index(c, 4, 0);
    const i32 bottom = node_index(c, 4, 32);
    CHECK(c.edge(static_cast<u32>(top), static_cast<u32>(bottom)) == 0);

    // A closed column of children splits it.
    const Cluster closed = path::build_cluster(OccupationWindow{});
    CHECK(closed.nodes.empty());
    for (int z = 0; z < 4; ++z) children[1 + 4 * static_cast<size_t>(z)] = &closed;
    const Cluster split = path::build_cluster(children, 1);
    const i32 west = node_index(split, 0, 4);
    const i32 east = node_index(split, 32, 4);
    REQUIRE(west >= 0);
    REQUIRE(east >= 0);
    CHECK(split.edge(static_cast<u32>(west), static_cast<u32>(east)) == -1);
}

TEST_CASE("The cache builds a cluster once for every map that asks, and lets it go with the "
          "last",
          "[path_clusters]") {
    ClusterCache cache;
    ClusterRef a = cache.fetch(open_window());
    ClusterRef b = cache.fetch(open_window());
    CHECK(a == b);
    CHECK(cache.builds() == 1);
    std::array<ClusterRef, 16> children;
    children.fill(a);
    ClusterRef p = cache.fetch(children, 1);
    ClusterRef q = cache.fetch(children, 1);
    CHECK(p == q);
    CHECK(cache.builds() == 2);
    CHECK(cache.live_payloads() == 2);
    a.reset();
    b.reset();
    children.fill(nullptr);
    p.reset();
    q.reset();
    CHECK(cache.live_payloads() == 0);
    // Gone, it is built again.
    ClusterRef c = cache.fetch(open_window());
    CHECK(cache.builds() == 3);
    CHECK(c->serial != 0);
}

TEST_CASE("A map's clusters start dirty and are rebuilt, children first, 10 from the budget "
          "each, scanning on where the last pass stopped",
          "[path_clusters]") {
    OpenSource source;
    ClusterCache cache;
    ClusterMap m(source, cache, 50, 64, 1, 1); // rounded up to 64 x 64
    CHECK(m.width() == 64);
    CHECK(m.clusters_x(1) == 8);
    CHECK(m.clusters_x(2) == 2);
    CHECK(m.dirty(1, 7, 7));
    CHECK(m.dirty(2, 1, 1));

    i32 budget = 100;
    m.background_work(budget);
    CHECK(budget == 0);
    CHECK(source.asked == 10); // ten children of the first top cluster
    CHECK(m.dirty(2, 0, 0));
    CHECK_FALSE(m.dirty(1, 1, 1));
    CHECK(m.dirty(1, 2, 2));

    budget = 100;
    m.background_work(budget);
    // The other six children and their parent (70), then three children
    // of the next top cluster.
    CHECK(budget == 0);
    CHECK(source.asked == 19);
    CHECK_FALSE(m.dirty(2, 0, 0));
    REQUIRE(m.cluster(2, 0, 0));
    CHECK(m.cluster(2, 0, 0)->nodes.size() == 16);
    CHECK(m.dirty(2, 1, 0));

    // A build is refused only once the budget is spent.
    budget = 5;
    CHECK(m.work_on_cluster(4, 0, 1, budget));
    CHECK(budget == -5);
    CHECK_FALSE(m.work_on_cluster(5, 0, 1, budget));
    CHECK(m.dirty(1, 5, 0));

    budget = INT_MAX;
    m.background_work(budget);
    CHECK(m.done());
    for (i32 z = 0; z < 8; ++z)
        for (i32 x = 0; x < 8; ++x) CHECK_FALSE(m.dirty(1, x, z));
}

TEST_CASE("A footprint's window: a corner is open when every cell under the footprint is",
          "[path_clusters]") {
    const auto terrain = flat_terrain(64);
    OccupancyGrid grid(64, 64);
    grid.fill(oc::kLand, {10, 10, 11, 11}, true);
    const NamedFootprint three = land_class(3, "Vehicle3x3");
    const osc::sim::FootprintOccupation source(three, *terrain, grid);
    const OccupationWindow w = source.window(8, 8);
    for (i32 r = 0; r < 9; ++r) {
        for (i32 b = 0; b < 9; ++b) {
            // Corner (8 + b, 8 + r) covers (10, 10) for b and r 0-2.
            INFO("bit " << b << " row " << r);
            CHECK(w.open(b, r) == !(b <= 2 && r <= 2));
        }
    }
    // Off the map's far edge, nothing fits: a 3-wide corner needs cells to 63.
    const OccupationWindow edge = source.window(56, 0);
    CHECK(edge.open(5, 0));       // corner 61: cells 61-63
    CHECK_FALSE(edge.open(6, 0)); // corner 62: cell 64 is off the map
    // An amphibian ignoring structures walks over the claim.
    NamedFootprint crusher = three;
    crusher.flags = osc::blueprints::kFootprintIgnoreStructures;
    CHECK(osc::sim::FootprintOccupation(crusher, *terrain, grid).window(8, 8).rows ==
          open_window().rows);
}

TEST_CASE("Dirtying the cells a claim changed leaves no cluster stale, for big classes too "
          "(Moho's pad would leave the clusters below)",
          "[path_clusters]") {
    const auto terrain = flat_terrain(96);
    for (const int size : {1, 3, 12}) {
        INFO("class " << size << "x" << size);
        OccupancyGrid grid(96, 96);
        const NamedFootprint fp = land_class(size, "Class");
        const osc::sim::FootprintOccupation source(fp, *terrain, grid);
        ClusterCache cache;
        ClusterMap m(source, cache, 96, 96, size, size);
        i32 budget = INT_MAX;
        m.background_work(budget);
        REQUIRE(m.done());

        // Moho's example: a 3-wide class, a change at x = 10. Cluster 0
        // reads cells 0-10, so it must go dirty.
        if (size == 3) {
            m.dirty_rect({10, 40, 11, 41});
            CHECK(m.dirty(1, 0, 4)); // z from (40 - 3) >> 3
            CHECK(m.dirty(1, 1, 5));
            CHECK_FALSE(m.dirty(1, 2, 5));
            budget = INT_MAX;
            m.background_work(budget);
        }

        u32 seed = 777U + static_cast<u32>(size);
        const auto next = [&seed] {
            seed = seed * 1664525U + 1013904223U;
            return static_cast<i32>(seed >> 8);
        };
        for (int round = 0; round < 25; ++round) {
            const i32 x0 = next() % 96;
            const i32 z0 = next() % 96;
            const OccupancyRect r{x0, z0, x0 + 1 + next() % 6, z0 + 1 + next() % 6};
            grid.fill(oc::kLand, r, (next() % 3) != 0);
            m.dirty_rect(r);
            budget = INT_MAX;
            m.background_work(budget);
            ClusterCache fresh_cache;
            ClusterMap fresh(source, fresh_cache, 96, 96, size, size);
            budget = INT_MAX;
            fresh.background_work(budget);
            INFO("round " << round);
            REQUIRE(same_clusters(m, fresh));
        }
    }
}

TEST_CASE("The sim keeps a map a class: one budget a tick in class order, dirtied as ground "
          "is claimed, and loaded as saved (its dirty bits, its clean clusters built again)",
          "[path_clusters]") {
    LuaGuard g;
    osc::blueprints::BlueprintStore store(g.L);
    store.add_footprint_class(land_class(1, "Vehicle1x1"));
    store.add_footprint_class(land_class(3, "Vehicle3x3"));
    osc::sim::SimState sim(g.L, &store);
    sim.set_terrain(flat_terrain(96));
    sim.build_pathfinding_grid();
    sim.add_army("ARMY_1", "ARMY_1");
    osc::sim::GameSetup game;
    game.scenario = "/maps/test/test_scenario.lua";
    game.seed = 3;
    sim.set_game_setup(game);
    REQUIRE(sim.path_tables());
    REQUIRE(sim.path_tables()->size() == 2);
    const ClusterMap& small = sim.path_tables()->map(0);
    const ClusterMap& big = sim.path_tables()->map(1);
    CHECK(small.width() == 96);

    // 144 + 9 clusters a map, 10 each: the first tick's 1000 go to the
    // first class.
    sim.tick();
    CHECK_FALSE(small.done());
    CHECK(small.dirty(2, 2, 2));
    CHECK(big.dirty(1, 0, 0));
    for (int i = 0; i < 3; ++i) sim.tick();
    REQUIRE(sim.path_tables()->done());

    osc::sim::GroundOccupant claim;
    claim.caps = oc::kLand;
    claim.rects.push_back({40, 40, 44, 44});
    sim.occupy_ground(9, claim);
    CHECK(small.dirty(1, 5, 5));
    CHECK(big.dirty(1, 4, 4)); // (40 - 3) >> 3
    CHECK_FALSE(sim.path_tables()->done());
    sim.tick();
    CHECK(sim.path_tables()->done());
    // Cluster (4, 4) holds corners 32-40; a 3-wide class can't stand on
    // 38-40 either way, so from (8, 4) to (4, 8) it goes round that block:
    // 4 + 2 x 1.414 over 4 + 4 x 0.414, ceil(6 ln 1.207) = 2.
    const Cluster* c = big.cluster(1, 4, 4);
    REQUIRE(c);
    const i32 east = node_index(*c, 8, 4);
    const i32 south = node_index(*c, 4, 8);
    REQUIRE(east >= 0);
    REQUIRE(south >= 0);
    CHECK(c->edge(static_cast<u32>(east), static_cast<u32>(south)) == 2);
    CHECK(small.cluster(1, 4, 4)->nodes.size() == 4);
    // Saved with some clusters dirty: restored, the same are dirty and the
    // rest built again as they were.
    sim.set_recording(true);
    sim.tick();
    sim.release_ground(9);
    CHECK(big.dirty(1, 4, 4));
    const osc::sim::SavedGame save = osc::sim::save_game(sim, "clusters");
    REQUIRE_FALSE(save.snapshot.empty());
    LuaGuard g2;
    osc::blueprints::BlueprintStore store2(g2.L);
    store2.add_footprint_class(land_class(1, "Vehicle1x1"));
    store2.add_footprint_class(land_class(3, "Vehicle3x3"));
    osc::sim::SimState restored(g2.L, &store2);
    restored.set_terrain(flat_terrain(96));
    restored.build_pathfinding_grid();
    restored.add_army("ARMY_1", "ARMY_1");
    restored.set_game_setup(game);
    const std::string err = osc::sim::load_snapshot(restored, save.snapshot);
    INFO(err);
    REQUIRE(err.empty());
    REQUIRE(restored.path_tables());
    for (size_t m = 0; m < 2; ++m) {
        const ClusterMap& before = sim.path_tables()->map(m);
        const ClusterMap& after = restored.path_tables()->map(m);
        CHECK(after.done() == before.done());
        for (u32 level = 1; level <= osc::sim::path::kClusterLevels; ++level)
            for (i32 cz = 0; cz < before.clusters_z(level); ++cz)
                for (i32 cx = 0; cx < before.clusters_x(level); ++cx) {
                    REQUIRE(after.dirty(level, cx, cz) == before.dirty(level, cx, cz));
                    if (before.dirty(level, cx, cz)) continue;
                    REQUIRE(after.cluster(level, cx, cz));
                    CHECK(
                        after.cluster(level, cx, cz)->same_content(*before.cluster(level, cx, cz)));
                }
    }
}
