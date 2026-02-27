#pragma once

#include "core/types.hpp"

#include <array>
#include <vector>

namespace osc::map {

/// Bit flags for per-army per-cell visibility state.
enum class VisFlag : u8 {
    None     = 0,
    Vision   = 1 << 0, // Direct line-of-sight
    Radar    = 1 << 1,
    Sonar    = 1 << 2,
    Omni     = 1 << 3,
    EverSeen = 1 << 4, // Sticky — never cleared once set
};

inline VisFlag operator|(VisFlag a, VisFlag b) {
    return static_cast<VisFlag>(static_cast<u8>(a) | static_cast<u8>(b));
}
inline VisFlag operator&(VisFlag a, VisFlag b) {
    return static_cast<VisFlag>(static_cast<u8>(a) & static_cast<u8>(b));
}
inline VisFlag& operator|=(VisFlag& a, VisFlag b) {
    a = a | b;
    return a;
}
inline bool has_flag(VisFlag flags, VisFlag test) {
    return (static_cast<u8>(flags) & static_cast<u8>(test)) != 0;
}

/// Per-army visibility grid.
/// Tracks Vision/Radar/Sonar/Omni/EverSeen per cell per army.
/// Pure data structure — no sim dependencies.
class VisibilityGrid {
public:
    static constexpr u32 CELL_SIZE = 16;
    static constexpr u32 MAX_ARMIES = 16;

    VisibilityGrid(u32 map_width, u32 map_height);

    u32 grid_width() const { return grid_width_; }
    u32 grid_height() const { return grid_height_; }
    u32 cell_size() const { return CELL_SIZE; }

    /// Convert world position to grid coordinates (clamped).
    void world_to_grid(f32 wx, f32 wz, u32& gx, u32& gz) const;

    /// Clear transient flags (Vision/Radar/Sonar/Omni) but keep EverSeen.
    void clear_transient();

    /// Paint a circle of the given flag for the given army.
    /// If flag includes Vision, also sets EverSeen on affected cells.
    void paint_circle(u32 army, f32 wx, f32 wz, f32 radius, VisFlag flag);

    /// OR all flags from army src into army dst (for alliance sharing).
    void merge_armies(u32 dst, u32 src);

    /// Query: does army have vision at world position?
    bool has_vision(f32 wx, f32 wz, u32 army) const;
    bool has_radar(f32 wx, f32 wz, u32 army) const;
    bool has_sonar(f32 wx, f32 wz, u32 army) const;
    bool has_omni(f32 wx, f32 wz, u32 army) const;
    bool ever_seen(f32 wx, f32 wz, u32 army) const;

    /// Raw flag query at grid coordinates.
    VisFlag get(u32 gx, u32 gz, u32 army) const;

private:
    u32 grid_width_;
    u32 grid_height_;
    u32 map_width_;
    u32 map_height_;

    // cells_[army][gz * grid_width_ + gx]
    std::array<std::vector<VisFlag>, MAX_ARMIES> cells_;
};

} // namespace osc::map
