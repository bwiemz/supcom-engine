#pragma once

#include "sim/entity.hpp"

#include <string>

namespace osc::sim {

class Shield : public Entity {
public:
    bool is_shield() const override { return true; }

    u32 owner_id = 0;           // entity ID of owning unit
    bool is_on = false;          // moho TurnOn/TurnOff/IsOn state
    f32 size = 10.0f;            // shield radius (from spec.Size)
    std::string shield_type;     // "Bubble", "Personal", etc.
};

} // namespace osc::sim
