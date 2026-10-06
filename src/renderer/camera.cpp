#include "renderer/camera.hpp"
#include "core/cursor.hpp"

#include "map/heightmap.hpp"
#include "map/terrain.hpp"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <optional>

namespace osc::renderer {

namespace {

constexpr f32 kDegToRad = Camera::kPi / 180.0f;
/// Far enough to cross any map from any eye.
constexpr f32 kReach = 40000.0f;

/// An angle into [-pi, pi], pi itself left be (NormalizeAngleSignedRadians).
f32 signed_angle(f32 a) {
    constexpr f32 kTwoPi = 2.0f * Camera::kPi;
    a = std::fmod(a, kTwoPi);
    if (a < -Camera::kPi) return a + kTwoPi;
    if (a > Camera::kPi) return a - kTwoPi;
    return a;
}

/// An angle into [-pi, pi], then unwrapped to lie within pi of `reference`
/// (NormalizeQuadrant).
f32 near_angle(f32 angle, f32 reference) {
    constexpr f32 kTwoPi = 2.0f * Camera::kPi;
    f32 a = std::fmod(angle, kTwoPi);
    if (a < -Camera::kPi) {
        a += kTwoPi;
    } else if (a > Camera::kPi) {
        a -= kTwoPi;
    }
    if (std::abs(a - reference) > Camera::kPi) a += a < reference ? kTwoPi : -kTwoPi;
    return a;
}

/// An entity's heading from its orientation (x, y, z, w).
f32 heading_of(const std::array<f32, 4>& q) {
    return std::atan2((q[2] * q[0] + q[1] * q[3]) * 2.0f,
                      1.0f - (q[1] * q[1] + q[0] * q[0]) * 2.0f);
}

/// An entity's pitch from its orientation: Moho's arcsine (COORDS_Pitch's
/// polynomial).
f32 pitch_of(const std::array<f32, 4>& q) {
    const f32 c = std::clamp((q[3] * q[2] - q[1] * q[0]) * -2.0f, -1.0f, 1.0f);
    const f32 poly = c * (c * (c * -0.018729299f + 0.074261002f) - 0.21211439f) + 1.5707288f;
    return 1.5707963f - std::sqrt(1.0f - c) * poly;
}

/// The cubic Hermite basis at t: start value, start tangent, end tangent,
/// end value.
struct Hermite {
    f32 start = 0.0f, start_tangent = 0.0f, end_tangent = 0.0f, end = 0.0f;
    explicit Hermite(f32 t) {
        const f32 tt = t * t;
        const f32 ttt = tt * t;
        start = ttt * 2.0f - tt * 3.0f + 1.0f;
        start_tangent = ttt - tt * 2.0f + t;
        end_tangent = ttt - tt;
        end = tt * 3.0f - ttt * 2.0f;
    }
    f32 blend(f32 from, f32 delta, f32 to) const {
        return start * from + start_tangent * delta + end_tangent * delta + end * to;
    }
};

} // namespace

void Camera::init(f32 map_width, f32 map_height) {
    rect_ = {0.0f, 0.0f, map_width, map_height};
    reset();
}

void Camera::set_playable_rect(f32 x0, f32 z0, f32 x1, f32 z1) {
    if (x1 <= x0 || z1 <= z0) return;
    rect_ = {x0, z0, x1, z1};
}

void Camera::set_viewport(f32 width, f32 height) {
    if (width <= 0.0f || height <= 0.0f) return;
    viewport_w_ = width;
    viewport_h_ = height;
}

void Camera::set_max_zoom_mult(f32 mult) {
    if (mult > 0.0f) max_zoom_mult_ = mult;
}

f32 Camera::max_zoom() const {
    // GetMaxZoom (ren_BorderSize 0): the wider of the rect's width and its
    // height at the view's aspect
    const f32 across = (rect_[2] - rect_[0]) * max_zoom_mult_;
    const f32 down = (rect_[3] - rect_[1]) * max_zoom_mult_ * (viewport_w_ / viewport_h_);
    return std::max(std::max(across, down), kNearZoom);
}

void Camera::reset() {
    fov_ = kFarFovDeg * kDegToRad;
    heading_ = kPi;
    rotated_ = false;
    revert_ = false;
    pitch_ = kFarPitchDeg * kDegToRad;
    target_ = {(rect_[0] + rect_[2]) * 0.5f, 0.0f, (rect_[1] + rect_[3]) * 0.5f};
    target_[1] = surface_height(target_[0], target_[2]);
    near_zoom_ = max_zoom();
    target_zoom_ = near_zoom_;
    focus_ = target_;
    eye_distance_ = target_zoom_ / std::tan(fov_ * 0.5f) / 2.0f;
    // A move under way runs on (CameraReset leaves it)
    end_pitch_ = pitch_;
    end_heading_ = kPi;
    nose_pitch_adjust_ = 0.0f;
    ease_in_out_ = true;
    heading_rate_ = 0.0f;
    zoom_rate_ = 0.0f;
    target_type_ = CameraTarget::Location;
    target_time_armed_ = false;
    target_time_left_ = 0.0f;
}

void Camera::zoom(f32 notches) {
    near_zoom_ = std::clamp(std::exp2(-zoom_amount_ * notches) * near_zoom_, kNearZoom, max_zoom());
}

void Camera::set_target(f32 x, f32 z) {
    target_[0] = x;
    target_[2] = z;
    target_[1] = surface_height(x, z);
    focus_ = target_;
    clamp_focus();
}

void Camera::set_zoom(f32 zoom) {
    near_zoom_ = std::clamp(zoom, kNearZoom, max_zoom());
    target_zoom_ = near_zoom_;
    fov_ = (kNearFovDeg + zoom_fraction() * (kFarFovDeg - kNearFovDeg)) * kDegToRad;
    if (!rotated_) pitch_ = zoom_pitch();
    eye_distance_ = target_zoom_ / std::tan(fov_ * 0.5f) / 2.0f;
}

void Camera::set_heading(f32 heading) {
    hold_rotation();
    heading_ = heading;
}

void Camera::set_pitch(f32 pitch) {
    hold_rotation();
    pitch_ = pitch;
}

void Camera::set_view(f32 x, f32 z, f32 zoom, f32 heading, f32 pitch) {
    hold_rotation();
    heading_ = heading;
    pitch_ = pitch;
    set_zoom(zoom);
    set_target(x, z);
}

void Camera::target_manual(f32 x, f32 y, f32 z, f32 heading, f32 pitch, f32 zoom, f32 seconds) {
    timed_move_init(seconds, 0.0f);
    end_pitch_ = pitch;
    // Unwrapped about the heading wrapped to +-pi, where the move starts.
    // faf-re's TargetManual reads the raw heading here; after a move that
    // ended past +-pi, the next across it would turn the long way round.
    end_heading_ = near_angle(heading, signed_angle(heading_));
    near_zoom_ = zoom;
    target_ = {x, y, z};
    if (seconds == 0.0f) {
        target_type_ = CameraTarget::Location;
        heading_ = end_heading_;
        pitch_ = end_pitch_;
        target_zoom_ = near_zoom_;
        focus_ = target_;
        rotated_ = true;
        clamp_focus();
        fov_ = (kNearFovDeg + zoom_fraction() * (kFarFovDeg - kNearFovDeg)) * kDegToRad;
        eye_distance_ = target_zoom_ / std::tan(fov_ * 0.5f) / 2.0f;
    } else {
        target_type_ = CameraTarget::Hermite;
        setup_hermite();
    }
}

void Camera::timed_move_init(f32 seconds, f32 transition) {
    // TimedMoveInit: the move's start, on the camera's clock
    move_focus_ = {};
    move_zoom_ = 0.0f;
    move_start_ = 0.0;
    move_pitch_ = 0.0f;
    move_heading_ = 0.0f;
    move_seconds_ = seconds;
    move_transition_ = transition;
    if (seconds > 0.0f) {
        move_start_ = now();
        move_focus_ = focus_;
        move_zoom_ = target_zoom_;
        move_pitch_ = pitch_;
        move_heading_ = signed_angle(heading_);
        signaled_ = false;
    }
}

void Camera::setup_hermite() {
    // With ease-in-out on the tangents stay as they were (zero until turned
    // off): an eased cubic. Off, both are the whole change: a straight line.
    if (ease_in_out_) return;
    for (int i = 0; i < 3; ++i) hermite_focus_[i] = target_[i] - move_focus_[i];
    hermite_heading_ = end_heading_ - move_heading_;
    hermite_pitch_ = end_pitch_ - move_pitch_;
    hermite_zoom_ = near_zoom_ - move_zoom_;
}

void Camera::target_box(const std::array<f32, 3>& min, const std::array<f32, 3>& max, f32 seconds) {
    timed_move_init(seconds, 0.0f);
    for (int i = 0; i < 3; ++i) target_[i] = (min[i] + max[i]) * 0.5f;
    near_zoom_ = std::max(max[0] - min[0], max[2] - min[2]);
    target_type_ = CameraTarget::Box;
    if (seconds == 0.0f) {
        target_zoom_ = near_zoom_;
        clamp_target();
        focus_ = target_;
        clamp_focus();
        fov_ = (kNearFovDeg + zoom_fraction() * (kFarFovDeg - kNearFovDeg)) * kDegToRad;
        eye_distance_ = target_zoom_ / std::tan(fov_ * 0.5f) / 2.0f;
    } else {
        setup_hermite();
    }
}

void Camera::target_entities(std::vector<u32> ids, bool track, f32 zoom, f32 seconds) {
    target_time_left_ = 0.0f;
    target_time_armed_ = false;
    target_ids_ = std::move(ids);
    active_target_ = 0;
    if (target_ids_.empty()) return;
    timed_move_init(seconds, 0.0f);
    CameraEntityPose pose;
    if (!target_pose(pose)) return;
    target_ = pose.pos;
    near_zoom_ = zoom;
    target_type_ = track ? CameraTarget::Entity : CameraTarget::Location;
    setup_hermite();
}

void Camera::target_nose_cam(std::vector<u32> ids, f32 pitch_adjust, f32 zoom, f32 seconds,
                             f32 transition) {
    target_time_left_ = 0.0f;
    target_time_armed_ = false;
    target_ids_ = std::move(ids);
    active_target_ = 0;
    if (target_ids_.empty()) return;
    timed_move_init(seconds, transition);
    CameraEntityPose pose;
    if (!target_pose(pose)) return;
    // The view at once; a move eases into it from where it was
    target_ = pose.pos;
    end_heading_ = near_angle(heading_of(pose.orient), move_heading_);
    end_pitch_ = pitch_of(pose.orient) + pitch_adjust;
    near_zoom_ = zoom;
    nose_pitch_adjust_ = pitch_adjust;
    pitch_ = end_pitch_;
    target_type_ = CameraTarget::NoseCam;
    heading_ = end_heading_;
    target_zoom_ = near_zoom_;
    focus_ = target_;
    clamp_focus();
    fov_ = (kNearFovDeg + zoom_fraction() * (kFarFovDeg - kNearFovDeg)) * kDegToRad;
    eye_distance_ = target_zoom_ / std::tan(fov_ * 0.5f) / 2.0f;
}

void Camera::target_nothing() {
    target_type_ = CameraTarget::Location;
    target_time_armed_ = false;
    target_time_left_ = 0.0f;
}

void Camera::spin_rates(f32 heading_rate, f32 zoom_rate) {
    heading_rate_ = heading_rate;
    zoom_rate_ = zoom_rate;
    rotated_ = true;
    target_type_ = CameraTarget::Hermite;
}

bool Camera::set_acc_mode(const std::string& name) {
    const auto is = [&](const char* want) {
        if (name.size() != std::char_traits<char>::length(want)) return false;
        for (size_t i = 0; i < name.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(name[i])) !=
                std::tolower(static_cast<unsigned char>(want[i])))
                return false;
        }
        return true;
    };
    if (is("Linear")) {
        accel_ = CameraAccel::Linear;
    } else if (is("FastInSlowOut")) {
        accel_ = CameraAccel::FastInSlowOut;
    } else if (is("SlowInOut")) {
        accel_ = CameraAccel::SlowInOut;
    } else {
        return false;
    }
    return true;
}

bool Camera::target_pose(CameraEntityPose& out) const {
    if (!entity_lookup_ || active_target_ >= target_ids_.size()) return false;
    return entity_lookup_(target_ids_[active_target_], out);
}

void Camera::target_next_entity() {
    // TargetNextEntity: on round the list to a live one, dropping the gone
    while (!target_ids_.empty()) {
        const size_t next = (active_target_ + 1) % target_ids_.size();
        active_target_ = next;
        CameraEntityPose pose;
        if (target_pose(pose)) {
            target_type_ = CameraTarget::Entity;
            target_time_left_ = 0.0f;
            target_time_armed_ = false;
            return;
        }
        target_ids_.erase(target_ids_.begin() + static_cast<std::ptrdiff_t>(next));
        if (target_ids_.empty()) return;
        // The one after the gone now sits where it was: step back so the
        // next turn lands on it
        active_target_ = (next + target_ids_.size() - 1) % target_ids_.size();
    }
}

void Camera::signal() {
    signaled_ = true;
    std::vector<std::function<void()>> waiters;
    waiters.swap(waiters_);
    for (auto& w : waiters) w();
}

void Camera::set_eye_distance(f32 distance) {
    // eye = zoom / tan(fov(zoom) / 2) / 2, the FOV slow in the zoom: a fixed
    // point
    hold_rotation();
    f32 zoom = 2.0f * distance * std::tan(fov_ * 0.5f);
    for (int i = 0; i < 16; ++i) {
        target_zoom_ = zoom;
        fov_ = (kNearFovDeg + zoom_fraction() * (kFarFovDeg - kNearFovDeg)) * kDegToRad;
        zoom = 2.0f * distance * std::tan(fov_ * 0.5f);
    }
    near_zoom_ = zoom;
    target_zoom_ = zoom;
    fov_ = (kNearFovDeg + zoom_fraction() * (kFarFovDeg - kNearFovDeg)) * kDegToRad;
    eye_distance_ = target_zoom_ / std::tan(fov_ * 0.5f) / 2.0f;
}

void Camera::revert_rotation() {
    if (!rotated_) return;
    revert_ = true;
    if (target_type_ != CameraTarget::Entity) target_type_ = CameraTarget::Location;
}

f32 Camera::zoom_fraction() const {
    // The log of the target zoom between the nearest and the farthest
    const f32 log_max = std::log(max_zoom());
    const f32 log_near = std::log(kNearZoom);
    if (log_max <= log_near) return 1.0f;
    const f32 log_zoom = std::clamp(std::log(target_zoom_), log_near, log_max);
    return (log_zoom - log_near) / (log_max - log_near);
}

f32 Camera::zoom_pitch() const {
    return (kNearPitchDeg + zoom_fraction() * (kFarPitchDeg - kNearPitchDeg)) * kDegToRad;
}

std::array<f32, 3> Camera::direction() const {
    const f32 cp = std::cos(pitch_);
    return {std::sin(heading_) * cp, -std::sin(pitch_), std::cos(heading_) * cp};
}

void Camera::pan(f32 dx, f32 dy) {
    // CameraPan: along the view's right for x, its up flattened onto the
    // ground for y, a pixel the zoom over the view's width
    const std::array<f32, 16> v = view();
    const f32 right[3] = {v[0], v[4], v[8]};
    f32 fx = v[1];
    f32 fz = v[9];
    const f32 len = std::sqrt(fx * fx + fz * fz);
    if (len > 1e-6f) {
        fx /= len;
        fz /= len;
    }
    const f32 scale = target_zoom_ / viewport_w_ * kPanSpeed;
    const f32 px = scale * dx;
    const f32 py = scale * dy;
    target_[0] += -px * right[0] + fx * py;
    target_[1] += -px * right[1];
    target_[2] += -px * right[2] + fz * py;
}

void Camera::spin(f32 dx, f32 dy) {
    // CameraSpin: cam_SpinSpeed degrees across the view's width
    const f32 scale = kSpinSpeedDeg / viewport_w_ * kDegToRad;
    hold_rotation();
    heading_ = signed_angle(heading_ - dx * scale);
    pitch_ = std::clamp(pitch_ + dy * scale, kMinSpinPitch, kMaxSpinPitch);
}

f32 Camera::surface_height(f32 x, f32 z) const {
    if (!ground_) return target_[1];
    const f32 h = ground_->get_height(x, z);
    return has_water_ ? std::max(h, water_elevation_) : h;
}

bool Camera::surface_hit(const f32 from[3], const f32 dir[3], f32 reach, f32 out[3]) const {
    if (!ground_) return false;
    std::optional<f32> t =
        ground_->intersect(from[0], from[1], from[2], dir[0], dir[1], dir[2], 0.0f, reach);
    if (has_water_ && std::abs(dir[1]) > 1e-6f) {
        const f32 tw = (water_elevation_ - from[1]) / dir[1];
        const f32 wx = from[0] + dir[0] * tw;
        const f32 wz = from[2] + dir[2] * tw;
        const bool over_map = wx >= 0.0f && wz >= 0.0f &&
                              wx <= static_cast<f32>(ground_->map_width()) &&
                              wz <= static_cast<f32>(ground_->map_height());
        if (tw >= 0.0f && tw <= reach && over_map && (!t || tw < *t)) t = tw;
    }
    if (!t) return false;
    for (int i = 0; i < 3; ++i) out[i] = from[i] + dir[i] * *t;
    return true;
}

void Camera::clamp_target() {
    // ClampTargetPos: inside the rect, less the zoom's share of its half
    const f32 share = std::clamp(target_zoom_, 0.0f, max_zoom()) / max_zoom();
    const f32 half_x = share * (rect_[2] - rect_[0]) * 0.5f;
    const f32 half_z = share * (rect_[3] - rect_[1]) * 0.5f;
    target_[0] = std::max(std::min(target_[0], rect_[2] - half_x), rect_[0] + half_x);
    target_[2] = std::max(std::min(target_[2], rect_[3] - half_z), rect_[1] + half_z);
}

void Camera::clamp_focus() {
    // ClampFocusPos: the view's line through the target, onto the surface
    const std::array<f32, 3> d = direction();
    const f32 from[3] = {focus_[0] - d[0] * kReach, focus_[1] - d[1] * kReach,
                         focus_[2] - d[2] * kReach};
    f32 hit[3];
    if (surface_hit(from, d.data(), kReach * 2.0f, hit)) focus_ = {hit[0], hit[1], hit[2]};
}

void Camera::frame(f64 dt) {
    // Frame: on the game clock the step is the game time's
    if (game_clock_) dt = game_time_ - last_game_time_;
    const auto seconds = static_cast<f32>(dt);
    update_targets(seconds);
    if (move_seconds_ > 0.0f) {
        interpolate_basis();
    } else {
        update_basis(seconds);
    }
    // UpdateCoords: the eye's distance from the zoom and the FOV
    eye_distance_ = target_zoom_ / std::tan(fov_ * 0.5f) / 2.0f;
    last_game_time_ = game_time_;
    decay_shake();
}

void Camera::update_targets(f32 dt) {
    // A target gone: the next in the list, once its time is up
    if (target_time_armed_) {
        target_time_left_ = std::max(0.0f, target_time_left_ - dt);
        if (target_time_left_ == 0.0f) target_next_entity();
    }
    if (target_type_ == CameraTarget::Entity || target_type_ == CameraTarget::NoseCam) {
        CameraEntityPose pose;
        if (!target_pose(pose)) {
            // Gone: a location, a turned view turned back
            target_time_armed_ = true;
            target_type_ = CameraTarget::Location;
            if (rotated_) revert_ = true;
            return;
        }
        target_ = pose.pos;
        if (target_type_ == CameraTarget::NoseCam) {
            end_pitch_ = pitch_of(pose.orient) + nose_pitch_adjust_;
            end_heading_ = near_angle(heading_of(pose.orient), move_heading_);
        }
        return;
    }
    if (target_type_ == CameraTarget::Hermite) {
        // Spin's rates: revolutions and zoom a second
        end_heading_ += heading_rate_ * dt * 2.0f * kPi;
        near_zoom_ += zoom_rate_ * dt;
    }
}

void Camera::update_basis(f32 seconds) {
    // UpdateBasis: the target zoom glides toward the zoom asked for in log2
    // space, by at most (|delta| * 8 + 1) a second
    const f32 start = target_zoom_;
    const f32 log_start = std::log2(start);
    const f32 delta = std::log2(near_zoom_) - log_start;
    const f32 step =
        std::min(std::abs(delta), (std::abs(delta) * kZoomSpeedLarge + kZoomSpeedSmall) * seconds);
    target_zoom_ = std::exp2(log_start + std::copysign(step, delta));
    if (!rotated_) target_zoom_ = std::clamp(target_zoom_, kNearZoom, max_zoom());

    // Closing in on a place, the ground under the pivot keeps its place on
    // the screen: the target is drawn toward it by the zoom's ratio
    const bool anchored =
        target_type_ == CameraTarget::Location || target_type_ == CameraTarget::Hermite;
    if (anchored && start > target_zoom_ && start > 0.0f) {
        f32 o[3];
        f32 d[3];
        f32 hit[3];
        if (screen_ray(pivot_x_, pivot_y_, viewport_w_, viewport_h_, o, d) &&
            surface_hit(o, d, kReach, hit)) {
            const f32 scale = std::max(0.0f, std::min(target_zoom_, start)) / start;
            for (int i = 0; i < 3; ++i) target_[i] = (target_[i] - hit[i]) * scale + hit[i];
        }
    }
    if (!free_) clamp_target();
    focus_ = target_;
    fov_ = (kNearFovDeg + zoom_fraction() * (kFarFovDeg - kNearFovDeg)) * kDegToRad;

    // The heading and pitch by the target: a nose's the entity's, a
    // Hermite's (a finished move, a spin) its own
    if (target_type_ == CameraTarget::NoseCam) {
        CameraEntityPose pose;
        if (target_pose(pose)) {
            heading_ = heading_of(pose.orient);
            pitch_ = pitch_of(pose.orient) + nose_pitch_adjust_;
        }
        clamp_focus();
        return;
    }
    if (target_type_ == CameraTarget::Hermite) {
        heading_ = end_heading_;
        pitch_ = end_pitch_;
        clamp_focus();
        return;
    }
    if (!rotated_) {
        pitch_ = zoom_pitch();
    } else if (revert_) {
        // Back toward heading +-pi and the zoom's pitch, 0.1 a frame
        const f32 want_pitch = zoom_pitch();
        const f32 want_heading = heading_ <= 0.0f ? -kPi : kPi;
        pitch_ = std::clamp(want_pitch, pitch_ - kRevertStep, pitch_ + kRevertStep);
        heading_ = std::clamp(want_heading, heading_ - kRevertStep, heading_ + kRevertStep);
        if (std::abs(heading_ - want_heading) < kRevertStep &&
            std::abs(pitch_ - want_pitch) < kRevertStep) {
            heading_ = kPi;
            pitch_ = want_pitch;
            rotated_ = false;
            revert_ = false;
        }
    }
    clamp_focus();
}

void Camera::interpolate_basis() {
    // InterpolateBasis: the move's progress on its clock
    const auto progress = static_cast<f32>((now() - move_start_) / move_seconds_);
    if (target_type_ == CameraTarget::Entity) {
        CameraEntityPose pose;
        if (target_pose(pose)) target_ = pose.pos;
    }
    const bool steered =
        target_type_ == CameraTarget::Hermite || target_type_ == CameraTarget::NoseCam;
    if (progress >= 1.0f) {
        // There: the end, held
        focus_ = target_;
        move_seconds_ = 0.0f;
        target_zoom_ = near_zoom_;
        signal();
        if (steered) {
            rotated_ = true;
            heading_ = end_heading_;
            // faf-re's recovery reads the start pitch here, which would
            // snap every move back; its UpdateBasis holds the end pitch
            pitch_ = end_pitch_;
        }
    } else {
        // A nose move's curve runs on its transition's own seconds
        f32 input = progress;
        if (target_type_ == CameraTarget::NoseCam) {
            input = 1.0f;
            if (move_transition_ > 0.0f) {
                input = std::min(1.0f, static_cast<f32>((now() - move_start_) / move_transition_));
            }
        }
        f32 t = input;
        if (accel_ == CameraAccel::FastInSlowOut) {
            t = std::sin(input * kPi * 0.5f);
        } else if (accel_ == CameraAccel::SlowInOut) {
            t = progress < 0.5f ? (1.0f - std::cos(input * kPi)) * 0.5f
                                : std::sin((input - 0.5f) * kPi) * 0.5f + 0.5f;
        }
        const Hermite w(t);
        if (steered) {
            pitch_ = w.blend(move_pitch_, hermite_pitch_, end_pitch_);
            heading_ = w.blend(move_heading_, hermite_heading_, end_heading_);
            rotated_ = true;
        }
        target_zoom_ = w.blend(move_zoom_, hermite_zoom_, near_zoom_);
        for (int i = 0; i < 3; ++i)
            focus_[i] = w.blend(move_focus_[i], hermite_focus_[i], target_[i]);
    }
    // A place's or a box's pitch follows the zoom, as the basis's does
    if (!steered && target_type_ != CameraTarget::Entity) pitch_ = zoom_pitch();
    fov_ = (kNearFovDeg + zoom_fraction() * (kFarFovDeg - kNearFovDeg)) * kDegToRad;
    // The focus onto the ground ahead of it along the view
    const std::array<f32, 3> d = direction();
    f32 hit[3];
    if (surface_hit(focus_.data(), d.data(), kReach, hit)) focus_ = {hit[0], hit[1], hit[2]};
    if (progress >= 1.0f && target_type_ != CameraTarget::NoseCam &&
        target_type_ != CameraTarget::Entity)
        target_type_ = CameraTarget::Location;
}

void Camera::update(GLFWwindow* window, f64 dt) {
    if (!input_enabled_ || !window) {
        frame(dt);
        return;
    }
    // Moho's MAUI_KeyIsDown: no key while a control has the keyboard
    const auto key = [&](int k) { return keys_enabled_ && glfwGetKey(window, k) == GLFW_PRESS; };
    CameraInput in;
    in.up = key(GLFW_KEY_UP);
    in.down = key(GLFW_KEY_DOWN);
    in.left = key(GLFW_KEY_LEFT);
    in.right = key(GLFW_KEY_RIGHT);
    in.insert = key(GLFW_KEY_INSERT);
    in.del = key(GLFW_KEY_DELETE);
    in.space = key(GLFW_KEY_SPACE);
    in.control = key(GLFW_KEY_LEFT_CONTROL) || key(GLFW_KEY_RIGHT_CONTROL);
    in.alt = key(GLFW_KEY_LEFT_ALT) || key(GLFW_KEY_RIGHT_ALT);
    in.middle = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS;
    f64 mx = 0;
    f64 my = 0;
    glfwGetCursorPos(window, &mx, &my);
    {
        // The pointer in framebuffer pixels, the viewport's units (M217h)
        int ww = 0;
        int wh = 0;
        int fw = 0;
        int fh = 0;
        glfwGetWindowSize(window, &ww, &wh);
        glfwGetFramebufferSize(window, &fw, &fh);
        const auto p = core::to_framebuffer(mx, my, ww, wh, fw, fh);
        in.mouse_x = static_cast<f32>(p[0]);
        in.mouse_y = static_cast<f32>(p[1]);
    }
    // The edges only while the pointer is over the window: a focused window
    // whose cursor is elsewhere reports a stale position
    if (glfwGetWindowAttrib(window, GLFW_FOCUSED) && glfwGetWindowAttrib(window, GLFW_HOVERED)) {
        int win_w = 0;
        int win_h = 0;
        glfwGetWindowSize(window, &win_w, &win_h);
        in.at_left = mx <= 0.0;
        in.at_top = my <= 0.0;
        in.at_right = mx >= win_w - 1;
        in.at_bottom = my >= win_h - 1;
    }
    apply(in, dt);
}

void Camera::apply(const CameraInput& in, f64 dt) {
    if (!input_enabled_) {
        frame(dt);
        return;
    }
    const f32 pan_speed = keyboard_pan_speed_ * (in.control ? keyboard_pan_accelerate_ : 1.0f);
    const f32 rotate_speed =
        keyboard_rotate_speed_ * (in.control ? keyboard_rotate_accelerate_ : 1.0f);
    const f32 dx = in.mouse_x - last_mouse_x_;
    const f32 dy = in.mouse_y - last_mouse_y_;

    // Space and the mouse spin the view while near; let go, it turns back
    if (in.space && (free_ || target_zoom_ < kSpinZoomLimit)) {
        // Over the world: the first motion arms it, the rest turn
        if (mouse_enabled_) {
            if (spinning_) spin(dx, dy);
            spinning_ = true;
        }
    } else {
        if (spinning_ && !free_) revert_rotation();
        spinning_ = false;
    }

    // Insert and Delete turn it (not with Alt)
    if (!in.alt) {
        if (in.insert) {
            spin(rotate_speed, 0.0f);
        } else if (in.del) {
            spin(-rotate_speed, 0.0f);
        }
    }

    // The screen's edges and the arrow keys pan, a fixed step a frame
    f32 px = 0.0f;
    f32 py = 0.0f;
    // (each as its option allows: ui_ScreenEdgeScrollView,
    // ui_ArrowKeysScrollView)
    if (edge_scroll_) {
        if (in.at_left) px = pan_speed;
        if (in.at_top) py = pan_speed;
        if (in.at_right) px -= pan_speed;
        if (in.at_bottom) py -= pan_speed;
    }
    if (arrow_scroll_) {
        if (in.up) py += pan_speed;
        if (in.down) py -= pan_speed;
        if (in.left) px += pan_speed;
        if (in.right) px -= pan_speed;
    }
    if (!in.alt && (px != 0.0f || py != 0.0f)) pan(px, py);

    // The middle button drags the ground (CameraDragger), from over the
    // world; letting go turns a rotated view back
    if (in.middle && (dragging_ || mouse_enabled_)) {
        if (dragging_) pan(invert_middle_ ? -dx : dx, invert_middle_ ? -dy : dy);
        dragging_ = true;
    } else if (dragging_) {
        dragging_ = false;
        if (!free_) revert_rotation();
    }

    // The pivot follows the cursor over the world
    if (mouse_enabled_) set_pivot(in.mouse_x, in.mouse_y);
    last_mouse_x_ = in.mouse_x;
    last_mouse_y_ = in.mouse_y;
    frame(dt);
}

void Camera::decay_shake() {
    if (shake_intensity_ > 0.01f)
        shake_intensity_ *= 0.9f;
    else
        shake_intensity_ = 0;
}

void Camera::apply_shake(f32 intensity) {
    shake_intensity_ = std::max(shake_intensity_, intensity);
}

f32 Camera::near_clip() const {
    return std::max(eye_distance_ * 0.01f, 0.01f);
}

f32 Camera::far_clip() const {
    return eye_distance_ + 17000.0f;
}

f32 Camera::tan_half_fov_y(f32 aspect) const {
    // VEC_D3DProjectionMatrixFOV with fovX = fovY: the FOV spans the wider
    // side
    const f32 t = std::tan(fov_ * 0.5f);
    return aspect > 1.0f ? t / aspect : t;
}

std::array<f32, 16> Camera::projection(f32 aspect) const {
    return math::perspective(2.0f * std::atan(tan_half_fov_y(aspect)), aspect, near_clip(),
                             far_clip());
}

void Camera::eye_position(f32& x, f32& y, f32& z) const {
    const std::array<f32, 3> d = direction();
    x = focus_[0] - d[0] * eye_distance_;
    y = focus_[1] - d[1] * eye_distance_;
    z = focus_[2] - d[2] * eye_distance_;
}

std::array<f32, 16> Camera::view() const {
    f32 ex = 0;
    f32 ey = 0;
    f32 ez = 0;
    eye_position(ex, ey, ez);
    return math::look_at(ex, ey, ez, focus_[0], focus_[1], focus_[2], 0.0f, 1.0f, 0.0f);
}

std::array<f32, 16> Camera::view_proj(f32 aspect) const {
    f32 ex = 0;
    f32 ey = 0;
    f32 ez = 0;
    eye_position(ex, ey, ez);
    f32 fx = focus_[0];
    f32 fz = focus_[2];
    if (shake_intensity_ > 0.01f) {
        // A deterministic offset from the intensity, varying as it decays
        const f32 phase = shake_intensity_ * 137.5f;
        const f32 ox = std::sin(phase * 3.7f) * shake_intensity_;
        const f32 oz = std::cos(phase * 2.3f) * shake_intensity_;
        fx += ox;
        fz += oz;
        ex += ox;
        ez += oz;
    }
    const auto v = math::look_at(ex, ey, ez, fx, focus_[1], fz, 0.0f, 1.0f, 0.0f);
    return math::mat4_mul(projection(aspect), v);
}

// --- Matrix math ---

namespace math {

std::array<f32, 16> look_at(f32 ex, f32 ey, f32 ez,
                            f32 tx, f32 ty, f32 tz,
                            f32 ux, f32 uy, f32 uz) {
    // Forward (camera looks along -Z in view space)
    f32 fx = tx - ex, fy = ty - ey, fz = tz - ez;
    f32 fl = std::sqrt(fx * fx + fy * fy + fz * fz);
    if (fl > 0) { fx /= fl; fy /= fl; fz /= fl; }

    // Right = forward x up
    f32 rx = fy * uz - fz * uy;
    f32 ry = fz * ux - fx * uz;
    f32 rz = fx * uy - fy * ux;
    f32 rl = std::sqrt(rx * rx + ry * ry + rz * rz);
    if (rl > 0) { rx /= rl; ry /= rl; rz /= rl; }

    // True up = right x forward
    f32 tux = ry * fz - rz * fy;
    f32 tuy = rz * fx - rx * fz;
    f32 tuz = rx * fy - ry * fx;

    // Column-major 4x4
    return {
        rx,  tux, -fx, 0,
        ry,  tuy, -fy, 0,
        rz,  tuz, -fz, 0,
        -(rx * ex + ry * ey + rz * ez),
        -(tux * ex + tuy * ey + tuz * ez),
        (fx * ex + fy * ey + fz * ez),
        1
    };
}

std::array<f32, 16> perspective(f32 fov_rad, f32 aspect, f32 near, f32 far) {
    f32 t = std::tan(fov_rad * 0.5f);
    f32 range = far - near;

    // Vulkan: [0,1] depth range, Y-flip (negate [1][1])
    return {
        1.0f / (aspect * t), 0,           0,                         0,
        0,                   -1.0f / t,   0,                         0,
        0,                    0,          -far / range,              -1,
        0,                    0,          -(far * near) / range,      0
    };
}

std::array<f32, 16> ortho(f32 l, f32 r, f32 b, f32 t, f32 n, f32 f) {
    // Column-major, Vulkan [0,1] depth, no Y-flip (shadow maps)
    return {
        2.0f / (r - l),     0,                  0,                  0,
        0,                  2.0f / (t - b),     0,                  0,
        0,                  0,                 -1.0f / (f - n),     0,
        -(r + l) / (r - l), -(t + b) / (t - b), -n / (f - n),      1
    };
}

std::array<f32, 16> mat4_mul(const std::array<f32, 16>& a,
                             const std::array<f32, 16>& b) {
    std::array<f32, 16> r{};
    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 4; row++) {
            f32 sum = 0;
            for (int k = 0; k < 4; k++)
                sum += a[k * 4 + row] * b[col * 4 + k];
            r[col * 4 + row] = sum;
        }
    }
    return r;
}

} // namespace math

bool Camera::screen_ray(f32 screen_x, f32 screen_y, f32 window_w, f32 window_h, f32 origin[3],
                        f32 dir[3]) const {
    // Convert screen pixel to NDC [-1, 1]. Screen y runs down, and our
    // perspective flips Y for Vulkan (view-space up lands at the top of the
    // screen), so a point up the screen lies along +up: the ray takes -ndc_y.
    // (It had taken +ndc_y, which sent every click to the point mirrored
    // about the view's centre line; view_proj and WorldView::project agree
    // with what is drawn.)
    f32 ndc_x = (2.0f * screen_x / window_w) - 1.0f;
    f32 ndc_y = 1.0f - (2.0f * screen_y / window_h);

    f32 aspect = window_w / window_h;

    eye_position(origin[0], origin[1], origin[2]);
    const auto v = view();
    // Right/up/forward from the view matrix (column-major, transposed rotation)
    f32 rx = v[0], ry = v[4], rz = v[8];     // right
    f32 ux = v[1], uy = v[5], uz = v[9];     // up
    f32 fx = -v[2], fy = -v[6], fz = -v[10]; // forward (negated -Z)

    // Half-angles from the projection
    const f32 tan_half = tan_half_fov_y(aspect);

    // Direction in world space
    f32 dx = fx + ndc_x * aspect * tan_half * rx + ndc_y * tan_half * ux;
    f32 dy = fy + ndc_x * aspect * tan_half * ry + ndc_y * tan_half * uy;
    f32 dz = fz + ndc_x * aspect * tan_half * rz + ndc_y * tan_half * uz;

    f32 len = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (len < 1e-6f) return false;
    dir[0] = dx / len;
    dir[1] = dy / len;
    dir[2] = dz / len;
    return true;
}

bool Camera::screen_to_world(f32 screen_x, f32 screen_y,
                              f32 window_w, f32 window_h,
                              f32 ground_y,
                              f32& out_x, f32& out_z) const {
    f32 o[3], d[3];
    if (!screen_ray(screen_x, screen_y, window_w, window_h, o, d)) return false;
    // Intersect ray (eye + t*dir) with y = ground_y plane
    if (std::abs(d[1]) < 1e-6f) return false; // ray parallel to ground
    f32 t = (ground_y - o[1]) / d[1];
    if (t < 0) return false; // intersection behind camera

    out_x = o[0] + t * d[0];
    out_z = o[2] + t * d[2];
    return true;
}

bool Camera::pick_ground(f32 screen_x, f32 screen_y, f32 window_w, f32 window_h,
                         const map::Terrain* terrain, f32& out_x, f32& out_y, f32& out_z) const {
    f32 o[3], d[3];
    if (!screen_ray(screen_x, screen_y, window_w, window_h, o, d)) return false;
    std::optional<f32> t;
    if (terrain) {
        // Well past any map's far corner from any eye.
        constexpr f32 kReach = 20000.0f;
        t = terrain->heightmap().intersect(o[0], o[1], o[2], d[0], d[1], d[2], 0.0f, kReach);
        // The water's surface, where the ray meets it before the ground.
        if (terrain->has_water() && d[1] < -1e-6f) {
            const f32 tw = (terrain->water_elevation() - o[1]) / d[1];
            if (tw >= 0.0f && (!t || tw < *t)) t = tw;
        }
    }
    if (!t) {
        // Off the map (or no terrain): the level of the camera's focus.
        if (std::abs(d[1]) < 1e-6f) return false;
        const f32 tp = (focus_[1] - o[1]) / d[1];
        if (tp < 0.0f) return false;
        t = tp;
    }
    out_x = o[0] + *t * d[0];
    out_y = o[1] + *t * d[1];
    out_z = o[2] + *t * d[2];
    return true;
}

} // namespace osc::renderer
