#pragma once

#include "sim/entity.hpp"

#include <array>
#include <vector>

namespace osc::sim {

class Prop : public Entity {
public:
    bool is_prop() const override { return true; }

    /// SinkAway: how fast the prop sinks into the ground (units/s, <= 0).
    f32 sink_rate = 0;

    /// TryCopyPose's copy of a unit's skinning matrices: a wreck keeps the
    /// pose its unit died in. Empty draws the mesh at rest.
    std::vector<std::array<f32, 16>> pose;
};

} // namespace osc::sim
