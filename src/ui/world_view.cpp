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

    // Where the cursor's ray meets the ground (or the water over it).
    return camera_->pick_ground(sx, sy, w, h, terrain_, wx, wy, wz);
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
