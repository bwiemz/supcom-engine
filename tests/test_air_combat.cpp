// A winged aircraft's attack runs (sim/air_combat): Moho's bomb release
// point and gate, and the combat state machine's transitions
// (docs/plans/2026-10-05-air-attack-runs-design.md).

#include <catch2/catch_test_macros.hpp>

#include "sim/air_combat.hpp"
#include "sim/sim_random.hpp"

#include <cmath>

using osc::f32;
using osc::u32;
using osc::sim::AirCombatMode;
using osc::sim::AirCombatRules;
using osc::sim::AirCombatState;
using osc::sim::AirTacticsInput;
using osc::sim::SimRandom;
using osc::sim::Vector3;

namespace {

/// A UEF T1 bomber's rules (UEA0103's blueprint).
AirCombatRules bomber_rules() {
    AirCombatRules r;
    r.winged = true;
    r.min_airspeed = 10.0f;
    r.combat_turn_speed = 0.7f;
    r.tight_turn_multiplier = 0.0f;
    r.engage_distance = 50.0f;
    r.break_off_trigger = 20.0f;
    r.break_off_distance = 18.0f;
    r.break_off_if_near_new_target = true;
    return r;
}

/// The bomber at the origin, 18 up, nose north (+z), the target `ahead`
/// along z (negative: behind), `side` across.
AirTacticsInput at_range(f32 ahead, f32 side = 0.0f, u32 tick = 100) {
    AirTacticsInput in;
    in.position = {0.0f, 18.0f, 0.0f};
    in.heading = 0.0f;
    in.target = {side, 0.0f, ahead};
    in.tick = tick;
    in.max_airspeed = 10.0f;
    in.break_off_trigger = 20.0f;
    in.break_off_distance = 18.0f;
    return in;
}

AirCombatMode mode(const AirCombatState& st) {
    return static_cast<AirCombatMode>(st.state);
}

} // namespace

TEST_CASE("calc_bomb_drop: the point a bomb with the aircraft's velocity leaves from",
          "[air_combat]") {
    // 18 up at 10 a second: t = sqrt(36 / 4.9), released 10 t short.
    const auto release = osc::sim::calc_bomb_drop({10.0f, 0.0f, 0.0f}, {0.0f, 18.0f, 0.0f},
                                                  {40.0f, 0.0f, 0.0f}, 4.9f);
    REQUIRE(release);
    CHECK(std::fabs(release->x - (40.0f - 10.0f * std::sqrt(36.0f / 4.9f))) < 1e-3f);
    CHECK(std::fabs(release->z) < 1e-6f);
    // Climbing at 2 a second takes the speed's size (Moho's |v.up|).
    const auto climbing = osc::sim::calc_bomb_drop({10.0f, 2.0f, 0.0f}, {0.0f, 18.0f, 0.0f},
                                                   {40.0f, 0.0f, 0.0f}, 4.9f);
    REQUIRE(climbing);
    const f32 t = (2.0f + std::sqrt(2.0f * 18.0f * 4.9f + 4.0f)) / 4.9f;
    CHECK(std::fabs(climbing->x - (40.0f - 10.0f * t)) < 1e-3f);
    // Below its target, nothing falls to it.
    CHECK_FALSE(osc::sim::calc_bomb_drop({10.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f},
                                         {40.0f, 18.0f, 0.0f}, 4.9f));
}

TEST_CASE("bomb_release_ok: only at the release point", "[air_combat]") {
    const Vector3 pos{0.0f, 18.0f, 0.0f}; // nose north
    const Vector3 target{0.0f, 0.0f, 27.0f};
    // Beyond the threshold: not yet.
    CHECK_FALSE(osc::sim::bomb_release_ok(pos, 0.0f, {0.0f, 0.0f, 3.1f}, target, 3.0f));
    // Within half of it: drop.
    CHECK(osc::sim::bomb_release_ok(pos, 0.0f, {0.0f, 0.0f, 1.4f}, target, 3.0f));
    // Between: the point must be behind or abeam, and the target ahead.
    CHECK(osc::sim::bomb_release_ok(pos, 0.0f, {0.0f, 0.0f, -2.0f}, target, 3.0f));
    CHECK_FALSE(osc::sim::bomb_release_ok(pos, 0.0f, {0.0f, 0.0f, 2.0f}, target, 3.0f));
    CHECK_FALSE(
        osc::sim::bomb_release_ok(pos, 0.0f, {0.0f, 0.0f, -2.0f}, {0.0f, 0.0f, 0.5f}, 3.0f));
}

TEST_CASE("air_tactics: Moho's combat state machine", "[air_combat]") {
    const AirCombatRules r = bomber_rules();
    SimRandom rng(7);

    SECTION("a target ahead is a run") {
        AirCombatState st;
        osc::sim::air_tactics(st, r, at_range(40.0f), rng);
        CHECK(mode(st) == AirCombatMode::Combat);
    }
    SECTION("a target to the side is a turn, held 3 to 6 seconds") {
        AirCombatState st;
        osc::sim::air_tactics(st, r, at_range(10.0f, 40.0f), rng);
        const auto m = mode(st);
        CHECK((m == AirCombatMode::CombatTurn || m == AirCombatMode::CombatTurnB ||
               m == AirCombatMode::Realign));
        CHECK(st.timeout_tick >= 130);
        CHECK(st.timeout_tick < 160);
    }
    SECTION("closing within BreakOffTrigger (3-D) breaks off, straight on for a while") {
        AirCombatState st;
        st.state = static_cast<osc::u8>(AirCombatMode::Combat);
        osc::sim::air_tactics(st, r, at_range(5.0f), rng); // 18.7 away
        CHECK(mode(st) == AirCombatMode::BreakOff);
        // [ceil(18 / 10 * 10), int(18 * 1.5)) ticks
        CHECK(st.timeout_tick >= 118);
        CHECK(st.timeout_tick < 127);
        // It holds the break-off while it lasts, even lined up.
        osc::sim::air_tactics(st, r, at_range(40.0f, 0.0f, 110), rng);
        CHECK(mode(st) == AirCombatMode::BreakOff);
    }
    SECTION("a new target already close breaks off at once") {
        AirCombatState st;
        osc::sim::air_tactics(st, r, at_range(10.0f), rng);
        CHECK(mode(st) == AirCombatMode::BreakOff);
    }
    SECTION("ten seconds of turning force a break-off") {
        AirCombatState st;
        st.state = static_cast<osc::u8>(AirCombatMode::CombatTurnB);
        st.sustained_turn_ticks = 101;
        osc::sim::air_tactics(st, r, at_range(10.0f, 40.0f), rng);
        CHECK(mode(st) == AirCombatMode::BreakOff);
    }
    SECTION("an aircraft ahead, going its way, is a chase that ends when it falls behind") {
        AirCombatState st;
        AirTacticsInput in = at_range(60.0f);
        in.target_in_air = true;
        in.target_moved = true;
        in.target_heading = 0.1f;
        osc::sim::air_tactics(st, r, in, rng);
        CHECK(mode(st) == AirCombatMode::NormalTurn);
        osc::sim::air_tactics(st, r, at_range(-30.0f, 5.0f), rng);
        CHECK(mode(st) == AirCombatMode::BreakOff);
    }
}
