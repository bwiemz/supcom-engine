#pragma once

#include "core/types.hpp"
#include "renderer/camera.hpp"

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace osc::map {
class Terrain;
}

namespace osc::ui {
class WorldView;
}

namespace osc::renderer {

/// The cameras by name (Moho's RCamManager). The WorldCamera is the
/// renderer's own; a world view makes the others.
class CameraSet {
public:
    static constexpr std::string_view kWorldCamera = "WorldCamera";

    explicit CameraSet(Camera& world) : world_(world) {}

    Camera* find(std::string_view name);
    Camera* find_tag(const void* tag);
    Camera& world() { return world_; }
    Camera& add(const std::string& name, bool& created);

    template <class F> void for_each_other(F&& f) {
        for (auto& [name, camera] : others_) {
            f(*camera);
        }
    }

private:
    Camera& world_;
    std::map<std::string, std::unique_ptr<Camera>, std::less<>> others_;
};

struct SceneView {
    Camera* camera = nullptr;
    f32 x = 0.0f;
    f32 y = 0.0f;
    f32 w = 0.0f;
    f32 h = 0.0f;
    bool global_camera_commands = false;
    bool resources = true;
    const ui::WorldView* world_view = nullptr;

    bool contains(f32 sx, f32 sy) const { return sx >= x && sy >= y && sx < x + w && sy < y + h; }
};

inline SceneView in_points(SceneView v, f32 scale) {
    const f32 s = scale > 0.0f ? scale : 1.0f;
    v.x /= s;
    v.y /= s;
    v.w /= s;
    v.h /= s;
    return v;
}

std::vector<SceneView> placed_views(const std::vector<SceneView>& laid_out, f32 scale, f32 width,
                                    f32 height, size_t most);

const SceneView& view_under(const std::vector<SceneView>& views, f32 sx, f32 sy);

bool pick_ground_in(const std::vector<SceneView>& views, f32 sx, f32 sy,
                    const map::Terrain* terrain, f32& out_x, f32& out_y, f32& out_z);

} // namespace osc::renderer
