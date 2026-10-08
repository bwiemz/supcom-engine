#pragma once

#include "core/types.hpp"
#include "sim/entity.hpp" // Vector3

#include <array>
#include <string>
#include <vector>

namespace osc::ui {
class UIControlRegistry;
}

namespace osc::renderer {

/// What the UI asks AddCommandFeedbackBlip(meshInfo, duration) for (faf-re
/// UiRuntimeTypes.cpp, cfunc_AddCommandFeedbackBlipL 0x00857BE0): a mesh at
/// a point, unrotated, for `duration` seconds. FA's commandmode.lua marks
/// each order it issues so (a flag and a crosshair, or the order's own
/// mesh).
struct FeedbackBlipSpec {
    sim::Vector3 position;
    std::string mesh_name;    ///< MeshName: an SCM file; it wins over BlueprintID
    std::string blueprint_id; ///< BlueprintID: the unit's LOD 0 mesh, at its UniformScale
    std::string texture_name; ///< TextureName: the albedo, either way
    std::string shader_name;  ///< ShaderName: mesh.fx's technique (CommandFeedback...)
    f32 uniform_scale = 0.0f; ///< UniformScale, for a MeshName (Moho reads it unguarded)
    f32 duration = 0.0f;      ///< seconds
};

struct FeedbackBlip {
    FeedbackBlipSpec spec;
    /// The game tick it was made at, as mesh instances keep it (mod 36000;
    /// the shaders' material.x).
    f32 created_tick = 0.0f;
    /// Real seconds since: it goes once its duration has passed, as Moho's
    /// UpdateCommandFeedbackBlips counts the UI's frames, not the game's
    /// ticks (paused, its animation stops but it still goes).
    f32 age = 0.0f;
};

/// The blips shown now (Moho keeps them in one list, in the UI).
class CommandFeedbackBlips {
public:
    void add(FeedbackBlipSpec spec, f32 created_tick);
    /// A UI frame `dt` seconds long: those whose duration has passed go.
    void update(f32 dt);
    const std::vector<FeedbackBlip>& blips() const { return blips_; }
    void clear() { blips_.clear(); }

private:
    std::vector<FeedbackBlip> blips_;
};

struct WorldMeshDraw {
    FeedbackBlipSpec spec;
    f32 created_tick = 0.0f;
    f32 lifetime = 0.0f;
    f32 lod_cutoff = 0.0f;
};

std::vector<WorldMeshDraw> shown_world_meshes(const ui::UIControlRegistry& registry);

/// mesh.fx's LOD metric at `p` (lodBasis, GeomCamera3's viewport row 1):
/// the view's width at p's depth along `forward` from `eye`, for a
/// horizontal field of view `fov` (radians), at the camera's LOD scale 1.
/// At the camera's focus it is the zoom.
f32 lod_metric(const sim::Vector3& p, const sim::Vector3& eye, const sim::Vector3& forward,
               f32 fov);

/// CommandFeedbackVS's scale for that metric: larger as the camera pulls
/// back, lerp(1, 15, (lod - 10.5) * 0.001), unclamped.
inline f32 feedback_distance_scale(f32 lod) {
    return 1.0f + 14.0f * ((lod - 10.5f) * 0.001f);
}

/// A blip's single LOD shows to a metric of 1000 (Mesh's default cutoff).
constexpr f32 kFeedbackLodCutoff = 1000.0f;

/// Its mesh instance's model matrix, column-major: at its position,
/// unrotated, `scale` its mesh's size.
std::array<f32, 16> feedback_model(const sim::Vector3& position, f32 scale);

} // namespace osc::renderer
