#pragma once

#include "core/types.hpp"

namespace osc::sim {

/// Resource deposit (mass/hydrocarbon point on map).
struct ResourceDeposit {
    f32 x = 0, y = 0, z = 0;
    f32 size = 1.0f;
    enum Type : u8 { Mass = 0, Hydrocarbon = 1 } type = Mass;
};

} // namespace osc::sim
