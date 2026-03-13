#pragma once

#include "ui/ui_control.hpp"

#include <string>

namespace osc::renderer {
class Camera;
class Renderer;
} // namespace osc::renderer

namespace osc::map {
class Terrain;
} // namespace osc::map

namespace osc::ui {

/// WorldView is the UIControl subclass that represents the 3D game world
/// inside FA's UI tree. It holds a camera reference, terrain pointer, and
/// provides world/screen projection and mouse raycasting.
class WorldView : public UIControl {
public:
    explicit WorldView();

    // --- Camera ---
    void register_camera(const std::string& name, renderer::Camera* cam);
    renderer::Camera* camera() const { return camera_; }
    const std::string& camera_name() const { return camera_name_; }

    // --- Terrain ---
    void set_terrain(const map::Terrain* t) { terrain_ = t; }
    const map::Terrain* terrain() const { return terrain_; }

    // --- Renderer ---
    void set_renderer(renderer::Renderer* r) { renderer_ = r; }
    renderer::Renderer* renderer() const { return renderer_; }

    // --- Viewport ---
    void set_viewport(u32 w, u32 h) { viewport_w_ = w; viewport_h_ = h; }
    u32 viewport_width() const { return viewport_w_; }
    u32 viewport_height() const { return viewport_h_; }

    // --- Projection ---
    /// Project world position to screen coordinates.
    /// Returns true if the point is in front of the camera.
    bool project(f32 wx, f32 wy, f32 wz, f32& sx, f32& sy) const;

    /// Unproject screen position to world coordinates using terrain height.
    bool get_mouse_world_pos(f32 sx, f32 sy, f32& wx, f32& wy, f32& wz) const;

    // --- Camera control ---
    void zoom_scale(f32 x, f32 y, f32 rotation, f32 delta);

    // --- Flags ---
    bool is_cartographic() const { return cartographic_; }
    void set_cartographic(bool c) { cartographic_ = c; }

    bool is_input_locked() const { return input_locked_; }
    void set_input_locked(bool l) { input_locked_ = l; }

    bool highlight_enabled() const { return highlight_enabled_; }
    void set_highlight_enabled(bool e) { highlight_enabled_ = e; }

    bool gets_global_camera_commands() const { return global_cam_cmds_; }
    void set_global_camera_commands(bool g) { global_cam_cmds_ = g; }

    bool is_minimap() const { return is_minimap_; }
    void set_minimap(bool m) { is_minimap_ = m; }

private:
    renderer::Camera* camera_ = nullptr;
    const map::Terrain* terrain_ = nullptr;
    renderer::Renderer* renderer_ = nullptr;
    std::string camera_name_;
    u32 viewport_w_ = 0;
    u32 viewport_h_ = 0;
    bool cartographic_ = false;
    bool input_locked_ = false;
    bool highlight_enabled_ = true;
    bool global_cam_cmds_ = false;
    bool is_minimap_ = false;
};

} // namespace osc::ui
