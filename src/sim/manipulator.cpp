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

namespace {

/// Normalized linear interpolation for quaternions (cheaper than slerp,
/// sufficient at 30fps SCA frame rates).
Quaternion nlerp(const Quaternion& a, const Quaternion& b, f32 t) {
    // Ensure shortest path (dot product check)
    f32 dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    f32 sign = dot < 0 ? -1.0f : 1.0f;
    Quaternion r = {
        a.x + t * (sign * b.x - a.x),
        a.y + t * (sign * b.y - a.y),
        a.z + t * (sign * b.z - a.z),
        a.w + t * (sign * b.w - a.w)
    };
    // Normalize
    f32 len = std::sqrt(r.x * r.x + r.y * r.y + r.z * r.z + r.w * r.w);
    if (len > 1e-8f) {
        f32 inv = 1.0f / len;
        r.x *= inv; r.y *= inv; r.z *= inv; r.w *= inv;
    } else {
        r = a; // Degenerate case (near-antipodal): return start quaternion
    }
    return r;
}

/// Linear interpolation for Vector3.
Vector3 lerp_vec3(const Vector3& a, const Vector3& b, f32 t) {
    return {a.x + t * (b.x - a.x),
            a.y + t * (b.y - a.y),
            a.z + t * (b.z - a.z)};
}

/// Build column-major 4x4 matrix from quaternion + position (scale=1).
void quat_pos_to_mat4(f32* out, const Quaternion& q, const Vector3& p) {
    f32 xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    f32 xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    f32 wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    // Column 0
    out[0]  = 1.0f - 2.0f * (yy + zz);
    out[1]  = 2.0f * (xy + wz);
    out[2]  = 2.0f * (xz - wy);
    out[3]  = 0.0f;
    // Column 1
    out[4]  = 2.0f * (xy - wz);
    out[5]  = 1.0f - 2.0f * (xx + zz);
    out[6]  = 2.0f * (yz + wx);
    out[7]  = 0.0f;
    // Column 2
    out[8]  = 2.0f * (xz + wy);
    out[9]  = 2.0f * (yz - wx);
    out[10] = 1.0f - 2.0f * (xx + yy);
    out[11] = 0.0f;
    // Column 3 (translation)
    out[12] = p.x;
    out[13] = p.y;
    out[14] = p.z;
    out[15] = 1.0f;
}

/// 4x4 column-major matrix multiply: C = A * B
void mat4_multiply(f32* C, const f32* A, const f32* B) {
    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 4; row++) {
            C[col * 4 + row] =
                A[0 * 4 + row] * B[col * 4 + 0] +
                A[1 * 4 + row] * B[col * 4 + 1] +
                A[2 * 4 + row] * B[col * 4 + 2] +
                A[3 * 4 + row] * B[col * 4 + 3];
        }
    }
}

/// Component-wise linear interpolation of 4x4 matrices.
/// Sufficient for short blend durations where rotation error is imperceptible.
void mat4_lerp(f32* out, const f32* a, const f32* b, f32 t) {
    for (int i = 0; i < 16; i++) {
        out[i] = a[i] + t * (b[i] - a[i]);
    }
}

} // anonymous namespace

void AnimManipulator::play_anim(const std::string& anim, bool loop,
                                 AnimCache* cache) {
    // Snapshot current bone matrices for cross-fade blending
    // (only if we're already playing an animation with bone data)
    if (owner_ && !current_anim_.empty() && sca_data_ && blend_time_ > 0.0f) {
        blend_from_matrices_ = owner_->animated_bone_matrices();
        blend_remaining_ = blend_time_;
    } else {
        blend_from_matrices_.clear();
        blend_remaining_ = 0.0f;
    }

    current_anim_ = anim;
    looping_ = loop;
    fraction_ = 0.0f;
    finished_ = false;
    sca_data_ = nullptr;
    sca_to_scm_map_.clear();

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
                for (u32 i = 0; i < sca_data_->num_bones; i++) {
                    sca_to_scm_map_[i] =
                        bd->find_bone(sca_data_->bone_names[i]);
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

    // Advance cross-fade blend BEFORE computing bone matrices,
    // so the first tick after play_anim() uses weight < 1.0 (not frozen on old pose).
    if (blend_remaining_ > 0.0f) {
        blend_remaining_ -= dt;
        if (blend_remaining_ < 0.0f) blend_remaining_ = 0.0f;
    }

    // Compute bone matrices after fraction update
    compute_bone_matrices();
}

bool AnimManipulator::is_at_goal() const {
    return finished_;
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

void AnimManipulator::compute_bone_matrices() {
    if (!sca_data_ || !owner_ || sca_data_->frames.empty()) return;
    auto& matrices = owner_->animated_bone_matrices();
    if (matrices.empty()) return;

    auto* bd = owner_->bone_data();
    if (!bd) return;

    u32 num_sca_bones = sca_data_->num_bones;
    u32 num_frames = sca_data_->num_frames;

    // Determine frame pair and lerp factor
    f32 frac = std::clamp(fraction_, 0.0f, 1.0f);
    f32 frame_float = frac * static_cast<f32>(num_frames - 1);
    u32 frame_a = static_cast<u32>(frame_float);
    u32 frame_b = frame_a + 1;
    if (frame_b >= num_frames) frame_b = num_frames - 1;
    f32 t = frame_float - static_cast<f32>(frame_a);

    const auto& fa = sca_data_->frames[frame_a];
    const auto& fb = sca_data_->frames[frame_b];

    // Temporary world transforms for SCA bones (position + rotation)
    struct WorldXform { Vector3 pos; Quaternion rot; };
    std::vector<WorldXform> world_xforms(num_sca_bones);

    // Forward pass: compute world transforms (parents come first in SCA)
    for (u32 i = 0; i < num_sca_bones; i++) {
        // Interpolate between frame pair
        Vector3 local_pos = lerp_vec3(fa.bones[i].position,
                                       fb.bones[i].position, t);
        Quaternion local_rot = nlerp(fa.bones[i].rotation,
                                      fb.bones[i].rotation, t);

        i32 parent = sca_data_->parent_indices[i];
        if (parent < 0 || parent >= static_cast<i32>(num_sca_bones)) {
            // Root bone
            world_xforms[i] = {local_pos, local_rot};
        } else {
            auto& pw = world_xforms[parent];
            // world_rot = parent_rot * local_rot
            world_xforms[i].rot = quat_multiply(pw.rot, local_rot);
            // world_pos = parent_pos + rotate(local_pos, parent_rot)
            auto rotated = quat_rotate(pw.rot, local_pos);
            world_xforms[i].pos = {
                pw.pos.x + rotated.x,
                pw.pos.y + rotated.y,
                pw.pos.z + rotated.z
            };
        }

        // Write final bone matrix: animated_world × inverse_bind_pose
        i32 scm_idx = (i < sca_to_scm_map_.size())
                          ? sca_to_scm_map_[i] : -1;
        if (scm_idx >= 0 &&
            scm_idx < static_cast<i32>(matrices.size()) &&
            scm_idx < bd->bone_count() &&
            disabled_bones_.find(scm_idx) == disabled_bones_.end()) {
            f32 world_mat[16];
            quat_pos_to_mat4(world_mat, world_xforms[i].rot,
                             world_xforms[i].pos);
            mat4_multiply(matrices[scm_idx].data(), world_mat,
                          bd->bones[scm_idx].inverse_bind_pose.data());
        }
    }

    // Apply cross-fade blending if active
    if (blend_remaining_ > 0.0f && blend_time_ > 0.0f && !blend_from_matrices_.empty()) {
        f32 weight = blend_remaining_ / blend_time_; // 1.0 → 0.0 over blend duration
        for (u32 i = 0; i < num_sca_bones; i++) {
            i32 scm_idx = (i < sca_to_scm_map_.size())
                              ? sca_to_scm_map_[i] : -1;
            if (scm_idx >= 0 &&
                scm_idx < static_cast<i32>(matrices.size()) &&
                scm_idx < static_cast<i32>(blend_from_matrices_.size()) &&
                disabled_bones_.find(scm_idx) == disabled_bones_.end()) {
                f32 blended[16];
                mat4_lerp(blended,
                          matrices[scm_idx].data(),        // "to" (new anim)
                          blend_from_matrices_[scm_idx].data(), // "from" (snapshot)
                          weight);
                std::memcpy(matrices[scm_idx].data(), blended, sizeof(f32) * 16);
            }
        }
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
    const Vector3 model_pos{parent.position.x + offset.x, parent.position.y + offset.y,
                            parent.position.z + offset.z};
    const Vector3 world = quat_rotate(unit.orientation(), model_pos);
    return {{unit.position().x + world.x, unit.position().y + world.y, unit.position().z + world.z},
            quat_multiply(unit.orientation(), model_rot)};
}

} // namespace

void RotateManipulator::contribute_pose(PoseDeltas& deltas) const {
    deltas.rotate(bone_index_, quat_axis_angle(axis_, current_angle_ * kDegToRad));
}

void SlideManipulator::contribute_pose(PoseDeltas& deltas) const {
    deltas.slide(bone_index_, current_);
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
        want_pitch = osc::dmath::atan2(w.y, std::sqrt(w.x * w.x + w.z * w.z));
        if (!full_circle) {
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

void AimManipulator::contribute_pose(PoseDeltas& deltas) const {
    if (yaw_bone_ < 0) return;
    const Quaternion yaw = quat_axis_angle('y', heading_);
    // Pitch turns the barrel's forward (+Z) up: about local X, negated.
    const Quaternion pitch = quat_axis_angle('x', -pitch_);
    if (pitch_bone_ < 0) {
        deltas.rotate(yaw_bone_, yaw);
    } else if (pitch_bone_ == yaw_bone_) {
        deltas.rotate(yaw_bone_, quat_multiply(yaw, pitch));
    } else {
        deltas.rotate(yaw_bone_, yaw);
        deltas.rotate(pitch_bone_, pitch);
    }
}

} // namespace osc::sim
