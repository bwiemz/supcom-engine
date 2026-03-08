#include "renderer/camera.hpp"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>

namespace osc::renderer {

void Camera::init(f32 map_width, f32 map_height) {
    map_w_ = map_width;
    map_h_ = map_height;
    target_x_ = map_width * 0.5f;
    target_z_ = map_height * 0.5f;
    distance_ = std::max(map_width, map_height) * 0.4f;
}

void Camera::update(GLFWwindow* window, f64 dt) {
    auto fdt = static_cast<f32>(dt);

    // Pan speed scales with zoom distance
    f32 pan_speed = distance_ * 0.8f * fdt;

    // WASD + arrow key pan
    bool up    = glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS ||
                 glfwGetKey(window, GLFW_KEY_UP) == GLFW_PRESS;
    bool down  = glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS ||
                 glfwGetKey(window, GLFW_KEY_DOWN) == GLFW_PRESS;
    bool left  = glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS ||
                 glfwGetKey(window, GLFW_KEY_LEFT) == GLFW_PRESS;
    bool right = glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS ||
                 glfwGetKey(window, GLFW_KEY_RIGHT) == GLFW_PRESS;

    // Mouse-edge scroll (cursor within EDGE_MARGIN pixels of window border)
    int win_w, win_h;
    glfwGetWindowSize(window, &win_w, &win_h);
    f64 mx, my;
    glfwGetCursorPos(window, &mx, &my);
    if (glfwGetWindowAttrib(window, GLFW_FOCUSED)) {
        constexpr f64 EDGE_MARGIN = 3.0;
        if (mx <= EDGE_MARGIN)             left  = true;
        if (mx >= win_w - EDGE_MARGIN - 1) right = true;
        if (my <= EDGE_MARGIN)             up    = true;
        if (my >= win_h - EDGE_MARGIN - 1) down  = true;
    }

    if (up)
        target_z_ -= pan_speed * std::cos(yaw_),
        target_x_ -= pan_speed * std::sin(yaw_);
    if (down)
        target_z_ += pan_speed * std::cos(yaw_),
        target_x_ += pan_speed * std::sin(yaw_);
    if (left)
        target_x_ -= pan_speed * std::cos(yaw_),
        target_z_ += pan_speed * std::sin(yaw_);
    if (right)
        target_x_ += pan_speed * std::cos(yaw_),
        target_z_ -= pan_speed * std::sin(yaw_);

    // Clamp target to map bounds
    target_x_ = std::clamp(target_x_, 0.0f, map_w_);
    target_z_ = std::clamp(target_z_, 0.0f, map_h_);

    // Middle mouse orbit (reuse mx/my from edge-scroll above)

    if (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS) {
        if (!orbiting_) {
            orbiting_ = true;
            last_mouse_x_ = mx;
            last_mouse_y_ = my;
        } else {
            f64 dx = mx - last_mouse_x_;
            f64 dy = my - last_mouse_y_;
            yaw_ += static_cast<f32>(dx) * 0.005f;
            pitch_ -= static_cast<f32>(dy) * 0.005f;
            pitch_ = std::clamp(pitch_, 0.52f, 1.40f); // ~30-80 degrees
            last_mouse_x_ = mx;
            last_mouse_y_ = my;
        }
    } else {
        orbiting_ = false;
    }

    // Decay camera shake
    if (shake_intensity_ > 0.01f)
        shake_intensity_ *= 0.9f;
    else
        shake_intensity_ = 0;
}

void Camera::apply_shake(f32 intensity) {
    shake_intensity_ = std::max(shake_intensity_, intensity);
}

std::array<f32, 16> Camera::view_proj(f32 aspect) const {
    // Apply shake offset to target
    f32 tx = target_x_;
    f32 tz = target_z_;
    if (shake_intensity_ > 0.01f) {
        // Simple deterministic pseudo-random offset from intensity
        // (varies each frame because intensity decays)
        f32 phase = shake_intensity_ * 137.5f; // irrational-ish multiplier
        tx += std::sin(phase * 3.7f) * shake_intensity_;
        tz += std::cos(phase * 2.3f) * shake_intensity_;
    }

    // Eye position from spherical coordinates
    f32 eye_x = tx + distance_ * std::sin(yaw_) * std::cos(pitch_);
    f32 eye_y = distance_ * std::sin(pitch_);
    f32 eye_z = tz + distance_ * std::cos(yaw_) * std::cos(pitch_);

    auto view = math::look_at(eye_x, eye_y, eye_z,
                              tx, 0.0f, tz,
                              0.0f, 1.0f, 0.0f);
    auto proj = math::perspective(0.785f, aspect, 1.0f, 5000.0f); // 45 deg FOV

    return math::mat4_mul(proj, view);
}

void Camera::eye_position(f32& x, f32& y, f32& z) const {
    // Use raw target (no shake) for eye position used by culling/specular
    x = target_x_ + distance_ * std::sin(yaw_) * std::cos(pitch_);
    y = distance_ * std::sin(pitch_);
    z = target_z_ + distance_ * std::cos(yaw_) * std::cos(pitch_);
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

bool Camera::screen_to_world(f32 screen_x, f32 screen_y,
                              f32 window_w, f32 window_h,
                              f32 ground_y,
                              f32& out_x, f32& out_z) const {
    // Convert screen pixel to NDC [-1, 1]
    f32 ndc_x = (2.0f * screen_x / window_w) - 1.0f;
    f32 ndc_y = (2.0f * screen_y / window_h) - 1.0f;
    // Vulkan Y-flip: NDC y is flipped in our projection
    // Our perspective already flips Y, so ndc_y maps correctly

    f32 aspect = window_w / window_h;

    // Reconstruct view and projection separately
    f32 ex, ey, ez;
    eye_position(ex, ey, ez);

    auto view = math::look_at(ex, ey, ez,
                               target_x_, 0.0f, target_z_,
                               0.0f, 1.0f, 0.0f);
    auto proj = math::perspective(0.785f, aspect, 1.0f, 5000.0f);

    // We need to invert VP to go from NDC to world.
    // Instead, construct ray directly from camera parameters:
    // Extract right/up/forward from view matrix (column-major, transposed rotation)
    f32 rx = view[0], ry = view[4], rz = view[8];   // right
    f32 ux = view[1], uy = view[5], uz = view[9];   // up
    f32 fx = -view[2], fy = -view[6], fz = -view[10]; // forward (negated -Z)

    // Half-angles from perspective
    f32 fov = 0.785f; // 45 deg
    f32 tan_half = std::tan(fov * 0.5f);

    // Direction in world space
    f32 dx = fx + ndc_x * aspect * tan_half * rx + ndc_y * tan_half * ux;
    f32 dy = fy + ndc_x * aspect * tan_half * ry + ndc_y * tan_half * uy;
    f32 dz = fz + ndc_x * aspect * tan_half * rz + ndc_y * tan_half * uz;

    // Normalize direction
    f32 len = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (len < 1e-6f) return false;
    dx /= len; dy /= len; dz /= len;

    // Intersect ray (eye + t*dir) with y = ground_y plane
    if (std::abs(dy) < 1e-6f) return false; // ray parallel to ground
    f32 t = (ground_y - ey) / dy;
    if (t < 0) return false; // intersection behind camera

    out_x = ex + t * dx;
    out_z = ez + t * dz;
    return true;
}

} // namespace osc::renderer
