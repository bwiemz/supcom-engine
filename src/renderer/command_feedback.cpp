#include "renderer/command_feedback.hpp"

#include "ui/ui_control.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace osc::renderer {

void CommandFeedbackBlips::add(FeedbackBlipSpec spec, f32 created_tick) {
    FeedbackBlip blip;
    blip.spec = std::move(spec);
    blip.created_tick = created_tick;
    blips_.push_back(std::move(blip));
}

void CommandFeedbackBlips::update(f32 dt) {
    for (FeedbackBlip& b : blips_) b.age += dt;
    blips_.erase(std::remove_if(blips_.begin(), blips_.end(),
                                [](const FeedbackBlip& b) { return b.age >= b.spec.duration; }),
                 blips_.end());
}

std::vector<WorldMeshDraw> shown_world_meshes(const ui::UIControlRegistry& registry) {
    std::vector<WorldMeshDraw> out;
    for (const auto& control : registry.all()) {
        if (!control || control->destroyed() || !control->world_mesh() ||
            control->world_mesh()->hidden) {
            continue;
        }
        const ui::UIControl::WorldMesh& mesh = *control->world_mesh();
        WorldMeshDraw draw;
        draw.spec.position = {mesh.position[0], mesh.position[1], mesh.position[2]};
        draw.spec.mesh_name = mesh.mesh_name;
        draw.spec.blueprint_id = mesh.blueprint_id;
        draw.spec.texture_name = mesh.texture_name;
        draw.spec.shader_name = mesh.shader_name;
        draw.spec.uniform_scale = mesh.uniform_scale;
        draw.created_tick = mesh.created_tick;
        draw.lifetime = mesh.lifetime;
        draw.lod_cutoff = mesh.lod_cutoff;
        out.push_back(std::move(draw));
    }
    return out;
}

f32 lod_metric(const sim::Vector3& p, const sim::Vector3& eye, const sim::Vector3& forward,
               f32 fov) {
    const f32 depth =
        (p.x - eye.x) * forward.x + (p.y - eye.y) * forward.y + (p.z - eye.z) * forward.z;
    return 2.0f * std::tan(fov * 0.5f) * depth;
}

std::array<f32, 16> feedback_model(const sim::Vector3& position, f32 scale) {
    std::array<f32, 16> m{};
    m[0] = scale;
    m[5] = scale;
    m[10] = scale;
    m[12] = position.x;
    m[13] = position.y;
    m[14] = position.z;
    m[15] = 1.0f;
    return m;
}

} // namespace osc::renderer
