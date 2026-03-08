#pragma once

#include "core/types.hpp"

#include <array>

struct GLFWwindow;

namespace osc::renderer {

/// RTS-style orbit camera with WASD pan, scroll zoom, middle-mouse orbit.
/// Produces a combined view-projection matrix as push constant data.
class Camera {
public:
    /// Initialize camera centered on map.
    void init(f32 map_width, f32 map_height);

    /// Process input from GLFW window.
    void update(GLFWwindow* window, f64 dt);

    /// Combined view * projection matrix (column-major, Vulkan conventions).
    std::array<f32, 16> view_proj(f32 aspect) const;

    f32 target_x() const { return target_x_; }
    f32 target_z() const { return target_z_; }
    f32 distance() const { return distance_; }

    /// Compute camera eye position from spherical coordinates.
    void eye_position(f32& out_x, f32& out_y, f32& out_z) const;
    void set_distance(f32 d) { distance_ = d; }
    void set_target(f32 x, f32 z) { target_x_ = x; target_z_ = z; }

    /// Unproject screen pixel to world XZ plane (y = ground_y).
    /// Returns true if intersection found, writes world x/z.
    bool screen_to_world(f32 screen_x, f32 screen_y,
                         f32 window_w, f32 window_h,
                         f32 ground_y,
                         f32& out_x, f32& out_z) const;

    f32 yaw() const { return yaw_; }
    f32 pitch() const { return pitch_; }

    /// Accumulate camera shake intensity (called from renderer per frame).
    void apply_shake(f32 intensity);

private:
    // Look-at target on XZ ground plane
    f32 target_x_ = 0;
    f32 target_z_ = 0;

    // Spherical coords relative to target
    f32 distance_ = 300.0f;
    f32 yaw_ = 0.0f;        // radians, 0 = looking along +Z
    f32 pitch_ = 0.87f;     // radians (~50 degrees), clamped 30-80 deg

    // Map bounds for clamping
    f32 map_w_ = 1024;
    f32 map_h_ = 1024;

    // Mouse state for orbit
    bool orbiting_ = false;
    f64 last_mouse_x_ = 0;
    f64 last_mouse_y_ = 0;

    // Camera shake
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
