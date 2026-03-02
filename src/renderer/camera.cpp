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

    // WASD pan
    if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS)
        target_z_ -= pan_speed * std::cos(yaw_),
        target_x_ -= pan_speed * std::sin(yaw_);
    if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS)
        target_z_ += pan_speed * std::cos(yaw_),
        target_x_ += pan_speed * std::sin(yaw_);
    if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS)
        target_x_ -= pan_speed * std::cos(yaw_),
        target_z_ += pan_speed * std::sin(yaw_);
    if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS)
        target_x_ += pan_speed * std::cos(yaw_),
        target_z_ -= pan_speed * std::sin(yaw_);

    // Clamp target to map bounds
    target_x_ = std::clamp(target_x_, 0.0f, map_w_);
    target_z_ = std::clamp(target_z_, 0.0f, map_h_);

    // Middle mouse orbit
    f64 mx, my;
    glfwGetCursorPos(window, &mx, &my);

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
}

std::array<f32, 16> Camera::view_proj(f32 aspect) const {
    // Eye position from spherical coordinates
    f32 eye_x = target_x_ + distance_ * std::sin(yaw_) * std::cos(pitch_);
    f32 eye_y = distance_ * std::sin(pitch_);
    f32 eye_z = target_z_ + distance_ * std::cos(yaw_) * std::cos(pitch_);

    auto view = math::look_at(eye_x, eye_y, eye_z,
                              target_x_, 0.0f, target_z_,
                              0.0f, 1.0f, 0.0f);
    auto proj = math::perspective(0.785f, aspect, 1.0f, 5000.0f); // 45 deg FOV

    return math::mat4_mul(proj, view);
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

} // namespace osc::renderer
