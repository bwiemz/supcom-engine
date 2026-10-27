// A killed aircraft's fall, as Moho's CUnitMotion drives it (faf-re unit/CUnitMotion.cpp):
// CalcMoveAir (0x006BF150 on) puts a dead aircraft still in flight on the Air layer and into
// UMS_Ballistic, draws its tumble from the sim's stream, and lets its flight body fall one
// step; MotionTick then gives each tick to CalcMoveBallistic (0x006C0290), which falls by
// gravity along the last tick's move, turns by the tumble, and stops where the step meets the
// ground or the water (STIMap::SurfaceIntersection). There the unit takes that layer, its
// script hears OnImpact, and it is Crashed: MotionTick moves it no more.

#include "sim/unit.hpp"

#include "core/dmath.hpp"
#include "core/test_status.hpp"
#include "map/terrain.hpp"
#include "sim/flight_math.hpp"
#include "sim/projectile.hpp"
#include "sim/sim_random.hpp"
#include "sim/sim_state.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>

#include <spdlog/spdlog.h>

extern "C" {
#include <lua.h>
}

namespace osc::sim {

namespace {

constexpr f32 kTwoPi = 6.28318530f;
/// SPhysBody's step in CalcMoveAir and CalcMoveBallistic (kFixedIntegrationDt).
constexpr f32 kBodyStep = 0.1f;
/// CalcMoveBallistic's gravity per tick squared: 0x00E4F950, one ulp above the float nearest
/// 0.01.
constexpr f32 kGravityTickScale = 0x1.47ae16p-7f;
/// Moho's water elevation on a map without water.
constexpr f32 kNoWater = -10000.0f;

const char* motion_state_name(Unit::MotionState state) {
    switch (state) {
    case Unit::MotionState::None: return "None";
    case Unit::MotionState::Ballistic: return "Ballistic";
    case Unit::MotionState::Crashed: return "Crashed";
    }
    return "None";
}

} // namespace

void Unit::set_flight_orientation(const Quaternion& q) {
    set_orientation(q);
    const Vector3 fwd = forward_of(q);
    f32 heading = osc::dmath::atan2(fwd.x, fwd.z);
    if (heading < 0.0f) {
        heading += kTwoPi;
    }
    heading_ = heading;
    const Vector3 right = quat_rotate(q, Vector3{1.0f, 0.0f, 0.0f});
    const Vector3 up = quat_rotate(q, Vector3{0.0f, 1.0f, 0.0f});
    bank_angle_ = osc::dmath::atan2(right.y, up.y);
    pitch_ = osc::dmath::asin(std::clamp(-fwd.y, -1.0f, 1.0f));
}

bool Unit::hovers_instead_of_landing() const {
    return transport_hover_height_ > 0.0f &&
           (has_unit_state("TransportLoading") || !cargo_ids_.empty());
}

void Unit::set_motion_state(MotionState next, lua_State* L) {
    if (next == motion_state_) {
        return;
    }
    const MotionState old = motion_state_;
    motion_state_ = next;
    if (!L || lua_table_ref() < 0) {
        return;
    }
    const int top = lua_gettop(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, lua_table_ref());
    const int self = lua_gettop(L);
    lua_pushstring(L, "OnMotionStateChange");
    lua_gettable(L, self);
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, self);
        lua_pushstring(L, motion_state_name(next));
        lua_pushstring(L, motion_state_name(old));
        if (lua_pcall(L, 3, 0, 0) != 0) {
            const char* err = lua_tostring(L, -1);
            const std::string message =
                "OnMotionStateChange error: " + std::string(err ? err : "(unknown)");
            spdlog::warn("{}", message);
            if (test_status::count_lua_failures()) test_status::record_failure(message);
        }
    }
    lua_settop(L, top);
}

void Unit::step_dead_motion(const map::Terrain* terrain, SimRandom& random, lua_State* L,
                            SimState* sim) {
    // MotionTick waits out a unit under construction before any of its motion.
    if (is_being_built_) {
        return;
    }
    switch (motion_state_) {
    case MotionState::Crashed: return;
    case MotionState::Ballistic: fall(terrain, L, sim); return;
    case MotionState::None: break;
    }
    // MotionTick's flier: Immobile or stunned, it stays put; else CalcMoveAir, which lets a
    // dead one fall if it is in the air or would hover rather than land. A landed one stays.
    if (!can_fly() || immobile() || stun_ticks_ > 0) {
        return;
    }
    if (layer_ != "Air" && !hovers_instead_of_landing()) {
        return;
    }
    start_fall(random, L);
}

void Unit::start_fall(SimRandom& random, lua_State* L) {
    if (layer_ != "Air") {
        set_layer_with_callback("Air", L);
    }
    set_motion_state(MotionState::Ballistic, L);
    // Its tumble: a draw per axis, x first, over its inverse inertia, turned to the world.
    const f32 mass = load_metric();
    const Vector3 inertia = box_inertia(size_x_, size_y_, size_z_);
    const Vector3 range = tumble_range(mass, inertia);
    Vector3 drawn;
    drawn.x = random.range(-range.x, range.x);
    drawn.y = random.range(-range.y, range.y);
    drawn.z = random.range(-range.z, range.z);
    tumble_torque_ = sim::tumble_torque(pending_orientation(), mass, inertia, drawn);

    // No control this beat: SPhysBody::IntegrateFreefallStep with no force and no torque, its
    // flight body's velocity (the level axes', and the lift's) gaining gravity's pull.
    const f32 was_lift = lift_velocity_;
    lift_velocity_ = was_lift + -Projectile::GRAVITY * kBodyStep;
    const f32 half = kBodyStep * 0.5f;
    Vector3 to = pending_position();
    to.x += (air_velocity_.x + air_velocity_.x) * half;
    to.y += (lift_velocity_ + was_lift) * half;
    to.z += (air_velocity_.z + air_velocity_.z) * half;
    const SpinStep spin =
        air_torque_step(pending_orientation(), air_spin_, inertia, Vector3{}, kBodyStep);
    air_spin_ = spin.momentum;
    set_flight_orientation(spin.orientation);
    set_position(to);
}

void Unit::fall(const map::Terrain* terrain, lua_State* L, SimState* sim) {
    // In the air its velocity is its last tick's move (Unit::GetVelocity), and gravity adds a
    // tick's pull to it.
    const Vector3& now = position();
    const Vector3& was = last_position();
    const Vector3 velocity{now.x - was.x, now.y - was.y + -Projectile::GRAVITY * kGravityTickScale,
                           now.z - was.z};
    const Vector3 from = pending_position();
    const Vector3 to{from.x + velocity.x, from.y + velocity.y, from.z + velocity.z};

    if (tumble_torque_.x != 0.0f || tumble_torque_.y != 0.0f || tumble_torque_.z != 0.0f) {
        const SpinStep spin =
            air_torque_step(pending_orientation(), air_spin_,
                            box_inertia(size_x_, size_y_, size_z_), tumble_torque_, kBodyStep);
        air_spin_ = spin.momentum;
        set_flight_orientation(spin.orientation);
    }

    // The step as a line from where it is: Moho's Segment3 from its two ends. An amphibious unit
    // meets the ground alone; the rest the water too.
    const Vector3 d{to.x - from.x, to.y - from.y, to.z - from.z};
    const f32 length = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    const bool amphibious = motion_type_ == "RULEUMT_Amphibious";
    std::optional<f32> hit;
    if (terrain && length > 0.0f) {
        const Vector3 dir{d.x / length, d.y / length, d.z / length};
        hit = amphibious
                  ? terrain->heightmap().intersect(from.x, from.y, from.z, dir.x, dir.y, dir.z,
                                                   0.0f, length)
                  : terrain->surface_intersect(from.x, from.y, from.z, dir.x, dir.y, dir.z, length);
    }
    const f32 water = terrain && terrain->has_water() ? terrain->water_elevation() : kNoWater;
    const std::string old_layer = layer_;
    std::string new_layer = layer_;
    Vector3 at = to;
    if (!hit) {
        // No hit: the layer by the heights under where it was.
        const f32 ground = terrain ? terrain->get_terrain_height(from.x, from.z) : kNoWater;
        if (amphibious) {
            if (ground >= from.y) new_layer = water > ground ? "Seabed" : "Land";
        } else if (water >= from.y) {
            new_layer = "Water";
        } else if (ground >= from.y) {
            new_layer = "Land";
        }
    } else {
        const f32 t = *hit;
        at = {from.x + d.x / length * t, from.y + d.y / length * t, from.z + d.z / length * t};
        if (water >= at.y) {
            new_layer = amphibious ? "Seabed" : "Water";
        } else {
            new_layer = "Land";
        }
    }
    if (new_layer != old_layer) {
        set_layer_with_callback(new_layer, L);
        // Only a dead unit falls here: the surface it met is its script's to play out, before it
        // moves there.
        if (sim) {
            sim->crash_impact(*this, new_layer == "Land" ? "Terrain" : "Water");
        }
        set_motion_state(MotionState::Crashed, L);
    }
    set_position(at);
}

} // namespace osc::sim
