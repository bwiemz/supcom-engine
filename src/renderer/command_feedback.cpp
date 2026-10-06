#include "renderer/command_feedback.hpp"

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
