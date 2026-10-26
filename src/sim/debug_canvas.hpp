#pragma once

#include "core/types.hpp"
#include "sim/entity.hpp" // Vector3

#include <utility>
#include <vector>

namespace osc::sim {

/// A line of the sim's debug drawing, in a packed ARGB colour (SCR_DecodeColor's).
struct DebugLine {
    Vector3 a;
    Vector3 b;
    u32 color = 0;
};

/// Moho's CDebugCanvas, as the sim's scripts draw into it (DrawLine, DrawLinePop,
/// DrawCircle): lines, and circles made of them.
class DebugCanvas {
public:
    /// AddLine (0x0044FA70)
    void line(const Vector3& a, const Vector3& b, u32 color) { lines_.push_back({a, b, color}); }
    /// AddWireCircle (0x00450030) about +Y: `segments` lines round `centre`, level, from its +x
    /// side, as AddWireOval (0x0044FED0) steps them with +Z turned onto +Y (its axes x and -z).
    void circle(const Vector3& centre, f32 radius, u32 color, u32 segments = 8);
    /// DrawLinePop (0x0075DE20): the line, and a ring of radius 1 two units back from `b`
    /// toward `a`.
    void line_pop(const Vector3& a, const Vector3& b, u32 color);

    const std::vector<DebugLine>& lines() const { return lines_; }
    std::vector<DebugLine> take() { return std::exchange(lines_, {}); }

private:
    std::vector<DebugLine> lines_;
};

} // namespace osc::sim
