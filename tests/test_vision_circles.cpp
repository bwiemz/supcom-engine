#include <catch2/catch_test_macros.hpp>

#include "renderer/vision_renderer.hpp"
#include "sim/world_snapshot.hpp"

#include <vector>

using namespace osc;
using renderer::vision_circles;

namespace {

/// A world of three armies: 0 the focus army, 1 its ally, 2 its enemy.
struct World {
    sim::WorldSnapshot snap;

    World() {
        snap.armies.resize(3);
        for (auto& a : snap.armies) a.valid = true;
        snap.armies[0].allies = 1u << 1;
        snap.armies[1].allies = 1u << 0;
    }

    /// A unit of `army` at (x, z) with a vision radius (none for 0).
    sim::EntityRecord& unit(u32 id, i32 army, f32 x, f32 z, f32 vision, bool enabled = true) {
        sim::EntityRecord e;
        e.id = id;
        e.army = army;
        e.is_unit = true;
        e.position = {x, 0, z};
        if (vision > 0.0f) {
            e.intel_offset = static_cast<u32>(snap.intel.size());
            snap.intel.push_back({"Radar", 60.0f, true});
            snap.intel.push_back({"Vision", vision, enabled});
            e.intel_count = 2;
        }
        snap.entities.push_back(e);
        return snap.entities.back();
    }

    std::vector<renderer::RangeRing> circles(i32 focus) const {
        return vision_circles(sim::FrameView(&snap, &snap, 1.0f), focus);
    }
};

} // namespace

TEST_CASE("The fog's circles are the focus army's and its allies' units' vision, in whole units",
          "[renderer][fog]") {
    // UserUnit::Tick: a handle per unit with a vision range, visible when
    // the focus army calls the unit's army an ally (its own included)
    World w;
    w.unit(1, 0, 10.0f, 20.0f, 26.7f);
    w.unit(2, 1, 30.0f, 40.0f, 18.0f);
    w.unit(3, 2, 50.0f, 60.0f, 22.0f);
    w.unit(4, -1, 70.0f, 80.0f, 22.0f); // of no army
    w.unit(5, 0, 90.0f, 90.0f, 0.0f);   // no vision
    const auto c = w.circles(0);
    REQUIRE(c.size() == 2);
    CHECK(c[0].x == 10.0f);
    CHECK(c[0].z == 20.0f);
    CHECK(c[0].inner == 0.0f);
    CHECK(c[0].outer == 26.0f); // the intel's range is whole units
    CHECK(c[1].x == 30.0f);
    CHECK(c[1].outer == 18.0f);
}

TEST_CASE("A unit being built or dying clears no fog, but one with its vision off still does",
          "[renderer][fog]") {
    World w;
    w.unit(1, 0, 10.0f, 10.0f, 20.0f).is_being_built = true;
    w.unit(2, 0, 20.0f, 20.0f, 20.0f).is_dying = true;
    // The unit's handle reads the range alone, not the intel's toggle
    w.unit(3, 0, 30.0f, 30.0f, 20.0f, false);
    const auto c = w.circles(0);
    REQUIRE(c.size() == 1);
    CHECK(c[0].x == 30.0f);
}

TEST_CASE("An observer has no fog circles, and a unit's circle follows it between the ticks",
          "[renderer][fog]") {
    World w;
    w.unit(1, 0, 10.0f, 10.0f, 20.0f);
    CHECK(w.circles(-1).empty());

    World prev;
    prev.unit(1, 0, 0.0f, 0.0f, 20.0f);
    World cur;
    cur.unit(1, 0, 10.0f, 4.0f, 20.0f);
    const auto c = vision_circles(sim::FrameView(&prev.snap, &cur.snap, 0.5f), 0);
    REQUIRE(c.size() == 1);
    CHECK(c[0].x == 5.0f);
    CHECK(c[0].z == 2.0f);
}

TEST_CASE("A script entity's vision draws no fog circle", "[renderer][fog]") {
    // Only units are taken: which army a VizMarker's user entity has isn't
    // known, so its circle would be a guess
    World w;
    w.unit(1, 0, 10.0f, 10.0f, 20.0f).is_unit = false;
    CHECK(w.circles(0).empty());
}
