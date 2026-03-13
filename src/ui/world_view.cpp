#include "ui/world_view.hpp"

#include "map/terrain.hpp"
#include "renderer/camera.hpp"

namespace osc::ui {

WorldView::WorldView() = default;

void WorldView::register_camera(const std::string& name,
                                renderer::Camera* cam) {
    camera_name_ = name;
    camera_ = cam;
}

bool WorldView::project(f32 wx, f32 wy, f32 wz, f32& sx, f32& sy) const {
    if (!camera_ || viewport_w_ == 0 || viewport_h_ == 0) return false;

    f32 aspect = static_cast<f32>(viewport_w_) / static_cast<f32>(viewport_h_);
    auto vp = camera_->view_proj(aspect);

    // Transform world position by view-projection matrix (column-major)
    f32 cx = vp[0] * wx + vp[4] * wy + vp[8] * wz + vp[12];
    f32 cy = vp[1] * wx + vp[5] * wy + vp[9] * wz + vp[13];
    f32 cw = vp[3] * wx + vp[7] * wy + vp[11] * wz + vp[15];

    // Behind camera check
    if (cw <= 0.0f) return false;

    // Perspective divide -> NDC
    f32 ndcx = cx / cw;
    f32 ndcy = cy / cw;

    // NDC to screen (Vulkan: Y is flipped in projection, so ndcy is already
    // in conventional screen orientation after the Y-flip in perspective())
    sx = (ndcx * 0.5f + 0.5f) * static_cast<f32>(viewport_w_);
    sy = (ndcy * 0.5f + 0.5f) * static_cast<f32>(viewport_h_);

    return true;
}

bool WorldView::get_mouse_world_pos(f32 sx, f32 sy,
                                     f32& wx, f32& wy, f32& wz) const {
    if (!camera_ || viewport_w_ == 0 || viewport_h_ == 0) return false;

    f32 w = static_cast<f32>(viewport_w_);
    f32 h = static_cast<f32>(viewport_h_);

    // First pass: intersect with y=0 ground plane (or water elevation)
    f32 ground_y = 0.0f;
    if (terrain_) {
        ground_y = terrain_->water_elevation();
    }

    f32 flat_x = 0, flat_z = 0;
    if (!camera_->screen_to_world(sx, sy, w, h, ground_y, flat_x, flat_z)) {
        return false;
    }

    // Refine with terrain height if available (iterative refinement)
    if (terrain_) {
        for (int i = 0; i < 3; ++i) {
            f32 th = terrain_->get_terrain_height(flat_x, flat_z);
            if (!camera_->screen_to_world(sx, sy, w, h, th, flat_x, flat_z)) {
                break;
            }
        }
        wx = flat_x;
        wy = terrain_->get_terrain_height(flat_x, flat_z);
        wz = flat_z;
    } else {
        wx = flat_x;
        wy = ground_y;
        wz = flat_z;
    }

    return true;
}

void WorldView::zoom_scale(f32 /*x*/, f32 /*y*/, f32 /*rotation*/, f32 delta) {
    if (!camera_) return;

    // Scale zoom speed by current distance for smooth feel
    constexpr f32 ZOOM_SPEED = 0.1f;
    f32 new_dist = camera_->distance() - delta * ZOOM_SPEED * camera_->distance();

    // Clamp to reasonable range
    constexpr f32 MIN_DIST = 10.0f;
    constexpr f32 MAX_DIST = 1000.0f;
    if (new_dist < MIN_DIST) new_dist = MIN_DIST;
    if (new_dist > MAX_DIST) new_dist = MAX_DIST;

    camera_->set_distance(new_dist);
}

} // namespace osc::ui
