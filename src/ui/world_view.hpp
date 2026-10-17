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
    /// Made a minimap (UIWorldView's isMiniMap): its camera takes the LOD
    /// scale cam_DefaultMiniLOD holds now, as CUIWorldView's does when made;
    /// setting the convar later leaves it.
    void set_minimap(bool m) {
        is_minimap_ = m;
        lod_scale_ = m ? default_mini_lod() : kDefaultLod;
    }
    /// Its camera's LOD scale (GeomCamera3's lodScale): the LOD metric is the
    /// view's width times it, so a larger one shows less detail.
    f32 lod_scale() const { return lod_scale_; }

    /// Moho's cam_DefaultMiniLOD (1.8): the LOD scale a minimap view's camera
    /// is made with. FAF's patched game draws no minimap meshes while it is
    /// 0 (FA-Binary-Patches MinimapMesh.cpp).
    static f32 default_mini_lod();
    static void set_default_mini_lod(f32 scale);
    /// cam_DefaultLOD's, any other view's (1).
    static constexpr f32 kDefaultLod = 1.0f;
    static constexpr f32 kDefaultMiniLod = 1.8f;

    bool resource_rendering() const { return resource_rendering_; }
    void set_resource_rendering(bool on) { resource_rendering_ = on; }

    /// FAF's patched game's SetCustomRender: each frame the view's Lua
    /// OnRenderWorld(self, delta) draws shapes in it (UI_DrawLine, ...).
    bool custom_render() const { return custom_render_; }
    void set_custom_render(bool on) { custom_render_ = on; }

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
    f32 lod_scale_ = kDefaultLod;
    bool resource_rendering_ = true;
    bool custom_render_ = false;
};

} // namespace osc::ui
