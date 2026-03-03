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

} // anonymous namespace

void AnimManipulator::play_anim(const std::string& anim, bool loop,
                                 AnimCache* cache) {
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
    if (finished_ || current_anim_.empty()) return;
    if (rate_ == 0) return;

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

    // Compute bone matrices after fraction update
    compute_bone_matrices();
}

bool AnimManipulator::is_at_goal() const {
    return finished_;
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
            scm_idx < bd->bone_count()) {
            f32 world_mat[16];
            quat_pos_to_mat4(world_mat, world_xforms[i].rot,
                             world_xforms[i].pos);
            mat4_multiply(matrices[scm_idx].data(), world_mat,
                          bd->bones[scm_idx].inverse_bind_pose.data());
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
// AimManipulator
// ---------------------------------------------------------------------------

void AimManipulator::set_firing_arc(f32 yaw_min, f32 yaw_max, f32 yaw_speed,
                                     f32 pitch_min, f32 pitch_max, f32 pitch_speed) {
    yaw_min_ = yaw_min;
    yaw_max_ = yaw_max;
    yaw_speed_ = yaw_speed;
    pitch_min_ = pitch_min;
    pitch_max_ = pitch_max;
    pitch_speed_ = pitch_speed;
}

void AimManipulator::set_heading_pitch(f32 h, f32 p) {
    heading_ = h;
    pitch_ = p;
    // Clamp to firing arc
    heading_ = std::clamp(heading_, yaw_min_, yaw_max_);
    pitch_ = std::clamp(pitch_, pitch_min_, pitch_max_);
    on_target_ = true;
}

void AimManipulator::tick(f32 /*dt*/) {
    // AimManipulator state is set externally via set_heading_pitch.
    // Future: could interpolate toward target heading/pitch at yaw_speed/pitch_speed.
}

} // namespace osc::sim
