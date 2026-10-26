#include "sim/debug_canvas.hpp"

#include <cmath>

namespace osc::sim {

void DebugCanvas::circle(const Vector3& centre, f32 radius, u32 color, u32 segments) {
    if (segments == 0) return;
    constexpr f32 kTwoPi = 6.2831855f;
    Vector3 prev{centre.x + radius, centre.y, centre.z};
    for (u32 i = 1; i <= segments; ++i) {
        const f32 angle = static_cast<f32>(i) * kTwoPi / static_cast<f32>(segments);
        const Vector3 next{centre.x + radius * std::cos(angle), centre.y,
                           centre.z - radius * std::sin(angle)};
        line(prev, next, color);
        prev = next;
    }
}

void DebugCanvas::line_pop(const Vector3& a, const Vector3& b, u32 color) {
    Vector3 back{a.x - b.x, a.y - b.y, a.z - b.z};
    const f32 length2 = back.x * back.x + back.y * back.y + back.z * back.z;
    if (length2 > 0.0f) {
        const f32 scale = 2.0f / std::sqrt(length2);
        back = {back.x * scale, back.y * scale, back.z * scale};
    } else {
        back = {};
    }
    line(a, b, color);
    circle({b.x + back.x, b.y + back.y, b.z + back.z}, 1.0f, color);
}

} // namespace osc::sim
