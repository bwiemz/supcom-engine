#pragma once

#include "core/types.hpp"
#include "sim/entity.hpp" // Vector3

#include <functional>
#include <vector>

namespace osc::sim {
struct StateIO;
}

namespace osc::sim::path {

/// Moho's spline point states (faf-re CAiPathSpline.h EPathPointState).
enum class PointState : u8 {
    None = 0,         ///< PPS_0: no state carried on
    Settle = 1,       ///< PPS_1 (CAiPathSpline::Update's)
    Sidestep = 2,     ///< PPS_2 (CAiPathSpline::Update's)
    Stop = 3,         ///< PPS_3: braking to a stop on its goal
    BrakeForward = 4, ///< PPS_4: braking, to back up
    Reverse = 5,      ///< PPS_5
    BrakeReverse = 6, ///< PPS_6
    Forward = 7,      ///< PPS_7
    Done = 8,         ///< PPS_8
};

/// The spline's path type (CAiSteeringImpl::UpdatePath's mode).
enum class PathType : u8 {
    Normal = 0,   ///< PT_0: it stops on its destination
    TopSpeed = 1, ///< PT_1: it drives through at top speed (IAiSteering::UseTopSpeed)
    Sidestep = 2, ///< PT_2: stepping aside, at twice its acceleration, brake and turn
    Settle = 3,   ///< PT_3: braking to a stop (CAiPathSpline::Update mode 3)
    Brake = 4,    ///< PT_4: braking at twice its brake, for a unit in its way (mode 4)
};

/// What Moho's spline reads of a unit (its blueprint's Physics, its size and its
/// attribute multipliers).
struct SplineUnit {
    f32 max_speed = 0;         ///< Physics.MaxSpeed
    f32 max_speed_reverse = 0; ///< Physics.MaxSpeedReverse
    f32 max_accel = 0;         ///< Physics.MaxAcceleration
    f32 max_brake = 0;         ///< Physics.MaxBrake (0: its acceleration)
    f32 max_steer_force = 0;   ///< Physics.MaxSteerForce (0: its acceleration)
    f32 turn_rate = 0;         ///< Physics.TurnRate, degrees a second
    f32 turn_radius = 0;       ///< Physics.TurnRadius
    bool rotate_on_spot = false;
    f32 rotate_threshold = 0; ///< Physics.RotateOnSpotThreshold
    f32 backup_distance = 0;  ///< Physics.BackUpDistance
    f32 size_x = 0;           ///< the blueprint's SizeX
    f32 size_z = 0;           ///< the blueprint's SizeZ
    f32 speed_mult = 1;       ///< attributes.moveSpeedMult
    f32 accel_mult = 1;       ///< attributes.accelerationMult
    f32 turn_mult = 1;        ///< attributes.turnMult
    /// mInfoCache.mFormationTopSpeed: its own top speed, or its formation's.
    f32 top_speed = 0;
    /// 20; 5 for a unit whose formation is forming (sFormationPathNodeLimit).
    int node_limit = 20;
};

/// Where the unit is: its position, its heading (a unit vector in x and z) and
/// its velocity, a tick's step.
struct Pose {
    Vector3 position;
    Vector3 forward{0, 0, 1};
    Vector3 velocity;
};

/// A spline node (CPathPoint): where the unit stands that tick, facing.
struct Node {
    Vector3 position;
    Vector3 direction;
};

/// The ground the spline is generated over.
struct SplineWorld {
    /// UnitWontFitAt: the unit would not fit there.
    std::function<bool(f32 x, f32 z)> wont_fit;
    /// UnitIsBlockedAt(..., 2): units stand in the way there.
    std::function<bool(f32 x, f32 z)> blocked;
    /// A node's height: the ground, or the water for a unit that floats.
    std::function<f32(f32 x, f32 z)> height;
};

/// Moho's CAiPathSpline: the nodes a ground unit stands on, a tick apart,
/// generated from its pose toward a destination (Generate, 0x005B2FF0), and
/// followed a node a tick (CAiSteeringImpl's UpdateMotionPathPointers).
class Spline {
    friend struct osc::sim::StateIO; // snapshots (state_io.hpp)
public:
    /// Generate. `carry_on` starts it from the last spline's continuation, in its
    /// state (Moho's allowContinuation false); else it starts from `pose`, its
    /// state chosen from where the unit faces and how fast it goes. `at_goal`:
    /// the unit stands at `destination` (Unit::IsAtPosition), so one node ends it.
    void generate(const SplineUnit& unit, const Pose& pose, const Vector3& destination,
                  PathType type, bool carry_on, bool at_goal, const SplineWorld& world);

    /// The node to stand on this tick, the spline moved on past it; none past its end.
    const Node* advance();
    /// CAiPathSpline::Update (0x005B26A0): brake to a stop where the unit goes
    /// (mode 4 at twice its brake, and five still nodes on; mode 3 at its brake).
    void brake(const SplineUnit& unit, const Pose& pose, int mode, const SplineWorld& world);
    /// At its last node: the steering generates again (ProcessSplineMovement).
    bool at_end() const { return current_ + 1 >= nodes_.size(); }
    bool empty() const { return current_ >= nodes_.size(); }
    void clear();

    const std::vector<Node>& nodes() const { return nodes_; }
    size_t current() const { return current_; }
    PathType type() const { return type_; }

private:
    std::vector<Node> nodes_;
    size_t current_ = 0;
    PathType type_ = PathType::Normal;
    // SContinueInfo: the pose before its last node, and the state it was in.
    Vector3 cont_position_;
    Vector3 cont_direction_{0, 0, 1};
    Vector3 cont_velocity_;
    PointState cont_state_ = PointState::None;
};

/// Moho's ComputeSteeringSpeedCapFromParams (0x00699760), a tick's speed: with
/// RotateOnSpot and slower than its threshold, its top speed once its goal is
/// within acos 0.98, else none; else as fast as turns it onto its goal.
/// `delta_x`, `delta_z` and `forward_x`, `forward_z` are in Moho's steering plane.
f32 steering_speed_cap(f32 delta_x, f32 delta_z, f32 forward_x, f32 forward_z, f32 max_speed,
                       f32 turn_rate, f32 turn_radius, bool rotate_on_spot, f32 rotate_threshold,
                       f32 speed_ratio);

/// Moho's RotateDirectionTowardTargetLimited (0x006992C0): `source` turned
/// toward `target` by at most `max_turn` radians, the positive way on a tie.
void rotate_toward(f32 max_turn, f32 source_x, f32 source_z, f32 target_x, f32 target_z, f32& out_x,
                   f32& out_z);

} // namespace osc::sim::path
