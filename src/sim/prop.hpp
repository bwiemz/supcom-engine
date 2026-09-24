#pragma once

#include "sim/entity.hpp"

namespace osc::sim {

class Prop : public Entity {
public:
    bool is_prop() const override { return true; }

    /// SinkAway: how fast the prop sinks into the ground (units/s, <= 0).
    f32 sink_rate = 0;
};

} // namespace osc::sim
