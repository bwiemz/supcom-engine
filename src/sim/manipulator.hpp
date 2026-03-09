#pragma once

#include "core/types.hpp"
#include "sim/entity.hpp" // Vector3, Quaternion
#include "sim/waitable.hpp"

#include <array>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace osc::sim {

class AnimCache;
class Unit;
struct SCAData;

/// Base class for all manipulators (rotators, animators, sliders, aim controllers).
/// Manipulators are lightweight C++ objects owned by Units, NOT entities.
/// They use the _c_object lightuserdata pattern for Lua binding.
class Manipulator : public Waitable {
public:
    ~Manipulator() override = default;

    Unit* owner() const { return owner_; }
    void set_owner(Unit* u) { owner_ = u; }

    i32 bone_index() const { return bone_index_; }
    void set_bone_index(i32 b) { bone_index_ = b; }

    i32 precedence() const { return precedence_; }
    void set_precedence(i32 p) { precedence_ = p; }

    bool enabled() const { return enabled_; }
    void set_enabled(bool e) { enabled_ = e; }

    bool is_destroyed() const { return destroyed_; }
    void mark_destroyed() { destroyed_ = true; }

    /// Per-tick update. Called from Unit::tick_manipulators().
    virtual void tick(f32 dt) = 0;

    /// Whether the manipulator has reached its goal (for WaitFor).
    virtual bool is_at_goal() const = 0;

    // Waitable interface
    bool is_done() const override { return is_at_goal(); }
    bool is_cancelled() const override { return destroyed_; }

protected:
    Unit* owner_ = nullptr;
    i32 bone_index_ = 0;
    i32 precedence_ = 0;
    bool enabled_ = true;
    bool destroyed_ = false;
};

// ---------------------------------------------------------------------------
// RotateManipulator — rotates a bone around an axis
// ---------------------------------------------------------------------------
class RotateManipulator : public Manipulator {
public:
    void tick(f32 dt) override;
    bool is_at_goal() const override;

    void set_axis(char a) { axis_ = a; }
    char axis() const { return axis_; }

    void set_goal(f32 angle);
    void clear_goal();
    void set_speed(f32 speed) { speed_ = speed; }
    void set_target_speed(f32 speed) { target_speed_ = speed; }
    void set_accel(f32 accel) { accel_ = accel; }
    void set_current_angle(f32 angle) { current_angle_ = angle; }
    f32 current_angle() const { return current_angle_; }
    void set_spin_down(bool sd) { spin_down_ = sd; }
    bool has_goal() const { return has_goal_; }

private:
    char axis_ = 'y';
    f32 current_angle_ = 0;
    f32 goal_angle_ = 0;
    f32 speed_ = 0;           // max degrees/sec (for goal mode)
    f32 target_speed_ = 0;    // continuous rotation target speed
    f32 accel_ = 0;           // degrees/sec^2
    f32 current_speed_ = 0;   // actual current speed (continuous mode)
    bool has_goal_ = false;
    bool spin_down_ = false;
};

// ---------------------------------------------------------------------------
// AnimManipulator — plays skeletal animations with SCA bone matrix computation
// ---------------------------------------------------------------------------
class AnimManipulator : public Manipulator {
public:
    void tick(f32 dt) override;
    bool is_at_goal() const override;

    void play_anim(const std::string& anim, bool loop,
                   AnimCache* cache = nullptr);
    void set_rate(f32 rate) { rate_ = rate; }
    f32 rate() const { return rate_; }
    void set_animation_fraction(f32 frac) { fraction_ = frac; finished_ = false; }
    f32 animation_fraction() const { return fraction_; }
    f32 animation_duration() const { return duration_; }
    void set_animation_time(f32 time);
    f32 animation_time() const { return fraction_ * duration_; }
    const std::string& current_anim() const { return current_anim_; }

    const SCAData* sca_data() const { return sca_data_; }

    /// Per-bone enable/disable (for weapon firing bone animation control).
    void set_bone_enabled(i32 scm_idx, bool enabled);
    bool is_bone_enabled(i32 scm_idx) const;

    /// Set cross-fade blend duration for animation transitions.
    void set_blend_time(f32 seconds) { blend_time_ = seconds; }
    f32 blend_time() const { return blend_time_; }

private:
    void compute_bone_matrices();

    std::string current_anim_;
    f32 rate_ = 0.0f;         // default 0 = paused until SetRate called
    f32 fraction_ = 0.0f;     // 0.0-1.0
    f32 duration_ = 1.0f;     // default (no .sca parsing yet)
    bool looping_ = false;
    bool finished_ = false;

    const SCAData* sca_data_ = nullptr;
    std::vector<i32> sca_to_scm_map_;  // SCA bone → SCM bone index
    std::unordered_set<i32> disabled_bones_;  // SCM bone indices to skip

    // Cross-fade blending state
    f32 blend_time_ = 0.2f;        // default cross-fade duration (seconds)
    f32 blend_remaining_ = 0.0f;   // time left in current cross-fade (0 = no blend)
    std::vector<std::array<f32, 16>> blend_from_matrices_; // snapshot of "from" pose
};

// ---------------------------------------------------------------------------
// SlideManipulator — linear bone translation
// ---------------------------------------------------------------------------
class SlideManipulator : public Manipulator {
public:
    void tick(f32 dt) override;
    bool is_at_goal() const override;

    void set_goal(f32 x, f32 y, f32 z);
    void set_speed(f32 speed) { speed_ = speed; }
    void set_accel(f32 accel) { accel_ = accel; }
    void set_world_units(bool wu) { world_units_ = wu; }

    const Vector3& current() const { return current_; }
    const Vector3& goal() const { return goal_; }

private:
    Vector3 current_ = {0, 0, 0};
    Vector3 goal_ = {0, 0, 0};
    f32 speed_ = 0;
    f32 accel_ = 0;
    bool world_units_ = false;
};

// ---------------------------------------------------------------------------
// AimManipulator — weapon turret aiming
// ---------------------------------------------------------------------------
class AimManipulator : public Manipulator {
public:
    void tick(f32 dt) override;
    bool is_at_goal() const override { return on_target_; }

    void set_firing_arc(f32 yaw_min, f32 yaw_max, f32 yaw_speed,
                        f32 pitch_min, f32 pitch_max, f32 pitch_speed);
    void set_heading_pitch(f32 h, f32 p);
    f32 heading() const { return heading_; }
    f32 pitch() const { return pitch_; }
    bool on_target() const { return on_target_; }
    void set_reset_pose_time(f32 t) { reset_pose_time_ = t; }
    void set_aim_heading_offset(f32 o) { aim_heading_offset_ = o; }

    void set_yaw_bone(i32 b) { yaw_bone_ = b; }
    void set_pitch_bone(i32 b) { pitch_bone_ = b; }
    void set_muzzle_bone(i32 b) { muzzle_bone_ = b; }

private:
    i32 yaw_bone_ = 0;
    i32 pitch_bone_ = 0;
    i32 muzzle_bone_ = 0;
    f32 heading_ = 0;
    f32 pitch_ = 0;
    f32 yaw_min_ = -180, yaw_max_ = 180, yaw_speed_ = 90;
    f32 pitch_min_ = -90, pitch_max_ = 90, pitch_speed_ = 90;
    f32 reset_pose_time_ = 2.0f;
    f32 aim_heading_offset_ = 0;
    bool on_target_ = false;
};

// ---------------------------------------------------------------------------
// SlaverManipulator — slaves one bone's rotation to follow another
// ---------------------------------------------------------------------------
class SlaverManipulator : public Manipulator {
public:
    void tick(f32 /*dt*/) override {} // slaving is resolved at render time
    bool is_at_goal() const override { return true; }

    void set_slave_bone(i32 b) { slave_bone_ = b; }
    void set_master_bone(i32 b) { master_bone_ = b; }
    i32 slave_bone() const { return slave_bone_; }
    i32 master_bone() const { return master_bone_; }

private:
    i32 slave_bone_ = 0;
    i32 master_bone_ = 0;
};

// ---------------------------------------------------------------------------
// CollisionDetectorManipulator — tracks bone positions for collision events
// ---------------------------------------------------------------------------
class CollisionDetectorManipulator : public Manipulator {
public:
    void tick(f32 /*dt*/) override {} // collision checks deferred
    bool is_at_goal() const override { return true; }

    void watch_bone(i32 bone_idx) { watched_bones_.push_back(bone_idx); }
    const std::vector<i32>& watched_bones() const { return watched_bones_; }

private:
    std::vector<i32> watched_bones_;
};

// ---------------------------------------------------------------------------
// FootPlantManipulator — IK foot placement on terrain
// ---------------------------------------------------------------------------
class FootPlantManipulator : public Manipulator {
public:
    void tick(f32 /*dt*/) override {} // IK deferred
    bool is_at_goal() const override { return true; }

    void set_foot_bone(i32 b) { foot_bone_ = b; }
    void set_knee_bone(i32 b) { knee_bone_ = b; }
    void set_hip_bone(i32 b) { hip_bone_ = b; }
    void set_straight_legs(bool s) { straight_legs_ = s; }
    void set_max_foot_fall(f32 m) { max_foot_fall_ = m; }

private:
    i32 foot_bone_ = 0;
    i32 knee_bone_ = 0;
    i32 hip_bone_ = 0;
    bool straight_legs_ = true;
    f32 max_foot_fall_ = 0;
};

// ---------------------------------------------------------------------------
// StorageManipulator — visual mass/energy storage fill indicator
// ---------------------------------------------------------------------------
class StorageManipulator : public Manipulator {
public:
    void tick(f32 /*dt*/) override {} // visual only
    bool is_at_goal() const override { return true; }
};

// ---------------------------------------------------------------------------
// ThrustManipulator — air unit thrust visual controller
// ---------------------------------------------------------------------------
class ThrustManipulator : public Manipulator {
public:
    void tick(f32 /*dt*/) override {} // visual only
    bool is_at_goal() const override { return true; }
};

} // namespace osc::sim
