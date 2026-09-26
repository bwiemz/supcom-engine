#include "sim/manipulator.hpp"
#include "sim/anim_cache.hpp"
#include "sim/bone_data.hpp"
#include "sim/sca_parser.hpp"
#include "sim/unit.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace osc::sim {

// ---------------------------------------------------------------------------
// RotateManipulator
// ---------------------------------------------------------------------------

void RotateManipulator::set_goal(f32 angle) {
    goal_angle_ = angle;
    has_goal_ = true;
}

void RotateManipulator::clear_goal() {
    has_goal_ = false;
}

void RotateManipulator::tick(f32 dt) {
    if (has_goal_) {
        // Goal mode: move current_angle toward goal_angle at speed_ deg/s
        // using shortest-arc rotation
        if (speed_ <= 0) return;
        f32 diff = goal_angle_ - current_angle_;
        // Normalize to [-180, 180] for shortest arc
        while (diff > 180.0f)  diff -= 360.0f;
        while (diff < -180.0f) diff += 360.0f;
        if (std::abs(diff) < 0.01f) {
            current_angle_ = goal_angle_;
            return;
        }
        f32 step = speed_ * dt;
        if (std::abs(diff) <= step) {
            current_angle_ = goal_angle_;
        } else {
            current_angle_ += (diff > 0 ? step : -step);
        }
    } else {
        // Continuous mode: accelerate current_speed toward target_speed
        if (accel_ > 0 && current_speed_ != target_speed_) {
            f32 diff = target_speed_ - current_speed_;
            f32 step = accel_ * dt;
            if (std::abs(diff) <= step) {
                current_speed_ = target_speed_;
            } else {
                current_speed_ += (diff > 0 ? step : -step);
            }
        } else if (accel_ <= 0) {
            // No acceleration — snap to target speed
            current_speed_ = target_speed_;
        }
        // Advance angle by current speed
        current_angle_ += current_speed_ * dt;
        // Keep angle in reasonable range to avoid float overflow
        if (current_angle_ > 36000.0f || current_angle_ < -36000.0f) {
            current_angle_ = std::fmod(current_angle_, 360.0f);
        }
    }
}

bool RotateManipulator::is_at_goal() const {
    if (has_goal_) {
        return std::abs(current_angle_ - goal_angle_) < 0.01f;
    }
    // Continuous mode: at goal when target_speed is 0 and we've stopped
    if (spin_down_ && target_speed_ == 0 && std::abs(current_speed_) < 0.01f) {
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// AnimManipulator — skeletal animation with SCA bone matrix computation
// ---------------------------------------------------------------------------


void AnimManipulator::play_anim(const std::string& anim, bool loop,
                                 AnimCache* cache) {
    // Cross-fade from the bones as the current animation left them (only
    // if one is playing with bone data).
    if (owner_ && !current_anim_.empty() && sca_data_ && blend_time_ > 0.0f) {
        blend_from_ = last_;
        blend_from_set_ = last_set_;
        blend_remaining_ = blend_time_;
    } else {
        blend_from_.clear();
        blend_from_set_.clear();
        blend_remaining_ = 0.0f;
    }

    current_anim_ = anim;
    looping_ = loop;
    fraction_ = 0.0f;
    finished_ = false;
    sca_data_ = nullptr;
    sca_to_scm_map_.clear();
    scm_to_sca_map_.clear();

    // Try to load SCA data
    if (cache && !anim.empty()) {
        sca_data_ = cache->get(anim);
        if (sca_data_) {
            duration_ = sca_data_->duration;
            if (duration_ <= 0) duration_ = 1.0f;

            // Build SCA bone → SCM bone index mapping
            auto* bd = owner_ ? owner_->bone_data() : nullptr;
            if (bd) {
                sca_to_scm_map_.resize(sca_data_->num_bones, -1);
                scm_to_sca_map_.assign(static_cast<size_t>(bd->bone_count()), -1);
                for (u32 i = 0; i < sca_data_->num_bones; i++) {
                    const i32 scm = bd->find_bone(sca_data_->bone_names[i]);
                    sca_to_scm_map_[i] = scm;
                    if (scm >= 0) scm_to_sca_map_[static_cast<size_t>(scm)] = static_cast<i32>(i);
                }
            }
        }
    }
}

void AnimManipulator::set_animation_time(f32 time) {
    if (duration_ > 0) {
        fraction_ = time / duration_;
        if (fraction_ < 0) fraction_ = 0;
        if (fraction_ > 1.0f && !looping_) fraction_ = 1.0f;
    }
}

void AnimManipulator::tick(f32 dt) {
    if (current_anim_.empty()) return;

    // Only the playhead stops when the animation is finished or paused: the
    // pose is written every tick regardless, because Unit::tick_manipulators
    // resets all bones to identity first. Scripts rely on this to hold a
    // pose (PlayAnim + SetRate(0) + SetAnimationFraction, or a one-shot
    // animation left at its last frame).
    if (!finished_ && rate_ != 0) {
        fraction_ += (rate_ * dt) / duration_;

        if (rate_ > 0) {
            if (fraction_ >= 1.0f) {
                if (looping_) {
                    fraction_ = std::fmod(fraction_, 1.0f);
                } else {
                    fraction_ = 1.0f;
                    finished_ = true;
                }
            }
        } else {
            // Negative rate = reverse playback
            if (fraction_ <= 0.0f) {
                if (looping_) {
                    fraction_ = std::fmod(fraction_, 1.0f);
                    if (fraction_ < 0.0f) fraction_ += 1.0f;
                } else {
                    fraction_ = 0.0f;
                    finished_ = true;
                }
            }
        }
    }

    // Advance the cross-fade before the pose is taken (Unit::update_pose),
    // so the first tick after play_anim() is not frozen on the old pose.
    if (blend_remaining_ > 0.0f) {
        blend_remaining_ -= dt;
        if (blend_remaining_ < 0.0f) blend_remaining_ = 0.0f;
    }
}

bool AnimManipulator::is_at_goal() const {
    // Done, for WaitFor, as Moho's CAnimationManipulator signals it (faf-re
    // UpdateTriggeredState): no animation (none played, or it failed to
    // load), a rate of 0, or a one-shot at its end (its start, played
    // backwards). A looping one at rate is never done.
    return !sca_data_ || rate_ == 0.0f || finished_;
}

void AnimManipulator::set_bone_enabled(i32 scm_idx, bool enabled) {
    if (enabled)
        disabled_bones_.erase(scm_idx);
    else
        disabled_bones_.insert(scm_idx);
}

bool AnimManipulator::is_bone_enabled(i32 scm_idx) const {
    return disabled_bones_.find(scm_idx) == disabled_bones_.end();
}

void AnimManipulator::apply_pose(PoseLocals& pose) {
    const f32 from_weight =
        blend_remaining_ > 0.0f && blend_time_ > 0.0f ? blend_remaining_ / blend_time_ : 0.0f;
    if (!sca_data_ || !owner_ || sca_data_->frames.empty() || !owner_->bone_data() ||
        scm_to_sca_map_.size() != static_cast<size_t>(owner_->bone_data()->bone_count())) {
        // No animation to show (it failed to load): fade the bones the last
        // one held back to what lies beneath, rather than snap.
        for (size_t s = 0; s < blend_from_set_.size() && from_weight > 0.0f; ++s) {
            if (!blend_from_set_[s] || !pose.valid(static_cast<i32>(s))) continue;
            const BonePose& below = pose.local[s];
            pose.set(static_cast<i32>(s),
                     {vec3_lerp(below.position, blend_from_[s].position, from_weight),
                      quat_nlerp(below.rotation, blend_from_[s].rotation, from_weight)});
        }
        return;
    }
    const BoneData* bd = owner_->bone_data();

    const u32 num_sca_bones = sca_data_->num_bones;
    const u32 num_frames = sca_data_->num_frames;
    const f32 frac = std::clamp(fraction_, 0.0f, 1.0f);
    const f32 frame_float = frac * static_cast<f32>(num_frames - 1);
    const u32 frame_a = static_cast<u32>(frame_float);
    const u32 frame_b = std::min(frame_a + 1, num_frames - 1);
    const f32 t = frame_float - static_cast<f32>(frame_a);
    const auto& fa = sca_data_->frames[frame_a];
    const auto& fb = sca_data_->frames[frame_b];

    // The frame's bones in model space (SCA parents come first).
    std::vector<BonePose>& world = frame_world_;
    world.resize(num_sca_bones);
    for (u32 i = 0; i < num_sca_bones; i++) {
        const BonePose local{vec3_lerp(fa.bones[i].position, fb.bones[i].position, t),
                             quat_nlerp(fa.bones[i].rotation, fb.bones[i].rotation, t)};
        const i32 parent = sca_data_->parent_indices[i];
        world[i] = parent < 0 || parent >= static_cast<i32>(num_sca_bones)
                       ? local
                       : pose_compose(world[static_cast<size_t>(parent)], local);
    }

    // Each animated bone relative to its SCM parent: that parent as this
    // frame has it, or in bind pose when the animation doesn't move it.
    // Other manipulators then turn the bones on top, and bones the
    // animation leaves alone follow their animated parents.
    const size_t bone_count = static_cast<size_t>(bd->bone_count());
    last_.resize(bone_count);
    last_set_.assign(bone_count, 0);
    for (u32 i = 0; i < num_sca_bones; i++) {
        const i32 scm = i < sca_to_scm_map_.size() ? sca_to_scm_map_[i] : -1;
        if (scm < 0 || static_cast<size_t>(scm) >= bone_count || !is_bone_enabled(scm)) continue;
        const i32 parent = bd->bones[static_cast<size_t>(scm)].parent_index;
        BonePose parent_world;
        if (parent >= 0 && static_cast<size_t>(parent) < bone_count) {
            const i32 animated = scm_to_sca_map_[static_cast<size_t>(parent)];
            parent_world = animated >= 0
                               ? world[static_cast<size_t>(animated)]
                               : BonePose{bd->bones[static_cast<size_t>(parent)].world_position,
                                          bd->bones[static_cast<size_t>(parent)].world_rotation};
        }
        BonePose local = pose_relative(parent_world, world[i]);
        const auto s_idx = static_cast<size_t>(scm);
        if (from_weight > 0.0f && s_idx < blend_from_set_.size() && blend_from_set_[s_idx]) {
            local = {vec3_lerp(local.position, blend_from_[s_idx].position, from_weight),
                     quat_nlerp(local.rotation, blend_from_[s_idx].rotation, from_weight)};
        }
        pose.set(scm, local);
        last_[s_idx] = local;
        last_set_[s_idx] = 1;
    }
}

// ---------------------------------------------------------------------------
// SlideManipulator
// ---------------------------------------------------------------------------

void SlideManipulator::set_goal(f32 x, f32 y, f32 z) {
    goal_ = {x, y, z};
}

void SlideManipulator::tick(f32 dt) {
    if (speed_ <= 0) return;

    f32 dx = goal_.x - current_.x;
    f32 dy = goal_.y - current_.y;
    f32 dz = goal_.z - current_.z;
    f32 dist = std::sqrt(dx * dx + dy * dy + dz * dz);

    if (dist < 0.001f) {
        current_ = goal_;
        return;
    }

    f32 step = speed_ * dt;
    if (step >= dist) {
        current_ = goal_;
    } else {
        f32 ratio = step / dist;
        current_.x += dx * ratio;
        current_.y += dy * ratio;
        current_.z += dz * ratio;
    }
}

bool SlideManipulator::is_at_goal() const {
    f32 dx = goal_.x - current_.x;
    f32 dy = goal_.y - current_.y;
    f32 dz = goal_.z - current_.z;
    return (dx * dx + dy * dy + dz * dz) < 0.001f * 0.001f;
}

// ---------------------------------------------------------------------------
// Pose contributions
// ---------------------------------------------------------------------------

namespace {

constexpr f32 kPi = 3.14159265358979f;
constexpr f32 kDegToRad = kPi / 180.0f;

/// An angle difference wrapped to [-pi, pi].
f32 wrap_angle(f32 a) {
    while (a > kPi) a -= 2.0f * kPi;
    while (a < -kPi) a += 2.0f * kPi;
    return a;
}

Vector3 sub(const Vector3& a, const Vector3& b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

/// Move `value` toward `goal` by at most `step`.
f32 approach(f32 value, f32 goal, f32 step) {
    if (goal > value) return std::min(goal, value + step);
    return std::max(goal, value - step);
}

/// A bone's rest frame in world space: its parent as currently posed,
/// composed with the bone's own bind transform (so without its own delta).
BonePose rest_frame_world(const Unit& unit, i32 bone) {
    const BoneInfo& info = unit.bone_data()->bones[static_cast<size_t>(bone)];
    BonePose parent;
    if (info.parent_index >= 0) parent = unit.bone_pose(info.parent_index);
    const Quaternion model_rot = quat_multiply(parent.rotation, info.local_rotation);
    const Vector3 offset = quat_rotate(parent.rotation, info.local_position);
    const f32 s = unit.bone_data()->model_scale;
    const Vector3 model_pos{(parent.position.x + offset.x) * s, (parent.position.y + offset.y) * s,
                            (parent.position.z + offset.z) * s};
    const Vector3 world = quat_rotate(unit.orientation(), model_pos);
    return {{unit.position().x + world.x, unit.position().y + world.y, unit.position().z + world.z},
            quat_multiply(unit.orientation(), model_rot)};
}

} // namespace

void RotateManipulator::apply_pose(PoseLocals& pose) {
    pose.rotate(bone_index_, quat_axis_angle(axis_, current_angle_ * kDegToRad));
}

void SlideManipulator::apply_pose(PoseLocals& pose) {
    pose.slide(bone_index_, current_);
}

// ---------------------------------------------------------------------------
// AimManipulator
// ---------------------------------------------------------------------------

void AimManipulator::set_firing_arc(f32 yaw_min, f32 yaw_max, f32 yaw_speed, f32 pitch_min,
                                    f32 pitch_max, f32 pitch_speed) {
    yaw_min_ = yaw_min * kDegToRad;
    yaw_max_ = yaw_max * kDegToRad;
    yaw_speed_ = yaw_speed * kDegToRad;
    pitch_min_ = pitch_min * kDegToRad;
    pitch_max_ = pitch_max * kDegToRad;
    pitch_speed_ = pitch_speed * kDegToRad;
}

void AimManipulator::set_heading_pitch(f32 h, f32 p) {
    const bool full_circle = yaw_max_ - yaw_min_ >= 2.0f * kPi - 1e-4f;
    heading_ = full_circle ? wrap_angle(h) : std::clamp(h, yaw_min_, yaw_max_);
    pitch_ = std::clamp(p, pitch_min_, pitch_max_);
}

void AimManipulator::tick(f32 dt) {
    const Unit* unit = owner_;
    const bool bones = unit && unit->bone_data() && unit->bone_data()->is_valid(yaw_bone_);
    const bool pitches = bones && unit->bone_data()->is_valid(pitch_bone_);
    const bool full_circle = yaw_max_ - yaw_min_ >= 2.0f * kPi - 1e-4f;

    // No bones to turn: nothing to wait for.
    if (!bones) {
        on_target_ = has_target_;
        return;
    }

    f32 want_heading = 0;
    f32 want_pitch = 0;
    bool reachable = true;
    if (has_target_) {
        idle_time_ = 0;
        // The target in the yaw bone's rest frame: heading about its Y axis
        // from its forward (+Z); pitch from the pitch bone, once turned.
        const BonePose yaw = rest_frame_world(*unit, yaw_bone_);
        const Quaternion to_local = quat_conjugate(yaw.rotation);
        const Vector3 v = quat_rotate(to_local, sub(target_, yaw.position));
        want_heading = osc::dmath::atan2(v.x, v.z);
        const Vector3 from = pitches ? unit->bone_world_position(pitch_bone_) : yaw.position;
        const Vector3 w = quat_rotate(quat_axis_angle('y', -want_heading),
                                      quat_rotate(to_local, sub(target_, from)));
        want_pitch =
            elevation_ ? *elevation_ : osc::dmath::atan2(w.y, std::sqrt(w.x * w.x + w.z * w.z));
        if (!full_circle) {
            // Measured about the arc's centre, so an arc across +-180 deg
            // (a rear turret) still contains the headings it should.
            const f32 centre = 0.5f * (yaw_min_ + yaw_max_);
            want_heading = centre + wrap_angle(want_heading - centre);
            reachable = want_heading >= yaw_min_ && want_heading <= yaw_max_;
            want_heading = std::clamp(want_heading, yaw_min_, yaw_max_);
        }
        if (pitches) {
            reachable = reachable && want_pitch >= pitch_min_ && want_pitch <= pitch_max_;
            want_pitch = std::clamp(want_pitch, pitch_min_, pitch_max_);
        }
    } else {
        // Hold the pose for the reset time, then return to rest.
        on_target_ = false;
        idle_time_ += dt;
        if (idle_time_ < reset_pose_time_) return;
        want_heading = full_circle ? 0.0f : std::clamp(0.0f, yaw_min_, yaw_max_);
        want_pitch = std::clamp(0.0f, pitch_min_, pitch_max_);
    }

    if (full_circle) {
        const f32 diff = wrap_angle(want_heading - heading_);
        heading_ = wrap_angle(heading_ + approach(0.0f, diff, yaw_speed_ * dt));
    } else {
        heading_ = approach(heading_, want_heading, yaw_speed_ * dt);
    }
    if (pitches) pitch_ = approach(pitch_, want_pitch, pitch_speed_ * dt);

    if (!has_target_) return;
    const f32 heading_error =
        std::fabs(full_circle ? wrap_angle(want_heading - heading_) : want_heading - heading_);
    const f32 pitch_error = pitches ? std::fabs(want_pitch - pitch_) : 0.0f;
    on_target_ = reachable && heading_error <= tolerance_ && pitch_error <= tolerance_;
}

void AimManipulator::apply_pose(PoseLocals& pose) {
    if (yaw_bone_ < 0) return;
    pose.rotate(yaw_bone_, quat_axis_angle('y', heading_));
    // Pitch turns the barrel's forward (+Z) up: about local X, negated. On
    // a single-bone turret it follows the yaw on the same bone.
    if (pitch_bone_ >= 0) pose.rotate(pitch_bone_, quat_axis_angle('x', -pitch_));
}

} // namespace osc::sim
