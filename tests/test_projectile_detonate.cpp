// A projectile's detonate band, as Moho's CheckCollision ends (Steam's exe 0x006A476E, faf-re
// Projectile.cpp): its height over the ground or the water where its step ends, against its
// DetonateBelowHeight on the way down and its DetonateAboveHeight on the way up, bursts it
// 'Air' where the step crosses the height over the ground, in place of anything else the
// step met. Flak bursts at its target's height (DetonatesAtTargetHeight), and a fragmentation
// shell splits as it comes down.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "map/heightmap.hpp"
#include "map/terrain.hpp"
#include "sim/entity_registry.hpp"
#include "sim/projectile.hpp"

#include <memory>
#include <vector>

using Catch::Approx;
using osc::f32;
using osc::sim::EntityRegistry;
using osc::sim::Projectile;

namespace {

constexpr f32 kGround = 1000.0f / 128.0f; // 7.8125

/// A shot at (20, y, 20) over flat ground, moving straight up or down at `vy`, stepped once.
struct Shot {
    osc::map::Terrain terrain;
    EntityRegistry reg;
    Projectile* p = nullptr;

    explicit Shot(f32 y, f32 vy, f32 water = 0.0f)
        : terrain(osc::map::Heightmap(64, 64, 1.0f / 128.0f, std::vector<osc::u16>(65 * 65, 1000)),
                  water, water > 0.0f) {
        reg.init_spatial_grid(64, 64);
        auto shot = std::make_unique<Projectile>();
        shot->set_position({20.0f, y, 20.0f});
        shot->velocity = {0.0f, vy, 0.0f};
        p = static_cast<Projectile*>(reg.find(reg.register_entity(std::move(shot))));
    }

    /// One tick, its move committed (an impact sets where it is)
    void step() {
        p->update(0.1, reg, nullptr, &terrain, 1);
        reg.advance_coords();
    }
};

} // namespace

TEST_CASE("Flak bursts where its step rises through its height over the ground", "[projectile]") {
    Shot s(12.0f, 300.0f); // up 30 this step
    s.p->detonate_above_override = 10.0f;
    s.step();
    REQUIRE(s.p->impact_waiting());
    CHECK(s.p->impact_kind == "Air");
    // (10 + ground - 12) / 30 of the step: 10 over the ground
    CHECK(s.p->impact_interpolation == Approx((10.0f + kGround - 12.0f) / 30.0f));
    CHECK(s.p->position().y == Approx(kGround + 10.0f));
}

TEST_CASE("A shell coming down bursts at its height, without rising past it first, and not on "
          "the ground",
          "[projectile]") {
    Shot s(kGround + 8.0f, -100.0f); // down 10: the step ends 2 under the ground
    s.p->detonate_below_height = 3.0f;
    s.step();
    REQUIRE(s.p->impact_waiting());
    CHECK(s.p->impact_kind == "Air");
    CHECK(s.p->impact_interpolation == Approx(0.5f));
    CHECK(s.p->position().y == Approx(kGround + 3.0f));
}

TEST_CASE("A burst is at least a tenth of the step along, and at most all of it", "[projectile]") {
    // Just under the band: a hundredth of the step would reach it, a tenth is taken
    Shot near(kGround + 9.9f, 100.0f);
    near.p->detonate_above_override = 10.0f;
    near.step();
    REQUIRE(near.p->impact_waiting());
    CHECK(near.p->impact_interpolation == Approx(0.1f));
    CHECK(near.p->position().y == Approx(kGround + 10.9f));

    // Over deep water the height is the water's, the solve the ground's: past the step
    Shot sea(30.0f, -80.0f, 20.0f); // ends at 22, 2 over the water
    sea.p->detonate_below_height = 3.0f;
    sea.step();
    REQUIRE(sea.p->impact_waiting());
    CHECK(sea.p->impact_interpolation == Approx(1.0f));
    CHECK(sea.p->position().y == Approx(22.0f));
}

TEST_CASE("A detonate height set below zero leaves the blueprint's; one set at or above it "
          "stands in its place",
          "[projectile]") {
    Shot bp(12.0f, 300.0f);
    bp.p->detonate_above_height = 10.0f;
    bp.p->detonate_above_override = -1.0f;
    bp.step();
    REQUIRE(bp.p->impact_waiting());
    CHECK(bp.p->position().y == Approx(kGround + 10.0f));

    Shot set(12.0f, 300.0f);
    set.p->detonate_above_height = 10.0f;
    set.p->detonate_above_override = 20.0f;
    set.step();
    REQUIRE(set.p->impact_waiting());
    CHECK(set.p->position().y == Approx(kGround + 20.0f));

    // Going the band's other way, nothing
    Shot down(kGround + 30.0f, -50.0f);
    down.p->detonate_above_override = 10.0f;
    down.step();
    CHECK_FALSE(down.p->impact_waiting());
}
