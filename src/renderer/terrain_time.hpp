#pragma once

#include "core/types.hpp"

#include <array>

namespace osc::renderer {

/// The terrain shader's Time (TTerrainGlow's scroll, M212f), as Moho's
/// HighFidelityTerrain::UpdateRenderContext sets it: the tick and
/// interpolant, but only when the terrain re-tessellates -- the camera's
/// transform differs bit for bit (VTransform::Compare), or the decals have
/// changed. So FA's lava stands still under a still camera.
class TerrainTime {
public:
    /// This frame's Time: `now` (tick + interpolant) when `view` (the
    /// camera's view matrix) or `decals` (a count of what's drawn) changed
    /// since it was last set, or it never was; else the last.
    f32 update(const std::array<f32, 16>& view, u64 decals, f32 now) {
        if (!set_ || view != view_ || decals != decals_) {
            time_ = now;
            view_ = view;
            decals_ = decals;
            set_ = true;
        }
        return time_;
    }
    f32 value() const { return time_; }
    /// A new scene: the next frame sets it.
    void reset() { set_ = false; }

private:
    f32 time_ = 0.0f;
    bool set_ = false;
    std::array<f32, 16> view_{};
    u64 decals_ = 0;
};

} // namespace osc::renderer
