#include "map/visibility_grid.hpp"
#include "map/terrain.hpp"

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

void VisibilityGrid::build_height_grid(const Terrain& terrain) {
    height_grid_.resize(static_cast<size_t>(grid_width_) * grid_height_);
    for (u32 gz = 0; gz < grid_height_; ++gz) {
        for (u32 gx = 0; gx < grid_width_; ++gx) {
            f32 cx =
                (static_cast<f32>(gx) + 0.5f) * static_cast<f32>(CELL_SIZE);
            f32 cz =
                (static_cast<f32>(gz) + 0.5f) * static_cast<f32>(CELL_SIZE);
            height_grid_[gz * grid_width_ + gx] =
                terrain.get_terrain_height(cx, cz);
        }
    }
}

bool VisibilityGrid::check_los(u32 src_gx, u32 src_gz, u32 tgt_gx,
                                u32 tgt_gz, f32 eye_height) const {
    if (src_gx == tgt_gx && src_gz == tgt_gz) return true;

    f32 cell_f = static_cast<f32>(CELL_SIZE);
    f32 src_wx = (static_cast<f32>(src_gx) + 0.5f) * cell_f;
    f32 src_wz = (static_cast<f32>(src_gz) + 0.5f) * cell_f;

    // Bresenham setup
    i32 dx = static_cast<i32>(tgt_gx) - static_cast<i32>(src_gx);
    i32 dz = static_cast<i32>(tgt_gz) - static_cast<i32>(src_gz);
    i32 sx = dx > 0 ? 1 : (dx < 0 ? -1 : 0);
    i32 sz = dz > 0 ? 1 : (dz < 0 ? -1 : 0);
    dx = std::abs(dx);
    dz = std::abs(dz);

    i32 x = static_cast<i32>(src_gx);
    i32 z = static_cast<i32>(src_gz);

    // Track max slope using signed-square form to avoid sqrt:
    // slope = h_diff / dist, so slope^2 * sign = h_diff * |h_diff| / dist_sq
    // Ordering is preserved since dist > 0 always.
    f32 max_ssq = -1e30f; // max signed-slope-squared

    auto signed_slope_sq = [](f32 h_diff, f32 dist_sq) -> f32 {
        return h_diff * std::abs(h_diff) / dist_sq;
    };

    // Walk Bresenham, checking intermediate cells' terrain slopes.
    // Target is visible if its slope >= max intermediate slope.
    if (dx >= dz) {
        i32 err = dx / 2;
        for (i32 i = 0; i <= dx; ++i) {
            // Skip source cell
            if (!(static_cast<u32>(x) == src_gx &&
                  static_cast<u32>(z) == src_gz)) {
                f32 cwx = (static_cast<f32>(x) + 0.5f) * cell_f;
                f32 cwz = (static_cast<f32>(z) + 0.5f) * cell_f;
                f32 ddx = cwx - src_wx;
                f32 ddz = cwz - src_wz;
                f32 dist_sq = ddx * ddx + ddz * ddz;
                f32 h = height_grid_[static_cast<u32>(z) * grid_width_ +
                                     static_cast<u32>(x)];
                f32 ssq = signed_slope_sq(h - eye_height, dist_sq);
                if (static_cast<u32>(x) == tgt_gx &&
                    static_cast<u32>(z) == tgt_gz) {
                    return ssq >= max_ssq;
                }
                max_ssq = std::max(max_ssq, ssq);
            }
            err -= dz;
            if (err < 0) {
                z += sz;
                err += dx;
            }
            x += sx;
        }
    } else {
        i32 err = dz / 2;
        for (i32 i = 0; i <= dz; ++i) {
            if (!(static_cast<u32>(x) == src_gx &&
                  static_cast<u32>(z) == src_gz)) {
                f32 cwx = (static_cast<f32>(x) + 0.5f) * cell_f;
                f32 cwz = (static_cast<f32>(z) + 0.5f) * cell_f;
                f32 ddx = cwx - src_wx;
                f32 ddz = cwz - src_wz;
                f32 dist_sq = ddx * ddx + ddz * ddz;
                f32 h = height_grid_[static_cast<u32>(z) * grid_width_ +
                                     static_cast<u32>(x)];
                f32 ssq = signed_slope_sq(h - eye_height, dist_sq);
                if (static_cast<u32>(x) == tgt_gx &&
                    static_cast<u32>(z) == tgt_gz) {
                    return ssq >= max_ssq;
                }
                max_ssq = std::max(max_ssq, ssq);
            }
            err -= dx;
            if (err < 0) {
                x += sx;
                err += dz;
            }
            z += sz;
        }
    }

    return true; // fallback — Bresenham always reaches target
}

void VisibilityGrid::paint_circle_los(u32 army, f32 wx, f32 wz, f32 radius,
                                       f32 eye_height) {
    if (army >= MAX_ARMIES || radius <= 0.0f || height_grid_.empty()) return;

    u32 src_gx, src_gz;
    world_to_grid(wx, wz, src_gx, src_gz);

    u32 gx_min, gz_min, gx_max, gz_max;
    world_to_grid(wx - radius, wz - radius, gx_min, gz_min);
    world_to_grid(wx + radius, wz + radius, gx_max, gz_max);

    f32 r_sq = radius * radius;

    for (u32 gz = gz_min; gz <= gz_max; ++gz) {
        for (u32 gx = gx_min; gx <= gx_max; ++gx) {
            f32 cx =
                (static_cast<f32>(gx) + 0.5f) * static_cast<f32>(CELL_SIZE);
            f32 cz =
                (static_cast<f32>(gz) + 0.5f) * static_cast<f32>(CELL_SIZE);
            f32 ddx = cx - wx;
            f32 ddz = cz - wz;
            if (ddx * ddx + ddz * ddz > r_sq) continue;

            // Source cell: always visible
            if (gx == src_gx && gz == src_gz) {
                u32 idx = gz * grid_width_ + gx;
                cells_[army][idx] |= VisFlag::Vision | VisFlag::EverSeen;
                continue;
            }

            if (check_los(src_gx, src_gz, gx, gz, eye_height)) {
                u32 idx = gz * grid_width_ + gx;
                cells_[army][idx] |= VisFlag::Vision | VisFlag::EverSeen;
            }
        }
    }
}

} // namespace osc::map
