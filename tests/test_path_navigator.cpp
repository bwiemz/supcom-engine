// Following a path (roadmap item 4c-2b): Moho's grid line walk and
// footprint sweeps (faf-re COGrid.cpp, CAiPathNavigator.cpp), and its path
// navigator driving a stand-in unit from its army's queue to a goal.

#include <catch2/catch_test_macros.hpp>

#include "blueprints/footprint.hpp"
#include "map/heightmap.hpp"
#include "map/terrain.hpp"
#include "sim/occupancy.hpp"
#include "sim/path_navigator.hpp"
#include "sim/path_search.hpp"
#include "sim/path_tables.hpp"
#include "sim/path_walk.hpp"

#include <cmath>
#include <cstdlib>
#include <memory>
#include <vector>

using osc::f32;
using osc::i32;
using osc::u32;
using osc::u8;
using osc::blueprints::NamedFootprint;
using osc::sim::OccupancyGrid;
using osc::sim::PathTables;
using osc::sim::path::Cell;
using osc::sim::path::GridLine;
using osc::sim::path::NavUnit;
using osc::sim::path::PathNavigator;
using osc::sim::path::PathQueue;
using osc::sim::path::PathWorld;
namespace oc = osc::blueprints::occupancy;
namespace path = osc::sim::path;
using State = PathNavigator::State;

namespace {

constexpr u32 kMap = 128;

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

std::vector<Cell> walk(GridLine line) {
    std::vector<Cell> cells;
    for (int i = 0; i < 64; ++i) {
        cells.push_back(line.cell());
        line.advance();
        if (line.beyond_end()) break;
    }
    return cells;
}

/// A stand-in unit on a 128 x 128 map, following its navigator's target at
/// `speed` a tick, its army's queue served first each tick (as the sim's
/// tick runs them).
struct Walk {
    std::unique_ptr<osc::map::Terrain> terrain = flat_terrain(kMap);
    OccupancyGrid grid{kMap, kMap};
    std::vector<NamedFootprint> classes{land_class(1), land_class(3)};
    std::unique_ptr<PathTables> tables;
    PathQueue queue;
    PathNavigator nav;
    f32 x = 0;
    f32 z = 0;
    i32 cls = 0;
    u32 layer = 1;
    bool frozen = false;
    /// Steered at a cell its footprint can't take (the move refused, as
    /// Moho's unit motion refuses it).
    bool bumped = false;
    int ticks = 0;
    bool moved = false;

    PathWorld world() const {
        return {terrain.get(),
                &grid,
                {0, 0, static_cast<i32>(kMap), static_cast<i32>(kMap)},
                false,
                100000};
    }
    void start(i32 c, f32 sx, f32 sz, Cell goal) {
        tables = std::make_unique<PathTables>(classes, *terrain, grid);
        cls = c;
        x = sx;
        z = sz;
        nav.set_unit(classes[static_cast<size_t>(c)], c, false);
        nav.set_goal({{goal.x, goal.z, goal.x + 1, goal.z + 1}, {}}, x, z);
    }
    /// One tick; false once the navigator is done.
    bool step(f32 speed = 1.0f) {
        ++ticks;
        i32 budget = 2500;
        queue.work(*tables, budget);
        const f32 px = x;
        const f32 pz = z;
        nav.update(NavUnit{x, z, moved, false, false, layer}, world(), queue);
        if (nav.state() == State::Idle || nav.state() == State::Failed) return false;
        if (!frozen) {
            const f32 dx = nav.target_x() - x;
            const f32 dz = nav.target_z() - z;
            const f32 d = std::sqrt(dx * dx + dz * dz);
            if (d > 0) {
                const f32 s = std::min(speed, d) / d;
                const NamedFootprint& fp = classes[static_cast<size_t>(cls)];
                const auto at = osc::sim::footprint_rect(fp, x + dx * s, z + dz * s);
                if (osc::sim::footprint_fits(fp, *terrain, grid, at.x0, at.z0) != 0) {
                    x += dx * s;
                    z += dz * s;
                } else {
                    // Refused, it settles and its steering asks for the way.
                    bumped = true;
                    nav.request_repath();
                }
            }
        }
        moved = x != px || z != pz;
        return true;
    }
    int run(int limit = 2000) {
        while (ticks < limit && step()) {}
        return ticks;
    }
};

} // namespace

TEST_CASE("Moho's grid line walks the cells a segment crosses, in order, either way",
          "[path_navigator]") {
    CHECK(walk(GridLine(1, 0.5f, 0.5f, 3.5f, 0.5f)) ==
          std::vector<Cell>{{0, 0}, {1, 0}, {2, 0}, {3, 0}});
    CHECK(walk(GridLine(1, 3.5f, 0.5f, 0.5f, 0.5f)) ==
          std::vector<Cell>{{3, 0}, {2, 0}, {1, 0}, {0, 0}});
    // A diagonal steps as a staircase, z first on a tie.
    CHECK(walk(GridLine(1, 0.5f, 0.5f, 2.5f, 2.5f)) ==
          std::vector<Cell>{{0, 0}, {0, 1}, {1, 1}, {1, 2}, {2, 2}});
    // The first cell is a cell even when the segment is a point.
    CHECK(walk(GridLine(1, 5.5f, 7.5f, 5.5f, 7.5f)).front() == Cell{5, 7});
}

TEST_CASE("A footprint sweep refuses a corner cut and a wall across the way", "[path_navigator]") {
    const auto terrain = flat_terrain(64);
    OccupancyGrid grid(64, 64);
    const NamedFootprint one = land_class(1);
    grid.fill(oc::kLand, {11, 10, 12, 11}, true);
    // (10,10) to (11,11) squeezes past (11,10).
    CHECK_FALSE(path::cell_step_clear(one, false, *terrain, grid, {10, 10}, {11, 11}));
    CHECK(path::cell_step_clear(one, false, *terrain, grid, {10, 10}, {9, 11}));
    // A long straight step through a wall, and beside it.
    grid.fill(oc::kLand, {20, 0, 21, 30}, true);
    CHECK_FALSE(path::cell_step_clear(one, false, *terrain, grid, {15, 10}, {25, 10}));
    CHECK(path::cell_step_clear(one, false, *terrain, grid, {15, 40}, {25, 40}));
    // A 3-wide one is caught where a 1-wide one passes.
    const NamedFootprint three = land_class(3);
    CHECK(path::cell_step_clear(one, false, *terrain, grid, {15, 31}, {25, 31}));
    CHECK_FALSE(path::cell_step_clear(three, false, *terrain, grid, {15, 29}, {25, 29}));
}

TEST_CASE("A navigator asks its army, then takes its unit to the goal, cutting straight where "
          "its footprint slides",
          "[path_navigator]") {
    Walk w;
    w.start(0, 10.5f, 10.5f, {100, 90});
    CHECK(w.nav.state() == State::Thinking);
    w.step();
    CHECK(w.nav.state() == State::Searching); // asked on its first update
    bool shortcut = false;
    while (w.ticks < 400 && w.step())
        shortcut = shortcut || std::abs(w.nav.target().x - w.nav.current().x) > 1 ||
                   std::abs(w.nav.target().z - w.nav.current().z) > 1;
    CHECK(w.nav.state() == State::Idle);
    CHECK(w.nav.current() == Cell{100, 90});
    CHECK(shortcut);
    CHECK_FALSE(w.bumped);
    CHECK(w.ticks < 200); // about 130 cells at one a tick
}

TEST_CASE("Through a gap in a wall: steered at a portal across the wall, the unit is pushed "
          "back and asks the way there cell by cell, as Moho's steering makes it",
          "[path_navigator]") {
    for (const i32 cls : {0, 1}) {
        INFO("class " << cls);
        Walk w;
        w.grid.fill(oc::kLand, {60, 0, 62, 70}, true);
        w.grid.fill(oc::kLand, {60, 76, 62, static_cast<i32>(kMap)}, true);
        w.start(cls, 30.5f, 20.5f, {100, 20});
        w.run();
        CHECK(w.nav.state() == State::Idle);
        CHECK(w.nav.current() == Cell{100, 20});
        CHECK(w.ticks < 300);
    }
}

TEST_CASE("A unit that can't get there stops short, and one held still gives up after a "
          "second and a half",
          "[path_navigator]") {
    Walk w;
    w.grid.fill(oc::kLand, {80, 80, 96, 82}, true);
    w.grid.fill(oc::kLand, {80, 94, 96, 96}, true);
    w.grid.fill(oc::kLand, {80, 82, 82, 94}, true);
    w.grid.fill(oc::kLand, {94, 82, 96, 94}, true);
    w.start(0, 20.5f, 87.5f, {88, 88});
    w.run();
    CHECK((w.nav.state() == State::Idle || w.nav.state() == State::Failed));
    CHECK(w.ticks < 2000);
    // Beside the walls, outside them: the side nearest the goal (the east,
    // 8 cells off where the west is 9), as the search's fallback finds it.
    const Cell end = w.nav.current();
    CHECK(end.x >= 79);
    CHECK(end.x <= 96);
    CHECK(end.z >= 79);
    CHECK(end.z <= 96);
    CHECK_FALSE((end.x >= 80 && end.x < 96 && end.z >= 80 && end.z < 96));

    Walk held;
    held.start(0, 20.5f, 20.5f, {100, 20});
    held.frozen = true;
    held.run();
    CHECK(held.nav.state() == State::Idle);
    CHECK(held.ticks < 50);
    CHECK(held.nav.current() == Cell{20, 20});
}

TEST_CASE("A new layer asks for the way on; the answer goes in front of what is left",
          "[path_navigator]") {
    Walk w;
    w.start(0, 10.5f, 10.5f, {100, 10});
    for (int i = 0; i < 20; ++i) w.step();
    REQUIRE(w.nav.state() == State::HasPath);
    const size_t left = w.nav.path().size();
    w.layer = 8; // into the water, say
    w.step();
    CHECK(w.nav.state() == State::Continuing);
    w.step();
    CHECK(w.nav.state() == State::HasPath);
    CHECK(w.nav.path().size() + 2 >= left);
    w.run();
    CHECK(w.nav.current() == Cell{100, 10});
}
