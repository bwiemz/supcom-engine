#pragma once

#include "sim/entity.hpp"

namespace osc::sim {

class Prop : public Entity {
public:
    bool is_prop() const override { return true; }
};

} // namespace osc::sim
