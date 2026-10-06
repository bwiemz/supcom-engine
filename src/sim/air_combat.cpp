#include "sim/air_combat.hpp"

#include "core/dmath.hpp"
#include "map/terrain.hpp"
#include "sim/sim_random.hpp"
#include "sim/sim_state.hpp"

#include <algorithm>
#include <cmath>

namespace osc::sim {

namespace {

constexpr f32 kPi = 3.14159265f;
constexpr f32 kTwoPi = 6.28318530f;

f32 wrap_angle(f32 a) {
    while (a > kPi) a -= kTwoPi;
    while (a < -kPi) a += kTwoPi;
    return a;
}

/// Moho's RandomUniformIntRange: [lo, hi); lo when the range is empty.
i32 uniform(SimRandom& rng, i32 lo, i32 hi) {
    return hi <= lo ? lo : static_cast<i32>(rng.next_int(lo, hi - 1));
}

/// Whether `p` is in the playable area, `margin` inside its edge (the whole
/// map counts when there is none).
bool in_map(const SimState& sim, const Vector3& p, f32 margin) {
    if (!sim.has_playable_rect()) return true;
    return p.x >= sim.playable_x0() + margin && p.x <= sim.playable_x1() - margin &&
           p.z >= sim.playable_z0() + margin && p.z <= sim.playable_z1() - margin;
}

} // namespace

void air_tactics(AirCombatState& st, const AirCombatRules& r, const AirTacticsInput& in,
                 SimRandom& rng) {
    using M = AirCombatMode;
    const auto prev = static_cast<M>(st.state);
    const f32 dx = in.target.x - in.position.x;
    const f32 dy = in.target.y - in.position.y;
    const f32 dz = in.target.z - in.position.z;
    const f32 horz = std::sqrt(dx * dx + dz * dz);
    const f32 dist = std::sqrt(horz * horz + dy * dy); // 3-D, as Moho's break-off test
    const f32 hx = osc::dmath::sin(in.heading);
    const f32 hz = osc::dmath::cos(in.heading);

    const auto break_off = [&] {
        st.state = static_cast<u8>(M::BreakOff);
        const f32 speed = std::max(in.max_airspeed, 1e-3f);
        const auto lo = static_cast<i32>(std::ceil(in.break_off_distance / speed * 10.0f));
        const auto hi = static_cast<i32>(static_cast<f32>(lo) * r.random_break_off_distance_mult);
        st.timeout_tick = in.tick + static_cast<u32>(std::max(0, uniform(rng, lo, hi)));
    };

    if (prev == M::ReturnToMap && !in.in_map_by_5) return;
    // The target's bearing against the nose, flat (none straight overhead).
    const f32 hdot = horz > 0.0f ? (dx * hx + dz * hz) / horz : 0.0f;
    const bool aligned = hdot > (prev == M::CombatTurn ? 0.0f : 0.866f);
    if (in.target_in_air && !in.in_map) {
        st.state = static_cast<u8>(M::ReturnToMap);
        return;
    }
    if ((prev == M::Combat && in.break_off_trigger > dist) ||
        (r.break_off_if_near_new_target && prev == M::None && in.break_off_distance > horz) ||
        (prev == M::NormalTurn && hdot < 0.0f) ||
        st.sustained_turn_ticks >
            static_cast<i32>(std::floor(r.sustained_turn_threshold * 10.0f))) {
        break_off();
        return;
    }
    const bool timeout_active = st.timeout_tick >= in.tick;
    if (timeout_active && (!aligned || prev == M::BreakOff)) return;
    if (!aligned) {
        bool roll = true;
        if (prev == M::NormalTurn) {
            // Still ahead in three dimensions: the turn goes on.
            const f32 dot3 = dist > 0.0f ? (dx * hx + dz * hz) / dist : 0.0f;
            if (dot3 >= 0.0f) roll = false;
        }
        if (roll) {
            st.state = static_cast<u8>(
                uniform(rng, static_cast<i32>(M::CombatTurn), static_cast<i32>(M::BreakOff)));
            const auto lo =
                static_cast<i32>(std::floor(r.random_min_change_combat_state_time * 10.0f));
            const auto hi =
                static_cast<i32>(std::floor(r.random_max_change_combat_state_time * 10.0f));
            st.timeout_tick = in.tick + static_cast<u32>(std::max(0, uniform(rng, lo, hi)));
        }
        return;
    }
    st.state = static_cast<u8>(M::Combat);
    // An aircraft on its tail, going its way: the chase.
    if (in.target_in_air && in.target_moved &&
        osc::dmath::sin(in.target_heading) * hx + osc::dmath::cos(in.target_heading) * hz > 0.0f)
        st.state = static_cast<u8>(M::NormalTurn);
}

std::optional<Vector3> calc_bomb_drop(const Vector3& v, const Vector3& p, const Vector3& t,
                                      f32 gravity) {
    const f32 g = std::abs(gravity);
    if (g <= 0.0f) return std::nullopt;
    const f32 h = p.y - t.y;      // its height over the target, up
    const f32 vy = std::abs(v.y); // Moho takes the vertical speed's size
    const f32 disc = (h >= 0.0f ? 2.0f : -2.0f) * std::abs(h) * g + vy * vy;
    if (disc < 0.0f) return std::nullopt;
    const f32 root = std::sqrt(disc);
    f32 time = (vy - root) / g;
    if (time < 0.0f) time = (vy + root) / g;
    if (time < 0.0f) return std::nullopt;
    // release = target - (g t^2/2 + v t), gravity pointing down
    return Vector3{t.x - v.x * time, t.y - (-g * time * time * 0.5f + v.y * time),
                   t.z - v.z * time};
}

bool bomb_release_ok(const Vector3& position, f32 heading, const Vector3& release,
                     const Vector3& target, f32 threshold) {
    const f32 rx = release.x - position.x;
    const f32 rz = release.z - position.z;
    const f32 dist = std::sqrt(rx * rx + rz * rz);
    if (dist > threshold) return false;        // not at the release point yet
    if (2.0f * dist <= threshold) return true; // right on it
    const f32 fx = osc::dmath::sin(heading);
    const f32 fz = osc::dmath::cos(heading);
    if (rx * fx + rz * fz > 0.0f) return false; // the point is still ahead
    const f32 tx = target.x - position.x;
    const f32 tz = target.z - position.z;
    return tx * fx + tz * fz >= 0.866f; // and the target still before the nose
}

Vector3 predict_ahead(const Entity& target, f32 seconds) {
    Vector3 at = target.position();
    if (target.is_unit()) {
        const Vector3& v = static_cast<const Unit&>(target).velocity();
        at.x += v.x * seconds;
        at.z += v.z * seconds;
    }
    return at;
}

namespace {

/// fly_attack_run at `at`: `target`'s position, or a ground attack's point
/// (no target).
void fly_run(Unit& unit, const Entity* target, const Vector3& at, SimState& sim,
             const map::Terrain* terrain, f32 dt) {
    using M = AirCombatMode;
    AirCombatState& st = unit.air_combat();
    const AirCombatRules& r = unit.air_combat_rules();
    const Vector3 pos = unit.position();
    f32 heading = unit.heading();
    if (!st.flying) {
        // The run takes over the airframe as it flies.
        st.flying = true;
        st.yaw_rate = 0.0f;
        st.velocity = {osc::dmath::sin(heading) * unit.current_airspeed(), 0.0f,
                       osc::dmath::cos(heading) * unit.current_airspeed()};
    }

    const Unit* target_unit =
        target && target->is_unit() ? static_cast<const Unit*>(target) : nullptr;
    AirTacticsInput in;
    in.position = pos;
    in.heading = heading;
    in.target = at;
    in.target_in_air = target_unit && target_unit->layer() == "Air";
    if (target_unit) {
        const Vector3& v = target_unit->velocity();
        in.target_moved = v.x != 0.0f || v.y != 0.0f || v.z != 0.0f;
        in.target_heading = target_unit->heading();
    }
    in.in_map = in_map(sim, pos, 0.0f);
    in.in_map_by_5 = in_map(sim, pos, 5.0f);
    in.tick = sim.tick_count();
    in.max_airspeed = unit.max_airspeed() * unit.speed_mult();
    in.break_off_trigger = r.break_off_trigger * unit.break_off_trigger_mult();
    in.break_off_distance = r.break_off_distance * unit.break_off_distance_mult();
    air_tactics(st, r, in, sim.random());
    const auto mode = static_cast<M>(st.state);

    // What the state asks (Moho's switch over the combat states).
    const f32 dx = in.target.x - pos.x;
    const f32 dz = in.target.z - pos.z;
    const f32 dist = std::sqrt(dx * dx + dz * dz + (in.target.y - pos.y) * (in.target.y - pos.y));
    AirSteer steer;
    steer.making_attack_run = mode == M::Combat || mode == M::NormalTurn || mode == M::BreakOff;
    steer.full_thrust = static_cast<u8>(mode) > static_cast<u8>(M::NormalTurn);
    steer.speed = in.max_airspeed;
    steer.turn_clamp = unit.turn_rate_rad() * unit.turn_mult();
    Vector3 aim = in.target;
    switch (mode) {
    case M::Combat:
    case M::NormalTurn:
        if (target_unit && target_unit->is_mobile()) {
            const f32 ahead = r.predict_ahead_for_bomb_drop > 0.0f && !in.target_in_air
                                  ? r.predict_ahead_for_bomb_drop
                                  : 1.0f;
            aim = predict_ahead(*target_unit, ahead);
        }
        if (mode == M::NormalTurn && in.target_in_air && !in.target_moved)
            steer.speed = std::max(r.min_airspeed, dist);
        break;
    case M::CombatTurn:
    case M::CombatTurnB:
        steer.speed = r.min_airspeed;
        if (mode == M::CombatTurn) steer.turn_clamp = r.combat_turn_speed * unit.turn_mult();
        ++st.sustained_turn_ticks;
        break;
    case M::Realign: ++st.sustained_turn_ticks; break;
    case M::BreakOff:
        aim = {pos.x + osc::dmath::sin(heading), pos.y, pos.z + osc::dmath::cos(heading)};
        st.sustained_turn_ticks = 0;
        break;
    case M::ReturnToMap:
        aim = sim.has_playable_rect()
                  ? Vector3{(sim.playable_x0() + sim.playable_x1()) * 0.5f, pos.y,
                            (sim.playable_z0() + sim.playable_z1()) * 0.5f}
                  : pos;
        st.sustained_turn_ticks = 0;
        break;
    case M::None: break;
    }
    steer.heading = (aim.x != pos.x || aim.z != pos.z)
                        ? osc::dmath::atan2(aim.x - pos.x, aim.z - pos.z)
                        : heading;
    const f32 align = osc::dmath::cos(wrap_angle(steer.heading - heading));
    const f32 wing_blend = 1.0f - align;
    steer.turn_gain = r.k_turn;
    if (mode == M::Combat || mode == M::NormalTurn) steer.turn_gain += wing_blend;
    else if (mode == M::CombatTurn) steer.turn_gain += r.tight_turn_multiplier * wing_blend;

    // Its height (CalcDesiredTargetElevation): over the ground ahead of it,
    // the target's in a run; an aircraft's cruise height for an aircraft.
    const Vector3 ground_at = (mode == M::BreakOff || mode == M::ReturnToMap)
                                  ? Vector3{pos.x + st.velocity.x, pos.y, pos.z + st.velocity.z}
                                  : in.target;
    const f32 floor_there = unit.air_floor(terrain, ground_at.x, ground_at.z);
    if (in.target_in_air && target_unit)
        steer.altitude =
            std::max(target_unit->elevation_target(), 0.5f * unit.elevation_target()) + floor_there;
    else
        steer.altitude =
            (mode == M::Combat ? r.attack_elevation : unit.elevation_target()) + floor_there;

    // The airframe follows with Moho's lag: the nose led by at most the turn
    // clamp, a yaw rate under KTurn and KTurnDamping, and the velocity
    // easing toward the nose under KMove (thrust waits for the nose until
    // the turns).
    const f32 err =
        std::clamp(wrap_angle(steer.heading - heading), -steer.turn_clamp, steer.turn_clamp);
    st.yaw_rate += (steer.turn_gain * err - r.k_turn_damping * st.yaw_rate) * dt;
    heading += st.yaw_rate * dt;
    while (heading < 0.0f) heading += kTwoPi;
    while (heading >= kTwoPi) heading -= kTwoPi;
    unit.set_heading(heading);
    const f32 nx = osc::dmath::sin(heading);
    const f32 nz = osc::dmath::cos(heading);
    const f32 thrust =
        steer.full_thrust
            ? 1.0f
            : std::max(osc::dmath::sin(steer.heading) * nx + osc::dmath::cos(steer.heading) * nz,
                       0.5f);
    st.velocity.x += (r.k_move * steer.speed * thrust * nx - r.k_move * st.velocity.x) * dt;
    st.velocity.z += (r.k_move * steer.speed * thrust * nz - r.k_move * st.velocity.z) * dt;
    st.velocity.y = 0.0f;

    Vector3 next = pos;
    next.x += st.velocity.x * dt;
    next.z += st.velocity.z * dt;
    const f32 floor_here = unit.air_floor(terrain, next.x, next.z);
    f32 alt = unit.current_altitude();
    const f32 want = steer.altitude - floor_here;
    const f32 climb = unit.climb_rate() * dt;
    alt = alt < want ? std::min(alt + climb, want) : std::max(alt - climb, want);
    unit.set_current_altitude(alt);
    next.y = floor_here + alt;

    const f32 bank = std::clamp(st.yaw_rate * 0.5f, -0.5f, 0.5f);
    f32 cur_bank = unit.bank_angle();
    cur_bank += (bank - cur_bank) * std::min(1.0f, 5.0f * dt);
    unit.set_bank_angle(cur_bank);
    const f32 pitch = std::clamp((steer.altitude - next.y) * 0.02f, -0.3f, 0.3f);
    unit.set_pitch_angle(pitch);
    unit.set_orientation(euler_to_quat(heading, pitch, cur_bank));
    unit.set_position(sim.clamp_to_playable(next, unit.army()));
    unit.set_current_airspeed(
        std::sqrt(st.velocity.x * st.velocity.x + st.velocity.z * st.velocity.z));
    unit.set_unit_state("MakingAttackRun", steer.making_attack_run);
}

} // namespace

void fly_attack_run(Unit& unit, const Entity& target, SimState& sim, const map::Terrain* terrain,
                    f32 dt) {
    fly_run(unit, &target, target.position(), sim, terrain, dt);
}

void fly_attack_run(Unit& unit, const Vector3& at, SimState& sim, const map::Terrain* terrain,
                    f32 dt) {
    fly_run(unit, nullptr, at, sim, terrain, dt);
}

void end_attack_run(Unit& unit) {
    AirCombatState& st = unit.air_combat();
    if (st.state == 0 && !st.flying && !unit.has_unit_state("MakingAttackRun")) return;
    st = AirCombatState{};
    unit.set_unit_state("MakingAttackRun", false);
}

} // namespace osc::sim
