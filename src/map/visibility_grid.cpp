#include "map/visibility_grid.hpp"

#include <algorithm>
#include <cmath>

namespace osc::map {

VisibilityGrid::VisibilityGrid(u32 map_width, u32 map_height)
    : map_width_(map_width), map_height_(map_height) {
    grid_width_ = map_width_ / CELL_SIZE;
    grid_height_ = map_height_ / CELL_SIZE;
    if (grid_width_ == 0) grid_width_ = 1;
    if (grid_height_ == 0) grid_height_ = 1;
    for (u32 i = 0; i < MAX_ARMIES; ++i) {
        cells_[i].resize(
            static_cast<size_t>(grid_width_) * grid_height_, VisFlag::None);
    }
}

void VisibilityGrid::world_to_grid(f32 wx, f32 wz, u32& gx, u32& gz) const {
    f32 fx = wx / static_cast<f32>(CELL_SIZE);
    f32 fz = wz / static_cast<f32>(CELL_SIZE);
    gx = static_cast<u32>(std::max(
        0.0f, std::min(fx, static_cast<f32>(grid_width_ - 1))));
    gz = static_cast<u32>(std::max(
        0.0f, std::min(fz, static_cast<f32>(grid_height_ - 1))));
}

void VisibilityGrid::clear_transient() {
    for (u32 a = 0; a < MAX_ARMIES; ++a) {
        for (auto& cell : cells_[a]) {
            cell = cell & VisFlag::EverSeen; // keep only EverSeen
        }
    }
}

void VisibilityGrid::paint_circle(u32 army, f32 wx, f32 wz, f32 radius,
                                  VisFlag flag) {
    if (army >= MAX_ARMIES || radius <= 0.0f) return;

    // Compute grid bounding box
    u32 gx_min, gz_min, gx_max, gz_max;
    world_to_grid(wx - radius, wz - radius, gx_min, gz_min);
    world_to_grid(wx + radius, wz + radius, gx_max, gz_max);

    f32 r_sq = radius * radius;

    for (u32 gz = gz_min; gz <= gz_max; ++gz) {
        for (u32 gx = gx_min; gx <= gx_max; ++gx) {
            // Cell center in world coordinates
            f32 cx =
                (static_cast<f32>(gx) + 0.5f) * static_cast<f32>(CELL_SIZE);
            f32 cz =
                (static_cast<f32>(gz) + 0.5f) * static_cast<f32>(CELL_SIZE);
            f32 dx = cx - wx;
            f32 dz = cz - wz;
            if (dx * dx + dz * dz <= r_sq) {
                u32 idx = gz * grid_width_ + gx;
                cells_[army][idx] |= flag;
                // Vision also sets EverSeen
                if (has_flag(flag, VisFlag::Vision)) {
                    cells_[army][idx] |= VisFlag::EverSeen;
                }
            }
        }
    }
}

void VisibilityGrid::merge_armies(u32 dst, u32 src) {
    if (dst >= MAX_ARMIES || src >= MAX_ARMIES) return;
    u32 total = grid_width_ * grid_height_;
    for (u32 i = 0; i < total; ++i) {
        cells_[dst][i] |= cells_[src][i];
    }
}

VisFlag VisibilityGrid::get(u32 gx, u32 gz, u32 army) const {
    if (army >= MAX_ARMIES || gx >= grid_width_ || gz >= grid_height_)
        return VisFlag::None;
    return cells_[army][gz * grid_width_ + gx];
}

bool VisibilityGrid::has_vision(f32 wx, f32 wz, u32 army) const {
    u32 gx, gz;
    world_to_grid(wx, wz, gx, gz);
    return has_flag(get(gx, gz, army), VisFlag::Vision);
}

bool VisibilityGrid::has_radar(f32 wx, f32 wz, u32 army) const {
    u32 gx, gz;
    world_to_grid(wx, wz, gx, gz);
    return has_flag(get(gx, gz, army), VisFlag::Radar);
}

bool VisibilityGrid::has_sonar(f32 wx, f32 wz, u32 army) const {
    u32 gx, gz;
    world_to_grid(wx, wz, gx, gz);
    return has_flag(get(gx, gz, army), VisFlag::Sonar);
}

bool VisibilityGrid::has_omni(f32 wx, f32 wz, u32 army) const {
    u32 gx, gz;
    world_to_grid(wx, wz, gx, gz);
    return has_flag(get(gx, gz, army), VisFlag::Omni);
}

bool VisibilityGrid::ever_seen(f32 wx, f32 wz, u32 army) const {
    u32 gx, gz;
    world_to_grid(wx, wz, gx, gz);
    return has_flag(get(gx, gz, army), VisFlag::EverSeen);
}

} // namespace osc::map
