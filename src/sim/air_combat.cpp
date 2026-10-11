#include "sim/air_combat.hpp"

#include "core/dmath.hpp"
#include "map/terrain.hpp"
#include "sim/flight_math.hpp"
#include "sim/sim_random.hpp"
#include "sim/sim_state.hpp"

#include <algorithm>
#include <cmath>

namespace osc::sim {

namespace {

constexpr f32 kTwoPi = 6.28318530f;

/// ComputeAirControl's turn and roll axes toward `wanted`, under KTurn and
/// KTurnDamping, KRoll and KRollDamping.
void steer_attitude(Unit& unit, const Quaternion& wanted, const EntityRegistry* registry,
                    f32 turn_mult, f32 turn_add, f32 dt) {
    const AirCombatRules& r = unit.air_combat_rules();
    const Quaternion body = unit.orientation();
    const f32 load = registry ? unit.transport_load_factor(*registry) : 1.0f;
    const Vector3 err = attitude_error(body, wanted);
    const Vector3 inertia = box_inertia(unit.size_x(), unit.size_y(), unit.size_z());
    const Vector3 w = body_spin(body, unit.air_spin(), inertia);
    const f32 turn = r.k_turn / load * turn_mult + turn_add;
    const Vector3 accel{err.x * turn - w.x * r.k_turn_damping,
                        err.y * turn - w.y * r.k_turn_damping,
                        err.z * r.k_roll / load - w.z * r.k_roll_damping};
    const SpinStep step = air_spin_step(body, unit.air_spin(), inertia, accel, dt);
    unit.set_air_spin(step.momentum);
    unit.set_orientation(step.orientation);
    const Vector3 fwd = forward_of(step.orientation);
    f32 heading = osc::dmath::atan2(fwd.x, fwd.z);
    if (heading < 0.0f) {
        heading += kTwoPi;
    }
    unit.set_heading(heading);
    const Vector3 right = quat_rotate(step.orientation, Vector3{1.0f, 0.0f, 0.0f});
    const Vector3 up = quat_rotate(step.orientation, Vector3{0.0f, 1.0f, 0.0f});
    unit.set_bank_angle(osc::dmath::atan2(right.y, up.y));
    unit.set_pitch_angle(osc::dmath::asin(std::clamp(-fwd.y, -1.0f, 1.0f)));
}

/// ComputeAirControl's step toward `force`, `lift` and `axes`.
void air_control(Unit& unit, const Vector3& force, f32 lift, f32 damping, const AirAxes& axes,
                 bool winged, f32 turn_mult, f32 turn_add, SimState* sim,
                 const map::Terrain* terrain, f32 dt) {
    const AirCombatRules& r = unit.air_combat_rules();
    const AirMoveStep step = air_move_step(unit.air_velocity(), force, r.k_move, damping, dt);
    unit.set_air_velocity(step.velocity);
    Vector3 next = unit.position();
    next.x += step.move.x;
    next.z += step.move.z;
    const f32 floor_here = unit.air_floor(terrain, next.x, next.z);
    const EntityRegistry* registry = sim ? &sim->entity_registry() : nullptr;
    next.y = unit.lift_toward(lift, winged, floor_here, terrain, registry, dt);
    unit.set_current_altitude(next.y - floor_here);
    steer_attitude(unit, attitude_of(axes), registry, turn_mult, turn_add, dt);
    if (sim) {
        next = sim->clamp_to_playable(next, unit.army());
    }
    unit.set_position(next);
    unit.set_current_airspeed(
        std::sqrt(step.velocity.x * step.velocity.x + step.velocity.z * step.velocity.z));
}

Vector3 flat_nose(const Unit& unit) {
    Vector3 nose = forward_of(unit.orientation());
    nose.y = 0.0f;
    const f32 len = std::sqrt(nose.x * nose.x + nose.z * nose.z);
    if (len > 0.0f) {
        return {nose.x / len, 0.0f, nose.z / len};
    }
    return {osc::dmath::sin(unit.heading()), 0.0f, osc::dmath::cos(unit.heading())};
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
    if (!unit.take_air_step()) {
        return;
    }
    using M = AirCombatMode;
    AirCombatState& st = unit.air_combat();
    const AirCombatRules& r = unit.air_combat_rules();
    const Vector3 pos = unit.position();
    const f32 to_x = at.x - pos.x;
    const f32 to_z = at.z - pos.z;
    unit.track_lift_ground(terrain, std::sqrt(to_x * to_x + to_z * to_z), sim.tick_count(), dt);
    st.flying = true;

    const Unit* target_unit =
        target && target->is_unit() ? static_cast<const Unit*>(target) : nullptr;
    AirTacticsInput in;
    in.position = pos;
    in.heading = unit.heading();
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

    const Vector3 nose = flat_nose(unit);
    const f32 top = in.max_airspeed;
    const f32 dist = std::sqrt((in.target.x - pos.x) * (in.target.x - pos.x) +
                               (in.target.y - pos.y) * (in.target.y - pos.y) +
                               (in.target.z - pos.z) * (in.target.z - pos.z));
    Vector3 want{in.target.x - pos.x, 0.0f, in.target.z - pos.z};
    f32 length = top;
    bool set_length = true;
    switch (mode) {
    case M::Combat:
    case M::NormalTurn:
        if (target_unit && target_unit->is_mobile()) {
            const f32 ahead = r.predict_ahead_for_bomb_drop > 0.0f && !in.target_in_air
                                  ? r.predict_ahead_for_bomb_drop
                                  : 1.0f;
            const Vector3 aim = predict_ahead(*target_unit, ahead);
            want = {aim.x - pos.x, 0.0f, aim.z - pos.z};
        }
        // A moving one, as retail trails it; faf-re's text reads the test the other way.
        if (mode == M::NormalTurn && in.target_in_air && in.target_moved) {
            length = std::max(r.min_airspeed, dist);
        }
        break;
    case M::CombatTurn:
    case M::CombatTurnB:
        length = r.min_airspeed;
        ++st.sustained_turn_ticks;
        break;
    case M::Realign: ++st.sustained_turn_ticks; break;
    case M::BreakOff:
        want = {nose.x * top, 0.0f, nose.z * top};
        set_length = false;
        st.sustained_turn_ticks = 0;
        break;
    case M::ReturnToMap:
        if (sim.has_playable_rect()) {
            want = {(sim.playable_x0() + sim.playable_x1()) * 0.5f - pos.x, 0.0f,
                    (sim.playable_z0() + sim.playable_z1()) * 0.5f - pos.z};
        } else {
            want = {};
        }
        st.sustained_turn_ticks = 0;
        break;
    case M::None: length = std::min(std::sqrt(want.x * want.x + want.z * want.z), top); break;
    }

    // CalcDesiredTargetElevation, over the ground where it is headed.
    const f32 floor_there = unit.air_floor(terrain, pos.x + want.x, pos.z + want.z);
    const f32 altitude =
        in.target_in_air && target_unit
            ? std::max(target_unit->elevation_target(), 0.5f * unit.elevation_target()) +
                  floor_there
            : (mode == M::Combat ? r.attack_elevation : unit.elevation_target()) + floor_there;
    want.y = altitude - pos.y;
    if (set_length) {
        const f32 len = std::sqrt(want.x * want.x + want.y * want.y + want.z * want.z);
        if (len > 0.0f) {
            want = {want.x * length / len, want.y * length / len, want.z * length / len};
        }
    }

    const f32 want_len = std::sqrt(want.x * want.x + want.y * want.y + want.z * want.z);
    const Vector3 selected =
        want_len > 0.0f ? Vector3{want.x / want_len, want.y / want_len, want.z / want_len} : nose;
    const f32 limited = std::min(std::sqrt(want.x * want.x + want.z * want.z), top);
    f32 thrust = limited;
    if (static_cast<u8>(mode) <= static_cast<u8>(M::NormalTurn)) {
        const f32 align = selected.x * nose.x + selected.z * nose.z;
        thrust *= align > 0.5f ? align : 0.5f;
    }
    const Vector3 force{nose.x * thrust, 0.0f, nose.z * thrust};
    const f32 elevation = unit.elevation_target();
    const f32 height = pos.y - unit.air_floor(terrain, pos.x, pos.z);
    const f32 lean =
        unit.has_unit_state("MovingDown") && elevation > 0.0f ? height / elevation : 1.0f;
    const f32 turn_speed =
        (mode == M::CombatTurn ? r.combat_turn_speed : unit.turn_rate_rad()) * unit.turn_mult();
    const AirAxes axes = winged_axes(nose, selected, limited, unit.start_turn_distance(),
                                     turn_speed, r.bank_factor, lean, mode == M::NormalTurn);
    f32 turn_add = 0.0f;
    if (mode == M::Combat || mode == M::NormalTurn) {
        turn_add = axes.wing_blend;
    } else if (mode == M::CombatTurn) {
        turn_add = r.tight_turn_multiplier * axes.wing_blend;
    }
    const f32 damping = unit.has_category("TARGETCHASER")
                            ? 1.0f
                            : air_move_damping(want_len, top, r.k_move, r.k_move_damping);
    air_control(unit, force, want.y, damping, axes, true, 1.0f, turn_add, &sim, terrain, dt);
    unit.set_unit_state("MakingAttackRun",
                        mode == M::Combat || mode == M::NormalTurn || mode == M::BreakOff);
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

bool circles(const Unit& unit) {
    // ComputeAirControl tests HoverOverAttack before anything else (FAF's
    // exe, 0x006BE83D: `cmp byte [Air+0x79], 0`), so such an aircraft never
    // circles, target or not. faf-re's text gates only the work half.
    const AirCombatRules& r = unit.air_combat_rules();
    return unit.can_fly() && unit.is_air_unit() && !r.winged && !r.hover_over_attack &&
           !unit.is_dying() && !unit.is_being_built() && !flies_winged_on_guard(unit);
}

bool flies_winged_on_guard(const Unit& unit) {
    const AirCombatRules& r = unit.air_combat_rules();
    if (!unit.can_fly() || !unit.is_air_unit() || r.winged || unit.is_dying() ||
        unit.is_being_built())
        return false;
    // UNITSTATE_Guarding lasts the guard task's life; an attack it breaks
    // off for is its own order here, at the head of the queue.
    const auto& queue = unit.command_queue();
    if (queue.empty() || queue.front().type != CommandType::Guard) return false;
    return !unit.is_building() && !unit.is_repairing() && !unit.has_unit_state("Ferrying") &&
           !unit.has_unit_state("Attacking");
}

void fly_winged_to(Unit& unit, const Vector3& goal, SimState& sim, const map::Terrain* terrain,
                   f32 dt) {
    if (!unit.take_air_step()) return;
    unit.air_combat().flying = true;
    AirMove move;
    move.target = goal;
    move.elevation = unit.elevation_target();
    move.winged = true;
    fly_air_move(unit, move, &sim, terrain, dt);
}

void fly_air_move(Unit& unit, const AirMove& move, SimState* sim, const map::Terrain* terrain,
                  f32 dt) {
    const AirCombatRules& r = unit.air_combat_rules();
    Vector3 pos = unit.position();
    const f32 top = unit.max_airspeed() * unit.speed_mult();
    const f32 dx = move.target.x - pos.x;
    const f32 dz = move.target.z - pos.z;
    const f32 dist = std::sqrt(dx * dx + dz * dz);
    const f32 speed = move.top_speed ? top : std::min(dist, top);
    Vector3 want{};
    if (dist > 0.0f) {
        want = {dx / dist * speed, 0.0f, dz / dist * speed};
    }
    const Vector3 raw = want;
    const u32 tick = sim ? sim->tick_count() : unit.next_lift_tick();
    const f32 slow = unit.track_lift_ground(terrain, dist, tick, dt, move.landing);
    want.x *= slow;
    want.z *= slow;
    const Vector3 v0 = unit.air_velocity();

    const f32 height = pos.y - unit.air_floor(terrain, pos.x, pos.z);
    want.y = unit.lift_ground() + move.elevation - pos.y;
    if (move.landing && want.y < 0.0f) {
        want.y = unit.has_category("TRANSPORTATION") ? std::min(want.y, -3.0f)
                                                     : std::min(want.y * 0.5f, -0.25f);
    }
    if (!(move.landing && move.elevation == 0.0f) && move.elevation > 0.0f &&
        height < move.elevation * 0.5f) {
        const f32 lv = unit.lift_velocity();
        if (top * 0.08f > std::sqrt(v0.x * v0.x + lv * lv + v0.z * v0.z)) {
            const f32 ratio = std::min(height / move.elevation, 1.0f);
            want.x *= ratio;
            want.z *= ratio;
        }
    }

    const Quaternion body = unit.orientation();
    const Vector3 nose = flat_nose(unit);
    const f32 nx = nose.x;
    const f32 nz = nose.z;
    const f32 want_len = std::sqrt(want.x * want.x + want.y * want.y + want.z * want.z);
    const f32 start_turn = unit.start_turn_distance();
    Vector3 facing = raw;
    if (dist <= start_turn) {
        const Vector3& f = unit.air_facing();
        const f32 len = std::sqrt(f.x * f.x + f.y * f.y + f.z * f.z);
        facing = len > 0.0f ? Vector3{f.x / len, f.y / len, f.z / len} : nose;
    }
    Vector3 force{want.x, 0.0f, want.z};
    AirAxes axes;
    const bool winged = r.winged || move.winged;
    if (winged) {
        const f32 limited = std::min(std::sqrt(want.x * want.x + want.z * want.z), top);
        const bool near = start_turn > limited;
        const auto& queue = unit.command_queue();
        const bool guarding = !queue.empty() && queue.front().type == CommandType::Guard;
        Vector3 selected = facing;
        if (!near || guarding) {
            if (want_len > 0.0f) {
                selected = {want.x / want_len, want.y / want_len, want.z / want_len};
            }
            const f32 align = want_len > 0.0f ? (want.x * nx + want.z * nz) / want_len : 0.0f;
            f32 scale = align > 0.5f ? align : 0.5f;
            if (guarding && near) {
                scale = std::min(scale, std::min(limited / start_turn, 0.5f));
            }
            force = {nx * limited * scale, 0.0f, nz * limited * scale};
        }
        const f32 elevation = unit.elevation_target();
        const f32 lean =
            unit.has_unit_state("MovingDown") && elevation > 0.0f ? height / elevation : 1.0f;
        axes = winged_axes(nose, selected, limited, start_turn,
                           unit.turn_rate_rad() * unit.turn_mult(), r.bank_factor, lean);
    } else {
        const f32 elevation = unit.elevation_target();
        axes = hover_axes(body, unit.air_dv(), r.bank_factor, r.bank_forward,
                          elevation > 0.0f ? height / elevation : 1.0f, facing);
    }

    const f32 damping = unit.has_category("TARGETCHASER")
                            ? 1.0f
                            : air_move_damping(want_len, top, r.k_move, r.k_move_damping);
    air_control(unit, force, want.y, damping, axes, winged, 1.0f, 0.0f, sim, terrain, dt);
}

void circling_draws(AirCombatState& st, const AirCombatRules& r, f32 attack_elevation, u32 tick,
                    SimRandom& rng) {
    if (tick <= st.timeout_tick) return;
    // Moho's ScaleRandomUInt32ToRange, (draw x n) >> 32, and a draw scaled
    // into [0, 1) by 2^-32.
    const auto scaled = [&](u32 n) {
        return static_cast<u32>((static_cast<u64>(rng.next_u32()) * n) >> 32);
    };
    const auto fraction = [&] { return static_cast<f64>(rng.next_u32()) * 2.3283064e-10; };
    if (r.circling_dir_change) st.circle_reverse = scaled(100) > 50;
    const f32 most = r.circling_elevation_ratio * attack_elevation;
    st.circle_elevation = static_cast<f32>(-most + (most + most) * fraction());
    st.circle_radius_ratio = static_cast<f32>(
        r.circling_radius_min + (r.circling_radius_max - r.circling_radius_min) * fraction());
    // Floored, as Moho's conversions are (not rounded).
    const auto longest = static_cast<i32>(std::floor(r.circling_change_frequency * 2.0f * 10.0f));
    const auto shortest = static_cast<i32>(std::floor(r.circling_change_frequency * 10.0f));
    st.timeout_tick = tick + static_cast<u32>(shortest) +
                      scaled(static_cast<u32>(std::max(longest - shortest, 0)));
}

CirclingSteer circling_steer(const CirclingInput& in, const map::Terrain* terrain) {
    // The way to the centre, flat; due east when it is overhead.
    f32 tx = in.center.x - in.position.x;
    f32 tz = in.center.z - in.position.z;
    if (tz * tz + tx * tx < 0.000001f) {
        tx = 1.0f;
        tz = 0.0f;
    }
    const f32 tlen = std::sqrt(tx * tx + tz * tz);
    tx /= tlen;
    tz /= tlen;
    // A quarter turn of it, the way round. FAF's exe turns it by Moho's
    // +90 degree yaw (the quaternion at 0x010B6178, cos and sin of +pi/4, made
    // by 0x00BD7390), or the -90 one (0x010B6158) when the draw says so,
    // through its row-major QuatToMatrix: (x, z) goes to (z, -x).
    const f32 gx = in.reverse ? -tz : tz;
    const f32 gz = in.reverse ? tx : -tx;
    // A MinAirspeed's step that way, put out on the circle.
    f32 ox = gx * in.min_airspeed + in.position.x - in.center.x;
    f32 oz = gz * in.min_airspeed + in.position.z - in.center.z;
    const f32 olen = std::sqrt(ox * ox + oz * oz);
    if (olen > 0.0f) {
        ox *= in.radius / olen;
        oz *= in.radius / olen;
    }
    CirclingSteer out;
    out.aim = {in.center.x + ox, 0.0f, in.center.z + oz};
    // Its height over the terrain at the aim's nearest whole point
    // (CHeightField::GetElevation(int, int)): water doesn't count.
    const f32 ground =
        terrain ? terrain->get_terrain_height(std::nearbyint(out.aim.x), std::nearbyint(out.aim.z))
                : 0.0f;
    out.aim.y = in.height + ground;
    Vector3 v{out.aim.x - in.position.x, out.aim.y - in.position.y, out.aim.z - in.position.z};
    const f32 len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    const f32 speed = std::min(len, in.max_airspeed);
    if (len > 0.0f) v = {v.x * speed / len, v.y * speed / len, v.z * speed / len};
    out.velocity = v;
    out.toward = {tx, 0.0f, tz};
    out.facing = osc::dmath::atan2(tx, tz);
    return out;
}

void fly_circling(Unit& unit, const CircleAround& around, SimState& sim,
                  const map::Terrain* terrain, f32 dt) {
    if (!unit.take_air_step()) {
        return;
    }
    AirCombatState& st = unit.air_combat();
    const AirCombatRules& r = unit.air_combat_rules();
    const Vector3 pos = unit.position();
    if (!st.flying) {
        // The circling takes over from where it is (Moho's motion target,
        // once stopped).
        st.flying = true;
        st.circle_anchor = pos;
    }
    circling_draws(st, r, r.attack_elevation, sim.tick_count(), sim.random());

    // Its circle: a target weapon's MaxRadius, wider or narrower against
    // an aircraft; else StartTurnDistance. The ratio scales either.
    f32 radius = unit.start_turn_distance() * st.circle_radius_ratio;
    if (around.weapon_radius > 0.0f) {
        radius = around.weapon_radius * st.circle_radius_ratio;
        if (around.target_in_air) {
            radius *= r.circling_radius_vs_air_mult;
        }
    }
    CirclingInput in;
    in.position = pos;
    in.center = around.center;
    in.radius = radius;
    in.min_airspeed = r.circling_min_airspeed;
    in.max_airspeed = unit.max_airspeed();
    in.height = r.attack_elevation + st.circle_elevation;
    in.reverse = st.circle_reverse;
    const f32 to_x = around.center.x - pos.x;
    const f32 to_z = around.center.z - pos.z;
    unit.track_lift_ground(terrain, std::sqrt(to_x * to_x + to_z * to_z), sim.tick_count(), dt);
    const CirclingSteer steer = circling_steer(in, terrain);

    // CalcAirMovementDampingFactor reads its move vector, to its goal at most
    // its top speed long, and to its flying height.
    const f32 top = unit.max_airspeed() * unit.speed_mult();
    f32 damping = 1.0f;
    if (!unit.has_category("TARGETCHASER")) {
        const f32 mx = around.move_goal.x - pos.x;
        const f32 mz = around.move_goal.z - pos.z;
        const f32 flat = std::min(std::sqrt(mx * mx + mz * mz), top);
        const f32 my = unit.air_floor(terrain, pos.x, pos.z) + unit.elevation_target() - pos.y;
        damping =
            air_move_damping(std::sqrt(flat * flat + my * my), top, r.k_move, r.k_move_damping);
    }
    const f32 elevation = unit.elevation_target();
    const f32 height = pos.y - unit.air_floor(terrain, pos.x, pos.z);
    const AirAxes axes =
        hover_axes(unit.orientation(), unit.air_dv(), r.bank_factor, r.bank_forward,
                   elevation > 0.0f ? height / elevation : 1.0f, steer.toward);
    air_control(unit, steer.velocity, steer.velocity.y, damping, axes, false, r.circling_turn_mult,
                0.0f, &sim, terrain, dt);
}

} // namespace osc::sim
