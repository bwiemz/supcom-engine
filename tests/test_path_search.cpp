// The hierarchical path search (roadmap item 4c-2a): Moho's A* over cells
// and cluster portals (faf-re AStarSearch.h, PathTables.cpp PathQueue), its
// army queue, and CAiPathFinder's rules for one unit's search.

#include <catch2/catch_test_macros.hpp>

#include "blueprints/footprint.hpp"
#include "map/heightmap.hpp"
#include "map/terrain.hpp"
#include "sim/occupancy.hpp"
#include "sim/path_clusters.hpp"
#include "sim/path_finder.hpp"
#include "sim/path_search.hpp"
#include "sim/path_tables.hpp"

#include <climits>
#include <cstdlib>
#include <memory>
#include <utility>
#include <vector>

using osc::f32;
using osc::i16;
using osc::i32;
using osc::u32;
using osc::u8;
using osc::blueprints::NamedFootprint;
using osc::sim::OccupancyGrid;
using osc::sim::OccupancyRect;
using osc::sim::PathTables;
using osc::sim::path::Cell;
using osc::sim::path::CellIndex;
using osc::sim::path::OpenHeap;
using osc::sim::path::pack_cell;
using osc::sim::path::PathFinder;
using osc::sim::path::PathQueue;
using osc::sim::path::PathWorld;
using osc::sim::path::SearchType;
namespace oc = osc::blueprints::occupancy;
namespace path = osc::sim::path;

namespace {

constexpr u32 kMap = 256;

std::unique_ptr<osc::map::Terrain> flat_terrain(u32 size) {
    std::vector<osc::u16> heights(static_cast<size_t>(size + 1) * (size + 1), 1280);
    osc::map::Heightmap hm(size, size, 1.0f / 128.0f, std::move(heights));
    return std::make_unique<osc::map::Terrain>(std::move(hm), 0.0f, false);
}

NamedFootprint land_class(int size) {
    NamedFootprint f;
    f.name = "Land";
    f.size_x = static_cast<u8>(size);
    f.size_z = static_cast<u8>(size);
    f.caps = oc::kLand;
    f.max_water_depth = 0.05f;
    f.max_slope = 0.75f;
    return f;
}

/// A 256 x 256 flat map with land classes 1x1, 3x3 and 5x5.
struct World {
    std::unique_ptr<osc::map::Terrain> terrain = flat_terrain(kMap);
    OccupancyGrid grid{kMap, kMap};
    std::vector<NamedFootprint> classes{land_class(1), land_class(3), land_class(5)};
    std::unique_ptr<PathTables> tables;

    void make_tables() { tables = std::make_unique<PathTables>(classes, *terrain, grid); }
    PathWorld world(i32 pathcap = 100000) const {
        return {terrain.get(),
                &grid,
                {0, 0, static_cast<i32>(kMap), static_cast<i32>(kMap)},
                false,
                pathcap};
    }
    /// A finder for class `cls` standing at cell `from` (its corner), bound
    /// for cell `to`.
    void aim(PathFinder& f, i32 cls, Cell from, Cell to, SearchType type = SearchType::None,
             i32 pathcap = 100000) const {
        const NamedFootprint& fp = classes[static_cast<size_t>(cls)];
        f.set_unit(fp, cls, false);
        const f32 x = static_cast<f32>(from.x) + static_cast<f32>(fp.size_x) * 0.5f;
        const f32 z = static_cast<f32>(from.z) + static_cast<f32>(fp.size_z) * 0.5f;
        f.prepare(world(pathcap), type, from, x, z);
        f.set_goal({{to.x, to.z, to.x + 1, to.z + 1}, {}});
    }
};

/// Search to the end with budget `slice` a call.
u32 run(World& w, PathQueue& q, PathFinder& f, i32 slice = INT_MAX / 2) {
    q.queue(f);
    u32 calls = 0;
    while (!f.has_result() && calls < 100000) {
        i32 budget = slice;
        q.work(*w.tables, budget);
        ++calls;
    }
    return calls;
}

/// Each step of a path is to a neighbouring cell, or a jump between two
/// portals of one cluster.
bool steps_are_edges(const PathTables& tables, i32 cls, const std::vector<Cell>& cells) {
    const path::ClusterMap& m = tables.map(static_cast<size_t>(cls));
    for (size_t i = 1; i < cells.size(); ++i) {
        const Cell a = cells[i - 1];
        const Cell b = cells[i];
        if (std::abs(a.x - b.x) <= 1 && std::abs(a.z - b.z) <= 1) continue;
        bool shared = false;
        for (u32 level = 1; level <= path::kClusterLevels && !shared; ++level) {
            const OccupancyRect ra = m.cluster_index_rect(a.x, a.z, level);
            const OccupancyRect rb = m.cluster_index_rect(b.x, b.z, level);
            shared = ra.x0 < rb.x1 && rb.x0 < ra.x1 && ra.z0 < rb.z1 && rb.z0 < ra.z1;
        }
        if (!shared) return false;
    }
    return true;
}

} // namespace

TEST_CASE("Moho's open heap: the cheapest first, an equal priority over the older, handles "
          "reused",
          "[path_search]") {
    OpenHeap h;
    const i32 a = h.push(5.0f, 0);
    h.push(5.0f, 1);
    CHECK(h.top() == 1); // equal rises past its parent
    h.push(3.0f, 2);
    CHECK(h.top() == 2);
    h.update(a, 1.0f);
    CHECK(h.top() == 0);
    h.pop();
    CHECK(h.top() == 2);
    const i32 reused = h.push(9.0f, 3);
    CHECK(reused == a); // the popped node's handle comes back
    h.pop();
    h.pop();
    CHECK(h.top() == 3);
    h.pop();
    CHECK(h.empty());
}

TEST_CASE("A search's cell index: the first node a cell gets stays, through growth; cleared, "
          "it holds nothing",
          "[path_search]") {
    CellIndex index;
    CHECK(index.find(0) == nullptr);
    // Every cell of a 100x100 square, the corner cells at -1 among them
    // (0xFFFFFFFF packed), well past the first table's 1,024 slots.
    u32 next = 0;
    for (i32 z = -1; z < 99; ++z)
        for (i32 x = -1; x < 99; ++x) {
            const auto [node, made] =
                index.try_emplace(pack_cell(Cell{static_cast<i16>(x), static_cast<i16>(z)}), next);
            CHECK(made);
            CHECK(node == next);
            ++next;
        }
    const u32 corner = pack_cell(Cell{-1, -1});
    REQUIRE(index.find(corner) != nullptr);
    CHECK(*index.find(corner) == 0);
    const auto [again, made] = index.try_emplace(corner, 12345);
    CHECK_FALSE(made);
    CHECK(again == 0);
    for (u32 n = 0; n < next; n += 997) {
        const auto x = static_cast<i16>(static_cast<i32>(n % 100) - 1);
        const auto z = static_cast<i16>(static_cast<i32>(n / 100) - 1);
        const u32* found = index.find(pack_cell(Cell{x, z}));
        REQUIRE(found != nullptr);
        CHECK(*found == n);
    }
    index.set(corner, 7); // a snapshot's load sets each node
    CHECK(*index.find(corner) == 7);
    index.clear();
    CHECK(index.find(corner) == nullptr);
    CHECK(index.find(pack_cell(Cell{50, 50})) == nullptr);
    CHECK(index.try_emplace(pack_cell(Cell{50, 50}), 3) == std::pair<u32, bool>{3, true});
    CHECK(index.find(corner) == nullptr);
}

TEST_CASE("Across an open map the path is cells near its ends and portal jumps between, "
          "found in a few hundred expansions",
          "[path_search]") {
    World w;
    w.make_tables();
    PathQueue q;
    PathFinder f;
    w.aim(f, 0, {10, 10}, {203, 181}); // neither on a cluster line
    run(w, q, f);
    REQUIRE(f.has_result());
    CHECK(f.reached());
    const std::vector<Cell>& p = f.path();
    REQUIRE(p.size() >= 2);
    CHECK(p.front() == Cell{10, 10});
    CHECK(p.back() == Cell{203, 181});
    CHECK(steps_are_edges(*w.tables, 0, p));
    // Portal jumps in the middle, cell steps at the ends.
    bool jumped = false;
    for (size_t i = 1; i < p.size(); ++i)
        jumped = jumped || std::abs(p[i].x - p[i - 1].x) > 1 || std::abs(p[i].z - p[i - 1].z) > 1;
    CHECK(jumped);
    CHECK(std::abs(p[1].x - p[0].x) <= 1);
    CHECK(std::abs(p.back().x - p[p.size() - 2].x) <= 1);
    CHECK(q.search().expansions() < 600);
}

TEST_CASE("A gap in a wall passes the classes that fit it; a bigger one gets the way to the "
          "nearest place it can reach",
          "[path_search]") {
    World w;
    // A wall down x = 120-121, open at z = 100-103: four cells.
    w.grid.fill(oc::kLand, {120, 0, 122, 100}, true);
    w.grid.fill(oc::kLand, {120, 104, 122, static_cast<i32>(kMap)}, true);
    w.make_tables();
    for (const i32 cls : {0, 1}) {
        INFO("class " << cls);
        PathQueue q;
        PathFinder f;
        w.aim(f, cls, {40, 40}, {200, 40});
        run(w, q, f);
        REQUIRE(f.has_result());
        CHECK(f.reached());
        CHECK(steps_are_edges(*w.tables, cls, f.path()));
    }
    PathQueue q;
    PathFinder f;
    w.aim(f, 2, {40, 40}, {200, 40}); // 5 wide
    run(w, q, f);
    REQUIRE(f.has_result());
    CHECK_FALSE(f.reached());
    REQUIRE_FALSE(f.path().empty());
    // Its corner can come no nearer than 5 west of the wall, and it got
    // as far as the portals there.
    CHECK(f.path().back().x <= 115);
    CHECK(f.path().back().x >= 96);
}

TEST_CASE("A search spread over many ticks finds what one tick's search finds", "[path_search]") {
    World w;
    w.grid.fill(oc::kLand, {60, 20, 64, 230}, true);
    w.grid.fill(oc::kLand, {150, 0, 152, 200}, true);
    w.make_tables();
    PathQueue whole_q;
    PathFinder whole;
    w.aim(whole, 1, {20, 120}, {230, 30});
    CHECK(run(w, whole_q, whole) == 1);

    World w2;
    w2.grid.fill(oc::kLand, {60, 20, 64, 230}, true);
    w2.grid.fill(oc::kLand, {150, 0, 152, 200}, true);
    w2.make_tables(); // its clusters built on demand, a slice at a time
    PathQueue sliced_q;
    PathFinder sliced;
    w2.aim(sliced, 1, {20, 120}, {230, 30});
    CHECK(run(w2, sliced_q, sliced, 7) > 10);
    REQUIRE(whole.has_result());
    REQUIRE(sliced.has_result());
    CHECK(whole.reached());
    CHECK(sliced.reached() == whole.reached());
    CHECK(sliced.path() == whole.path());
}

TEST_CASE("Out of pathcap, or with no way there, a search hands back the way to the cell it "
          "found nearest",
          "[path_search]") {
    World w;
    // The goal walled in.
    w.grid.fill(oc::kLand, {96, 96, 112, 98}, true);
    w.grid.fill(oc::kLand, {96, 110, 112, 112}, true);
    w.grid.fill(oc::kLand, {96, 98, 98, 110}, true);
    w.grid.fill(oc::kLand, {110, 98, 112, 110}, true);
    w.make_tables();
    PathQueue q;
    PathFinder f;
    w.aim(f, 0, {20, 103}, {104, 103});
    run(w, q, f);
    REQUIRE(f.has_result());
    CHECK_FALSE(f.reached());
    CHECK(f.path().front() == Cell{20, 103});
    // Beside the walls, outside them.
    const Cell near = f.path().back();
    CHECK(f.heuristic(near) < 9.0f);
    CHECK_FALSE((near.x >= 96 && near.x < 112 && near.z >= 96 && near.z < 112));

    PathQueue q2;
    PathFinder capped;
    w.aim(capped, 0, {20, 20}, {200, 20}, SearchType::None, 5);
    run(w, q2, capped);
    REQUIRE(capped.has_result());
    CHECK_FALSE(capped.reached());
    CHECK(capped.path().front() == Cell{20, 20});
    CHECK(capped.path().back().x > 20);
}

TEST_CASE("The queue serves its travelers in turn, one in flight across ticks, and lets one "
          "go unheard",
          "[path_search]") {
    World w;
    w.make_tables();
    PathQueue q;
    PathFinder a;
    PathFinder b;
    PathFinder c;
    w.aim(a, 0, {10, 10}, {240, 240});
    w.aim(b, 0, {20, 10}, {30, 10});
    w.aim(c, 0, {30, 10}, {40, 10});
    q.queue(a);
    q.queue(b);
    q.queue(c);
    i32 budget = 20;
    q.work(*w.tables, budget);
    CHECK(budget <= 0);
    CHECK_FALSE(a.has_result()); // still in flight
    CHECK(q.search().traveler() == &a);
    CHECK(q.queued(b));
    q.cancel(a);
    q.cancel(c);
    CHECK_FALSE(q.queued(a));
    CHECK_FALSE(q.queued(c));
    budget = 100000;
    q.work(*w.tables, budget);
    CHECK_FALSE(a.has_result());
    CHECK(b.has_result());
    CHECK(b.reached());
    CHECK_FALSE(c.has_result());
    CHECK(budget > 0); // nobody left: the rest goes unspent
}

TEST_CASE("A finder searches cell by cell round its start and its goal (or round the unit, "
          "repathing), and inside the playable area by its span",
          "[path_search]") {
    World w;
    PathFinder f;
    w.aim(f, 1, {50, 50}, {150, 150});
    CHECK(f.should_search_rect({49, 49, 52, 52}));       // holds the start
    CHECK_FALSE(f.should_search_rect({80, 80, 90, 90})); // between
    CHECK(f.should_search_rect({145, 145, 151, 151}));   // overlaps the goal
    CHECK_FALSE(f.should_search_rect({52, 52, 52, 60})); // empty

    f.set_goal({{140, 140, 160, 160}, {145, 145, 155, 155}});
    CHECK_FALSE(f.should_search_rect({146, 146, 150, 150})); // inside the inner rect
    CHECK(f.is_goal({141, 141}));
    CHECK_FALSE(f.is_goal({150, 150}));
    CHECK_FALSE(f.is_goal({160, 150}));
    CHECK(f.heuristic({130, 150}) == 10.0f * 1.01f);

    // Repathing: the 16 x 16 round the unit, the last three times.
    PathFinder r;
    w.aim(r, 1, {50, 50}, {150, 150}, SearchType::Repath);
    CHECK(r.should_search_rect({40, 40, 45, 45}));
    CHECK_FALSE(r.should_search_rect({70, 70, 75, 75}));
    for (const i32 x : {80, 110, 140, 170})
        r.prepare(w.world(), SearchType::Repath, {x, 50}, static_cast<f32>(x) + 1.5f, 51.5f);
    CHECK(r.history().size() == 3);
    CHECK_FALSE(r.should_search_rect({40, 40, 45, 45})); // forgotten
    CHECK(r.should_search_rect({108, 50, 112, 52}));

    // The playable area: a 3-wide footprint keeps 3 cells from its edge,
    // but only once the unit is inside it.
    PathFinder b;
    PathWorld pw = w.world();
    pw.playable = {10, 10, 200, 200};
    b.set_unit(w.classes[1], 1, false);
    b.prepare(pw, SearchType::None, {50, 50}, 51.5f, 51.5f);
    f32 cost = 1.0f;
    CHECK_FALSE(b.in_bounds({50, 50}, {12, 50}, cost));
    CHECK(b.in_bounds({50, 50}, {13, 50}, cost));
    CHECK_FALSE(b.in_bounds({50, 50}, {50, 198}, cost));
    b.prepare(pw, SearchType::None, {5, 50}, 6.5f, 51.5f); // outside
    CHECK(b.in_bounds({5, 50}, {2, 50}, cost));
    pw.use_whole_map = true;
    b.prepare(pw, SearchType::None, {50, 50}, 51.5f, 51.5f);
    CHECK(b.in_bounds({50, 50}, {0, 50}, cost));

    // A goal's edge is walked as Moho walks it, through the line just past
    // its rect: a 1x1 goal on the last cell whose span fits still counts as
    // at the boundary.
    PathFinder e;
    pw = w.world();
    pw.playable = {0, 0, 64, 64};
    e.set_unit(w.classes[0], 0, false);
    e.prepare(pw, SearchType::None, {30, 30}, 30.5f, 30.5f);
    e.set_goal({{62, 30, 63, 31}, {}});
    CHECK_FALSE(e.goal_boundary_blocked());
    e.set_goal({{63, 30, 64, 31}, {}});
    CHECK(e.goal_boundary_blocked());
}

TEST_CASE("A unit standing inside an obstacle searches without its footprint test, so it can "
          "walk out of its cluster",
          "[path_search]") {
    World w;
    w.grid.fill(oc::kLand, {46, 46, 54, 54}, true); // a factory, say
    w.make_tables();
    PathQueue q;
    PathFinder f;
    w.aim(f, 0, {50, 50}, {100, 50});
    CHECK(f.can_traverse({47, 47})); // no footprint test: it doesn't fit where it stands
    run(w, q, f);
    REQUIRE(f.has_result());
    CHECK(f.reached());

    PathFinder g;
    w.aim(g, 0, {30, 50}, {100, 50});
    CHECK_FALSE(g.can_traverse({47, 47}));
    CHECK(g.can_traverse({30, 30}));
}

TEST_CASE("A traveler gone mid-search leaves its queue, and a queue gone first lets its "
          "travelers go",
          "[path_search]") {
    World w;
    w.make_tables();
    PathQueue q;
    PathFinder keep;
    w.aim(keep, 0, {20, 10}, {30, 10});
    {
        PathFinder gone;
        w.aim(gone, 0, {10, 10}, {240, 240});
        q.queue(gone);
        q.queue(keep);
        i32 budget = 20;
        q.work(*w.tables, budget); // `gone` in flight
        CHECK(q.search().traveler() == &gone);
        CHECK(gone.queue() == &q);
    }
    CHECK(q.search().traveler() == nullptr);
    i32 budget = 100000;
    q.work(*w.tables, budget);
    CHECK(keep.has_result());
    CHECK(keep.queue() == nullptr);

    PathFinder outlives;
    w.aim(outlives, 0, {20, 10}, {30, 10});
    {
        PathQueue short_lived;
        short_lived.queue(outlives);
        CHECK(outlives.queue() == &short_lived);
    }
    CHECK(outlives.queue() == nullptr); // and its destructor touches nothing

    // Queued on one queue, then another: it moves.
    PathQueue a;
    PathQueue b;
    PathFinder mover;
    w.aim(mover, 0, {20, 10}, {30, 10});
    a.queue(mover);
    b.queue(mover);
    CHECK_FALSE(a.queued(mover));
    CHECK(b.queued(mover));
}
