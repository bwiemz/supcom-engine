#include "renderer/scene_views.hpp"

namespace osc::renderer {

Camera* CameraSet::find(std::string_view name) {
    if (name == kWorldCamera) {
        return &world_;
    }
    const auto it = others_.find(name);
    return it == others_.end() ? nullptr : it->second.get();
}

Camera* CameraSet::find_tag(const void* tag) {
    if (tag == &world_) {
        return &world_;
    }
    for (auto& [name, camera] : others_) {
        if (tag == camera.get()) {
            return camera.get();
        }
    }
    return nullptr;
}

Camera& CameraSet::add(const std::string& name, bool& created) {
    created = false;
    if (Camera* camera = find(name)) {
        return *camera;
    }
    created = true;
    return *others_.emplace(name, std::make_unique<Camera>()).first->second;
}

const SceneView& view_under(const std::vector<SceneView>& views, f32 sx, f32 sy) {
    for (auto it = views.rbegin(); it != views.rend(); ++it) {
        if (it->contains(sx, sy)) {
            return *it;
        }
    }
    return views.front();
}

bool pick_ground_in(const std::vector<SceneView>& views, f32 sx, f32 sy,
                    const map::Terrain* terrain, f32& out_x, f32& out_y, f32& out_z) {
    const SceneView& view = view_under(views, sx, sy);
    return view.camera->pick_ground(sx - view.x, sy - view.y, view.w, view.h, terrain, out_x, out_y,
                                    out_z);
}

} // namespace osc::renderer
