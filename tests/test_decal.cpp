#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "map/visibility_grid.hpp"
#include "sim/decal.hpp"

#include <cmath>
#include <vector>

using namespace osc;
using Catch::Approx;
using sim::DecalSpec;

namespace {

constexpr f32 kQuarter = 1.5707964f;

/// Armies for the sight rules: each allied to itself and to those listed,
/// detecting what `sees` says.
struct FakeArmies final : sim::DecalArmies {
    std::vector<bool> civilians;
    std::vector<u32> allies; ///< bit j: army i counts j among its allies
    std::vector<bool> sees;  ///< army i's recon detects the decal

    explicit FakeArmies(size_t n) : civilians(n, false), allies(n, 0), sees(n, false) {
        for (size_t i = 0; i < n; ++i) allies[i] = 1u << i;
    }
    void ally(size_t a, size_t b) {
        allies[a] |= 1u << b;
        allies[b] |= 1u << a;
    }
    size_t count() const override { return civilians.size(); }
    bool exists(size_t) const override { return true; }
    bool civilian(size_t i) const override { return civilians[i]; }
    bool allied(size_t a, size_t b) const override { return (allies[a] >> b & 1u) != 0; }
    bool detects(size_t observer, const sim::DecalBounds&, f32) const override {
        return sees[observer];
    }
};

DecalSpec spec_from(i32 army, bool splat, u32 remove_tick = 0) {
    DecalSpec s;
    s.army = army;
    s.splat = splat;
    s.remove_tick = remove_tick;
    s.size_x = s.size_z = 4;
    return s;
}

} // namespace

TEST_CASE("A runtime decal is centred on its transform", "[decal]") {
    // An 8 by 4 decal at (24, 0, 24), a quarter turn: its sides run along
    // right (0, 0, -1) and forward (1, 0, 0), so its corner is the centre
    // less half of each: (24 - 2, 24 + 4). Its turn is -h.
    const DecalSpec s =
        sim::make_decal_spec({24, 3, 24}, sim::heading_quaternion(kQuarter), 8, 4, 0, 50);
    CHECK(s.position.x == Approx(22.0f).margin(1e-5));
    CHECK(s.position.y == Approx(3.0f));
    CHECK(s.position.z == Approx(28.0f).margin(1e-5));
    CHECK(s.rotation_y == Approx(-kQuarter));
    CHECK(s.size_x == 8.0f);
    CHECK(s.size_z == 4.0f);
    CHECK(s.remove_tick == 0);

    // Its footprint's middle is the centre.
    const sim::DecalBounds b = sim::decal_bounds(s);
    CHECK((b.min_x + b.max_x) * 0.5f == Approx(24.0f));
    CHECK((b.min_z + b.max_z) * 0.5f == Approx(24.0f));
    CHECK(b.max_x - b.min_x == Approx(4.0f));
    CHECK(b.max_z - b.min_z == Approx(8.0f));
}

TEST_CASE("A splat on a pitched bone: its corner by the flattened axes", "[decal]") {
    // A bone headed an eighth and pitched: its sides are its x and z axes
    // flattened onto the ground (the heading's), its turn Moho's formula,
    // -atan2(2(xz + wy), 1 - 2(z^2 + y^2)): its z axis's x over its x axis's.
    const f32 h = 0.7853982f;
    const f32 p = 0.3f;
    const sim::Quaternion yaw = sim::heading_quaternion(h);
    const sim::Quaternion pitch{std::sin(p * 0.5f), 0, 0, std::cos(p * 0.5f)};
    const DecalSpec s =
        sim::make_decal_spec({10, 0, 10}, sim::quat_multiply(yaw, pitch), 2, 6, 0, 0);
    CHECK(s.position.x == Approx(10.0f - std::cos(h) * 1.0f - std::sin(h) * 3.0f).margin(1e-5));
    CHECK(s.position.z == Approx(10.0f + std::sin(h) * 1.0f - std::cos(h) * 3.0f).margin(1e-5));
    CHECK(s.rotation_y == Approx(-std::atan2(std::sin(h) * std::cos(p), std::cos(h))).margin(1e-5));
}

TEST_CASE("A runtime decal ends at floor(duration x 10) ticks", "[decal]") {
    const auto ends = [](f32 duration) {
        return sim::make_decal_spec({}, sim::heading_quaternion(0), 1, 1, duration, 100)
            .remove_tick;
    };
    CHECK(ends(0.5f) == 105);
    CHECK(ends(0.19f) == 101);
    CHECK(ends(0.05f) == 100);
    CHECK(ends(0.0f) == 0);
    CHECK(ends(-1.0f) == 0);
}

TEST_CASE("Who sees a splat as it is made", "[decal]") {
    FakeArmies a(4);
    a.civilians[3] = true;
    a.ally(0, 1);
    a.ally(3, 2);
    // From a non-civilian army: every army, whatever it can see.
    CHECK(sim::decal_sight_at_creation(spec_from(0, true), a) == 0b1111u);
    // From a civilian army: its allies (itself included).
    CHECK(sim::decal_sight_at_creation(spec_from(3, true), a) == 0b1100u);
    // With no army, it follows the decals' rule: every non-civilian army,
    // and its allies (the civilian 3, army 2's).
    CHECK(sim::decal_sight_at_creation(spec_from(-1, true), a) == 0b1111u);
}

TEST_CASE("Who sees a decal as it is made", "[decal]") {
    FakeArmies a(4);
    a.civilians[3] = true;
    a.ally(0, 1);
    // No one detects it: its source and the source's allies.
    CHECK(sim::decal_sight_at_creation(spec_from(0, false), a) == 0b0011u);
    // An enemy that detects it, and that enemy's allies (from its own
    // index up, as Moho loops).
    a.sees[2] = true;
    CHECK(sim::decal_sight_at_creation(spec_from(0, false), a) == 0b0111u);
    a.ally(2, 3);
    CHECK(sim::decal_sight_at_creation(spec_from(0, false), a) == 0b1111u);
    // A civilian army doesn't look for itself.
    FakeArmies b(3);
    b.civilians[2] = true;
    b.sees[2] = true;
    CHECK(sim::decal_sight_at_creation(spec_from(0, false), b) == 0b0001u);
    // With no army, every non-civilian army sees it.
    CHECK(sim::decal_sight_at_creation(spec_from(-1, false), b) == 0b0011u);
    // Army 1 detects it, army 0 (its ally, lower) doesn't: only 1 and up
    // take it (PropagateVisibilityToObserverAllies starts at the observer).
    FakeArmies c(3);
    c.ally(0, 1);
    c.sees[1] = true;
    CHECK(sim::decal_sight_at_creation(spec_from(2, false), c) == 0b0110u);
}

TEST_CASE("Each tick one army in turn looks at the decals", "[decal]") {
    FakeArmies a(3);
    a.ally(1, 2);
    const DecalSpec d = spec_from(0, false);
    // Tick 4: army 1's turn (4 % 3). It can't see it yet.
    CHECK(sim::decal_sight_on_tick(d, 0b001u, 0, 4, a) == 0b001u);
    a.sees[1] = true;
    // Army 2's turn: it can't see it (the grid's its own).
    CHECK(sim::decal_sight_on_tick(d, 0b001u, 0, 5, a) == 0b001u);
    // Army 1's: it and its allies.
    CHECK(sim::decal_sight_on_tick(d, 0b001u, 0, 7, a) == 0b111u);
    // Flags are never cleared: a later look without sight keeps them.
    a.sees[1] = false;
    CHECK(sim::decal_sight_on_tick(d, 0b111u, 0, 10, a) == 0b111u);
    // A civilian's turn does nothing.
    a.civilians[1] = true;
    a.sees[1] = true;
    CHECK(sim::decal_sight_on_tick(d, 0b001u, 0, 7, a) == 0b001u);
}

TEST_CASE("A splat is looked at again only in its first 10 ticks, with a lifetime", "[decal]") {
    FakeArmies a(2);
    a.sees[1] = true;
    // From a civilian army, so only its allies saw it at first.
    a.civilians[0] = true;
    const DecalSpec timed = spec_from(0, true, 100);
    CHECK(sim::decal_sight_on_tick(timed, 0b01u, 20, 29, a) == 0b11u);
    CHECK(sim::decal_sight_on_tick(timed, 0b01u, 20, 31, a) == 0b01u);
    const DecalSpec lasting = spec_from(0, true, 0);
    CHECK(sim::decal_sight_on_tick(lasting, 0b01u, 20, 21, a) == 0b01u);
}

TEST_CASE("A rectangle's vision is any cell it covers", "[decal]") {
    map::VisibilityGrid grid(128, 128);                    // cells of 16
    grid.paint_circle(0, 40, 40, 1, map::VisFlag::Vision); // cell (2, 2)
    CHECK(grid.any_vision(33, 33, 36, 36, 0));
    CHECK(grid.any_vision(20, 20, 32.5f, 32.5f, 0)); // ceil reaches cell 2
    CHECK_FALSE(grid.any_vision(20, 20, 32, 32, 0)); // ends where cell 2 starts
    CHECK_FALSE(grid.any_vision(50, 50, 60, 60, 0));
    CHECK_FALSE(grid.any_vision(33, 33, 36, 36, 1));
    CHECK_FALSE(grid.any_vision(-40, -40, -20, -20, 0));
}
