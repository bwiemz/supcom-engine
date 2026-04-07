#pragma once

#include "core/types.hpp"
#include <array>

namespace osc::renderer {

/// 8 army colors (RGB float, 0-1 range).
/// Index 0-7 corresponds to ARMY_1 through ARMY_8.
inline constexpr std::array<std::array<f32, 3>, 8> ARMY_COLORS = {{
    {0.2f, 0.4f, 1.0f},  // ARMY_1: Blue
    {1.0f, 0.2f, 0.2f},  // ARMY_2: Red
    {0.2f, 0.8f, 0.2f},  // ARMY_3: Green
    {1.0f, 1.0f, 0.2f},  // ARMY_4: Yellow
    {1.0f, 0.5f, 0.1f},  // ARMY_5: Orange
    {0.7f, 0.2f, 0.9f},  // ARMY_6: Purple
    {0.2f, 0.9f, 0.9f},  // ARMY_7: Cyan
    {0.9f, 0.9f, 0.9f},  // ARMY_8: White
}};

/// Look up army color by 0-based index.  Returns white for out-of-range.
inline void get_army_color(i32 army, f32& r, f32& g, f32& b) {
    if (army >= 0 && army < 8) {
        r = ARMY_COLORS[army][0];
        g = ARMY_COLORS[army][1];
        b = ARMY_COLORS[army][2];
    } else {
        r = g = b = 1.0f;
    }
}

} // namespace osc::renderer
