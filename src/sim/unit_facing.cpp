// A stopped surface unit's hull turned to its weapons' work, as Moho's
// CUnitMotion::CalcMoveCommon turns it (faf-re unit/CUnitMotion.cpp:
// 2746-2838): toward the target of its first SlavedToBody weapon, with
// hysteresis, or AttackAngle off it; with none, the way its parked attack
// order asked (the attack task's SetFacing).

#include "sim/unit.hpp"

#include "core/dmath.hpp"
#include "sim/entity_registry.hpp"
#include "sim/weapon.hpp"

#include <cmath>
#include <optional>

namespace osc::sim {

namespace {

constexpr f32 kPi = 3.14159265358979f;
constexpr f32 kDegToRad = kPi / 180.0f;

f32 wrap(f32 a) {
    while (a > kPi) a -= 2.0f * kPi;
    while (a <= -kPi) a += 2.0f * kPi;
    return a;
}

} // namespace

f32 attack_angle_heading(f32 bearing, f32 heading, f32 attack_angle) {
    // ApplyCommonMoveAttackAngle: the target AttackAngle off the bow, on the
    // side it already lies (dead ahead, the right).
    const f32 err = wrap(bearing - heading);
    return wrap(bearing + (err >= 0.0f ? -attack_angle : attack_angle));
}

bool Unit::rotate_yaw_toward(f32 want, f32 max_step) {
    const f32 yaw = quat_yaw(orientation());
    const f32 err = wrap(want - yaw);
    // Already facing it: within about 0.81 degrees (a dot of 0.9999).
    if (osc::dmath::cos(err) >= 0.9999f || max_step <= 0.0f) return false;
    // At most a tick's turn, onto the heading exactly once within it.
    const f32 next = std::fabs(err) <= max_step ? want : yaw + (err > 0.0f ? max_step : -max_step);
    set_orientation(euler_to_quat(wrap(next), 0.0f, 0.0f));
    return true;
}

void Unit::face_weapons_work(f64 dt, const EntityRegistry& registry) {
    turned_in_place_ = false;
    // Aircraft, flying or landed, move by CalcMoveAir alone; a unit being
    // built, dying, carried or attached, or rising to a transport, is turned
    // by none of this.
    if (can_fly() || is_being_built() || dying_ || parent_entity_id() != 0 || beam_up_ticks_ > 0 ||
        has_unit_state("Attached"))
        return;
    // Driven along a path, it faces the path; a new goal overwrites a parked
    // attack's facing (Moho's SetTarget, mFormationVec). Held still or
    // stunned, it turns in place all the same.
    if (drove_ && !immobile_ && !is_stunned()) {
        attack_facing_ = {};
        return;
    }

    // The target of the first slaved weapon that has one (HasSlavedTarget).
    const Weapon* slaved = nullptr;
    std::optional<Vector3> at;
    for (const auto& w : weapons_) {
        if (!w->slaved_to_body || !w->has_target()) continue;
        at = w->target_point(registry);
        if (!at) continue;
        slaved = w.get();
        break;
    }

    std::optional<f32> want;
    if (!slaved) {
        slaved_turning_ = false;
    } else {
        const f32 bearing = osc::dmath::atan2(at->x - position().x, at->z - position().z);
        const f32 yaw = quat_yaw(orientation());
        if (attack_angle_ > 0.0f) {
            want = attack_angle_heading(bearing, yaw, attack_angle_ * kDegToRad);
        } else if (slaved->slaved_arc_range < 180.0f) {
            // Turning once it is SlavedToBodyArcRange off, until under half
            // that; not turning, it faces its own way (and nothing else).
            const f32 off = std::fabs(wrap(bearing - yaw));
            const f32 arc = slaved->slaved_arc_range * kDegToRad;
            if (slaved_turning_) {
                if (off < arc * 0.5f) slaved_turning_ = false;
            } else if (off > arc) {
                slaved_turning_ = true;
            }
            want = slaved_turning_ ? bearing : yaw;
        }
    }
    // With nothing slaved to face, an idle unit faces the way its parked
    // attack asked.
    if (!want && !navigator_.busy() && (attack_facing_.x != 0.0f || attack_facing_.z != 0.0f))
        want = osc::dmath::atan2(attack_facing_.x, attack_facing_.z);
    if (!want) return;
    // A hover turns at TurnFacingRate (none without one), the rest at TurnRate.
    const f32 rate = (is_hover() ? drive_.turn_facing_rate : drive_.turn_rate) * turn_mult_;
    turned_in_place_ = rotate_yaw_toward(*want, rate * static_cast<f32>(dt));
}

} // namespace osc::sim
