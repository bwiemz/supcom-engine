#include "sim/manipulator.hpp"

#include <algorithm>
#include <cmath>

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
// AnimManipulator
// ---------------------------------------------------------------------------

void AnimManipulator::play_anim(const std::string& anim, bool loop) {
    current_anim_ = anim;
    looping_ = loop;
    fraction_ = 0.0f;
    finished_ = false;
    // Default rate stays at whatever was set (or 0 if not yet set)
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
}

bool AnimManipulator::is_at_goal() const {
    return finished_;
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
