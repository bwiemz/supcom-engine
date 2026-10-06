#pragma once

#include "core/types.hpp"

#include <array>
#include <functional>
#include <string>
#include <vector>

struct GLFWwindow;

namespace osc::map {
class Heightmap;
class Terrain;
}

namespace osc::renderer {

/// The world view's input for a frame, as CUIWorldView reads it.
struct CameraInput {
    bool up = false, down = false, left = false, right = false; ///< the arrow keys
    bool insert = false, del = false, space = false;
    bool control = false, alt = false;
    bool middle = false; ///< the middle button
    /// The cursor on the screen's edges (windowed, focused and over it).
    bool at_left = false, at_right = false, at_top = false, at_bottom = false;
    f32 mouse_x = 0.0f, mouse_y = 0.0f;
};

/// Where the camera aims (Moho's target types, M217g).
enum class CameraTarget : u8 { Location, Box, Entity, NoseCam, Hermite };
/// A move's pace (SetAccMode).
enum class CameraAccel : u8 { Linear, FastInSlowOut, SlowInOut };
/// An entity as the camera sees it: its interpolated position, and its
/// orientation (x, y, z, w).
struct CameraEntityPose {
    std::array<f32, 3> pos{};
    std::array<f32, 4> orient{0.0f, 0.0f, 0.0f, 1.0f};
};

/// Moho's world camera (CameraImpl, M217f). The zoom is the world's extent
/// across the view at its focus.
///
/// It glides in log space toward the zoom asked for, and the pitch and field
/// of view follow its log:
/// - 40 degrees and 65 at the nearest zoom;
/// - 89.9 and 60 at the farthest.
///
/// The middle button drags the ground; the arrow keys and the screen's edges
/// pan; Space (while near) and Insert/Delete spin the view, which glides back
/// when let go. frame() is UpdateBasis then UpdateCoords.
class Camera {
public:
    // Moho's tunables (cam_*, ui_Keyboard*, ren_BgLowerBound)
    static constexpr f32 kNearZoom = 5.0f;
    static constexpr f32 kNearPitchDeg = 40.0f;
    static constexpr f32 kFarPitchDeg = 89.9f;
    static constexpr f32 kNearFovDeg = 65.0f;
    static constexpr f32 kFarFovDeg = 60.0f;
    static constexpr f32 kZoomAmount = 0.05f;
    static constexpr f32 kZoomSpeedLarge = 8.0f;
    static constexpr f32 kZoomSpeedSmall = 1.0f;
    static constexpr f32 kSpinSpeedDeg = 360.0f;
    static constexpr f32 kMinSpinPitch = 0.1f;
    static constexpr f32 kMaxSpinPitch = 1.5607964f;
    static constexpr f32 kPanSpeed = 1.0f;
    static constexpr f32 kDefaultMaxZoomMult = 1.4f;
    /// Space spins the view only nearer than this zoom (ren_BgLowerBound).
    static constexpr f32 kSpinZoomLimit = 125.0f;
    static constexpr f32 kKeyboardPanSpeed = 90.0f;
    static constexpr f32 kKeyboardPanAccelerate = 4.0f;
    static constexpr f32 kKeyboardRotateSpeed = 10.0f;
    static constexpr f32 kKeyboardRotateAccelerate = 2.0f;
    /// A rotated view's heading and pitch step back this far a frame.
    static constexpr f32 kRevertStep = 0.1f;
    static constexpr f32 kPi = 3.14159265f;

    /// The map's extent (the playable rect, until set_playable_rect), and
    /// CameraReset.
    void init(f32 map_width, f32 map_height);
    /// The ground the focus sits on and the zoom-in's pivot meets: its
    /// heights, and the water over them (nullptr: none, the target's level).
    void set_ground(const map::Heightmap* heights, bool has_water, f32 water_elevation) {
        ground_ = heights;
        has_water_ = has_water;
        water_elevation_ = water_elevation;
    }
    /// The playable rect (SetPlayableRect), which the zoom and target keep to.
    void set_playable_rect(f32 x0, f32 z0, f32 x1, f32 z1);
    /// The view's size in pixels: its aspect, and the pan's and spin's scale.
    void set_viewport(f32 width, f32 height);

    /// This frame's input from the window, then apply().
    void update(GLFWwindow* window, f64 dt);
    /// The world view's input for this frame (CUIWorldView::Frame and
    /// HandleEvent): keyboard pan and spin, Space spin and its revert, the
    /// middle-button drag. Then frame().
    void apply(const CameraInput& input, f64 dt);
    /// UpdateBasis then UpdateCoords: the zoom's glide, the clamps, the FOV
    /// and pitch, the focus on the ground.
    void frame(f64 dt);

    // --- CameraImpl's operations ---
    /// CameraZoom: `notches` of the wheel (out is negative), about the pivot.
    void zoom(f32 notches);
    /// CameraSetPivot: the screen point a zoom-in closes on (the cursor).
    void set_pivot(f32 screen_x, f32 screen_y) {
        pivot_x_ = screen_x;
        pivot_y_ = screen_y;
    }
    /// CameraPan: a drag of (dx, dy) pixels across the ground.
    void pan(f32 dx, f32 dy);
    /// CameraSpin: (dx, dy) pixels of turn and tilt.
    void spin(f32 dx, f32 dy);
    void hold_rotation() {
        rotated_ = true;
        revert_ = false;
    }
    void revert_rotation();
    /// CameraReset: the whole map from above.
    void reset();

    // --- A pinned view (tests, tools, scripts) ---
    /// The target on the ground, at once.
    void set_target(f32 x, f32 z);
    /// The zoom, at once (no glide).
    void set_zoom(f32 zoom);
    /// CameraSetHeading / CameraSetPitch: held until reverted.
    void set_heading(f32 heading);
    void set_pitch(f32 pitch);
    /// A whole view, held: target, zoom, heading and pitch.
    void set_view(f32 x, f32 z, f32 zoom, f32 heading, f32 pitch);
    /// TargetManual: the target, heading (unwrapped nearest the current),
    /// pitch and zoom. At once (0 seconds), unclamped and held; else a
    /// Hermite move over `seconds`.
    void target_manual(f32 x, f32 y, f32 z, f32 heading, f32 pitch, f32 zoom, f32 seconds = 0.0f);
    /// SetTargetZoom: the zoom the view glides toward, as given.
    void set_requested_zoom(f32 zoom) { near_zoom_ = zoom; }
    /// The zoom (held, at once) that puts the eye `distance` from the focus:
    /// a view framed by its eye (tests, tools).
    void set_eye_distance(f32 distance);
    /// cam_Free: the target isn't kept in the map, Space spins at any zoom,
    /// and a spin isn't reverted when let go.
    void set_free(bool free) { free_ = free; }
    bool free() const { return free_; }
    void set_max_zoom_mult(f32 mult);

    // --- Moves (M217g) ---
    /// An entity's pose by id (the frame's interpolated view), for the
    /// targets that follow one; false when it is gone.
    using EntityLookup = std::function<bool(u32 id, CameraEntityPose& out)>;
    void set_entity_lookup(EntityLookup lookup) { entity_lookup_ = std::move(lookup); }
    /// This frame's clocks: the system's seconds, and the game's
    /// ((tick + interpolant) x 0.1, GameTimeSource).
    void set_clocks(f64 system_seconds, f64 game_seconds) {
        system_time_ = system_seconds;
        game_time_ = game_seconds;
    }
    /// UseGameClock / UseSystemClock: the clock the moves run on.
    void use_game_clock(bool game) { game_clock_ = game; }
    /// TargetBox: the box's centre, zoomed to its wider x or z extent.
    void target_box(const std::array<f32, 3>& min, const std::array<f32, 3>& max, f32 seconds);
    /// TargetEntities: the first live one, at `zoom`; followed if tracked.
    void target_entities(std::vector<u32> ids, bool track, f32 zoom, f32 seconds);
    /// TargetNoseCam: behind the first live one's nose, at its heading and
    /// its pitch plus `pitch_adjust`.
    void target_nose_cam(std::vector<u32> ids, f32 pitch_adjust, f32 zoom, f32 seconds,
                         f32 transition);
    /// TargetNothing: a location, followed no more.
    void target_nothing();
    /// Spin (Lua): turn `heading_rate` revolutions a second, zoom
    /// `zoom_rate` a second.
    void spin_rates(f32 heading_rate, f32 zoom_rate);
    /// SetAccMode: Linear, FastInSlowOut or SlowInOut (any case); false for
    /// another name (unchanged).
    bool set_acc_mode(const std::string& name);
    void set_ease_in_out(bool on) { ease_in_out_ = on; }
    /// A move is under way.
    bool moving() const { return move_seconds_ > 0.0f; }
    /// The camera's event: signalled when a move ends (WaitFor).
    bool signaled() const { return signaled_; }
    /// Called once, when the event is next signalled.
    void on_signal(std::function<void()> waiter) { waiters_.push_back(std::move(waiter)); }
    CameraTarget target_type() const { return target_type_; }
    CameraAccel acc_mode() const { return accel_; }

    // --- The lanes ---
    f32 target_x() const { return target_[0]; }
    f32 target_y() const { return target_[1]; }
    f32 target_z() const { return target_[2]; }
    /// The focus: the target on the ground along the view (ClampFocusPos).
    f32 focus_x() const { return focus_[0]; }
    f32 focus_y() const { return focus_[1]; }
    f32 focus_z() const { return focus_[2]; }
    /// The target zoom: the world's extent across the view (Lua GetZoom).
    f32 zoom() const { return target_zoom_; }
    /// The zoom asked for, which the target zoom glides toward.
    f32 requested_zoom() const { return near_zoom_; }
    /// The eye's distance from the focus (CameraGetZoom).
    f32 eye_distance() const { return eye_distance_; }
    f32 heading() const { return heading_; }
    f32 pitch() const { return pitch_; }
    /// The horizontal field of view (radians).
    f32 fov() const { return fov_; }
    bool rotated() const { return rotated_; }
    f32 min_zoom() const { return kNearZoom; }
    /// GetMaxZoom: the playable rect's extent times the multiplier.
    f32 max_zoom() const;
    f32 viewport_width() const { return viewport_w_; }
    f32 viewport_height() const { return viewport_h_; }

    /// When false, update() ignores keyboard and mouse (scripted captures).
    void set_input_enabled(bool enabled) { input_enabled_ = enabled; }

    // The options' console variables (M217i), the k* above their defaults:
    // cam_ZoomAmount, ui_KeyboardPanSpeed, ui_KeyboardPanAccelerateMultiplier,
    // ui_KeyboardRotateSpeed, ui_KeyboardRotateAccelerateMultiplier,
    // ui_ScreenEdgeScrollView, ui_ArrowKeysScrollView.
    f32 zoom_amount() const { return zoom_amount_; }
    void set_zoom_amount(f32 v) { zoom_amount_ = v; }
    f32 keyboard_pan_speed() const { return keyboard_pan_speed_; }
    void set_keyboard_pan_speed(f32 v) { keyboard_pan_speed_ = v; }
    f32 keyboard_pan_accelerate() const { return keyboard_pan_accelerate_; }
    void set_keyboard_pan_accelerate(f32 v) { keyboard_pan_accelerate_ = v; }
    f32 keyboard_rotate_speed() const { return keyboard_rotate_speed_; }
    void set_keyboard_rotate_speed(f32 v) { keyboard_rotate_speed_ = v; }
    f32 keyboard_rotate_accelerate() const { return keyboard_rotate_accelerate_; }
    void set_keyboard_rotate_accelerate(f32 v) { keyboard_rotate_accelerate_ = v; }
    bool edge_scroll() const { return edge_scroll_; }
    void set_edge_scroll(bool on) { edge_scroll_ = on; }
    bool arrow_scroll() const { return arrow_scroll_; }
    void set_arrow_scroll(bool on) { arrow_scroll_ = on; }
    /// SetInvertMidMouseButton: the middle button drags the ground the other
    /// way (Moho negates its scrub deltas, UI_SetInvertMidMouseScrub).
    void set_invert_middle(bool on) { invert_middle_ = on; }
    bool invert_middle() const { return invert_middle_; }
    /// When false, the keys don't pan or spin: a UI control has the keyboard
    /// (Moho's MAUI_KeyIsDown is false while one has focus).
    void set_keys_enabled(bool enabled) { keys_enabled_ = enabled; }
    /// When false, the mouse's drags aren't the world view's: it is over a UI
    /// control.
    void set_mouse_enabled(bool enabled) { mouse_enabled_ = enabled; }
    /// Whether the mouse is the world view's now (the wheel's zoom).
    bool accepts_mouse() const { return input_enabled_ && mouse_enabled_; }

    // --- The view (UpdateCoords) ---
    void eye_position(f32& out_x, f32& out_y, f32& out_z) const;
    /// The view matrix (column-major): from the eye to the focus.
    std::array<f32, 16> view() const;
    /// D3D's FOV projection (horizontal FOV at aspect > 1), Vulkan's depth.
    std::array<f32, 16> projection(f32 aspect) const;
    std::array<f32, 16> view_proj(f32 aspect) const;
    /// The clip planes: max(0.01 d, 0.01) and d + 17000.
    f32 near_clip() const;
    f32 far_clip() const;
    /// tan of the vertical half field of view at an aspect.
    f32 tan_half_fov_y(f32 aspect) const;

    /// The ray from the eye through a screen pixel: `dir` has unit length.
    bool screen_ray(f32 screen_x, f32 screen_y, f32 window_w, f32 window_h, f32 origin[3],
                    f32 dir[3]) const;
    /// Unproject screen pixel to world XZ plane (y = ground_y).
    bool screen_to_world(f32 screen_x, f32 screen_y, f32 window_w, f32 window_h, f32 ground_y,
                         f32& out_x, f32& out_z) const;
    /// Where the pixel's ray first meets the ground: the terrain (Moho's
    /// heightfield intersection), or the water's surface where the ray meets
    /// that first. Off the map, or without a terrain, it meets the level of
    /// the camera's focus.
    bool pick_ground(f32 screen_x, f32 screen_y, f32 window_w, f32 window_h,
                     const map::Terrain* terrain, f32& out_x, f32& out_y, f32& out_z) const;

    /// Accumulate camera shake intensity (called from renderer per frame).
    void apply_shake(f32 intensity);

    /// The unit view direction, from the eye toward the focus.
    std::array<f32, 3> direction() const;

private:
    void decay_shake();
    f64 now() const { return game_clock_ ? game_time_ : system_time_; }
    void timed_move_init(f32 seconds, f32 transition);
    void setup_hermite();
    void update_targets(f32 dt);
    /// UpdateBasis: the zoom's glide, the clamps, the FOV, the heading and
    /// pitch by the target's type.
    void update_basis(f32 seconds);
    void interpolate_basis();
    void signal();
    /// The active target entity's pose (false: none, or gone).
    bool target_pose(CameraEntityPose& out) const;
    void target_next_entity();
    /// The log-zoom blend, 0 at the nearest zoom and 1 at the farthest.
    f32 zoom_fraction() const;
    f32 zoom_pitch() const;
    void clamp_target();
    void clamp_focus();
    /// Where a line first meets the terrain or water, walked from `from`.
    bool surface_hit(const f32 from[3], const f32 dir[3], f32 reach, f32 out[3]) const;

    /// The surface's height at (x, z): the ground, or the water over it.
    f32 surface_height(f32 x, f32 z) const;

    const map::Heightmap* ground_ = nullptr;
    bool has_water_ = false;
    f32 water_elevation_ = 0.0f;
    bool input_enabled_ = true;
    bool keys_enabled_ = true;
    bool mouse_enabled_ = true;
    f32 zoom_amount_ = kZoomAmount;
    f32 keyboard_pan_speed_ = kKeyboardPanSpeed;
    f32 keyboard_pan_accelerate_ = kKeyboardPanAccelerate;
    f32 keyboard_rotate_speed_ = kKeyboardRotateSpeed;
    f32 keyboard_rotate_accelerate_ = kKeyboardRotateAccelerate;
    bool edge_scroll_ = true;
    bool arrow_scroll_ = true;
    bool invert_middle_ = false;

    // CameraImpl's lanes
    std::array<f32, 3> target_{512.0f, 0.0f, 512.0f}; ///< mTargetLocation
    std::array<f32, 3> focus_{512.0f, 0.0f, 512.0f};  ///< mOffset
    f32 heading_ = kPi;
    f32 pitch_ = kFarPitchDeg * kPi / 180.0f; ///< mFarPitch
    f32 fov_ = kFarFovDeg * kPi / 180.0f;     ///< mFarFov
    f32 near_zoom_ = 1024.0f;                 ///< the zoom asked for
    f32 target_zoom_ = 1024.0f;
    f32 eye_distance_ = 0.0f; ///< mZoom
    bool rotated_ = false;
    bool revert_ = false;
    bool free_ = false; ///< cam_Free
    f32 pivot_x_ = 0.0f;
    f32 pivot_y_ = 0.0f;
    f32 max_zoom_mult_ = kDefaultMaxZoomMult;
    /// Until the renderer's (square: no window's shape)
    f32 viewport_w_ = 1024.0f;
    f32 viewport_h_ = 1024.0f;
    std::array<f32, 4> rect_{0.0f, 0.0f, 1024.0f, 1024.0f}; ///< x0, z0, x1, z1

    // The moves (M217g)
    CameraTarget target_type_ = CameraTarget::Location;
    CameraAccel accel_ = CameraAccel::Linear;
    bool ease_in_out_ = true; ///< mEnableEaseInOut
    bool game_clock_ = false;
    f64 system_time_ = 0.0;
    f64 game_time_ = 0.0;
    f64 last_game_time_ = 0.0;
    f32 move_seconds_ = 0.0f;    ///< mTimedMoveDuration
    f32 move_transition_ = 0.0f; ///< a nose move's own seconds
    f64 move_start_ = 0.0;
    std::array<f32, 3> move_focus_{};
    f32 move_zoom_ = 0.0f;
    f32 move_pitch_ = 0.0f;
    f32 move_heading_ = 0.0f;
    /// SetupHermite's deltas (its start and end tangents are the same)
    std::array<f32, 3> hermite_focus_{};
    f32 hermite_heading_ = 0.0f;
    f32 hermite_pitch_ = 0.0f;
    f32 hermite_zoom_ = 0.0f;
    f32 end_heading_ = kPi; ///< mHeadingZoom
    f32 end_pitch_ = 0.0f;  ///< mCurrentPitch
    f32 heading_rate_ = 0.0f;
    f32 zoom_rate_ = 0.0f;
    f32 nose_pitch_adjust_ = 0.0f;
    std::vector<u32> target_ids_;
    size_t active_target_ = 0;
    bool target_time_armed_ = false; ///< mTargetTime
    f32 target_time_left_ = 0.0f;
    bool signaled_ = true;
    std::vector<std::function<void()>> waiters_;
    EntityLookup entity_lookup_;

    // The world view's drags
    bool dragging_ = false;
    bool spinning_ = false;
    f32 last_mouse_x_ = 0.0f;
    f32 last_mouse_y_ = 0.0f;

    f32 shake_intensity_ = 0;
};

// Inline matrix math (no GLM dependency)
namespace math {

std::array<f32, 16> look_at(f32 ex, f32 ey, f32 ez,
                            f32 tx, f32 ty, f32 tz,
                            f32 ux, f32 uy, f32 uz);

/// Perspective projection with Vulkan [0,1] depth and Y-flip.
std::array<f32, 16> perspective(f32 fov_rad, f32 aspect, f32 near, f32 far);

/// Orthographic projection with Vulkan [0,1] depth, no Y-flip (for shadow maps).
std::array<f32, 16> ortho(f32 left, f32 right, f32 bottom, f32 top, f32 near, f32 far);

/// Multiply two 4x4 column-major matrices: result = a * b.
std::array<f32, 16> mat4_mul(const std::array<f32, 16>& a,
                             const std::array<f32, 16>& b);

} // namespace math

} // namespace osc::renderer
