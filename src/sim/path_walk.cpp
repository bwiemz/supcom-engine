#include "sim/path_walk.hpp"

#include "map/terrain.hpp"

#include <cmath>

namespace osc::sim::path {

namespace {

// Where in a cell a sweep's line runs: its middle for a straight step, near
// opposite corners for a diagonal one.
constexpr f32 kProbeLow = 0.1f;
constexpr f32 kProbeHigh = 0.9f;
constexpr f32 kProbeCentre = 0.5f;

} // namespace

GridLine::GridLine(i32 step, f32 x_start, f32 z_start, f32 x_end, f32 z_end) : step_(step) {
    if (x_end < x_start) {
        x0_ = -x_start;
        x1_ = -x_end;
        x_mask_ = -step;
    } else {
        x0_ = x_start;
        x1_ = x_end;
    }
    if (z_end < z_start) {
        z0_ = -z_start;
        z1_ = -z_end;
        z_mask_ = -step;
    } else {
        z0_ = z_start;
        z1_ = z_end;
    }
    dx_ = x1_ - x0_;
    dz_ = z1_ - z0_;
    x_edge_ = static_cast<i32>(std::floor(x0_)) & -step;
    z_edge_ = static_cast<i32>(std::floor(z0_)) & -step;
}

Cell GridLine::cell() const {
    // A mirrored axis's edge, flipped back: -e - 1 for a step of 1.
    return {x_edge_ ^ x_mask_, z_edge_ ^ z_mask_};
}

void GridLine::advance() {
    const i32 next_x = x_edge_ + step_;
    const i32 next_z = z_edge_ + step_;
    // Which edge the segment meets first, by cross-multiplying from its end.
    const f32 x_metric = (static_cast<f32>(next_x) - x1_) * dz_;
    const f32 z_metric = (static_cast<f32>(next_z) - z1_) * dx_;
    if (z_metric <= x_metric) z_edge_ = next_z;
    else x_edge_ = next_x;
}

bool GridLine::beyond_end() const {
    return static_cast<f32>(x_edge_) > x1_ || static_cast<f32>(z_edge_) > z1_;
}

bool footprint_clear_along_line(const blueprints::Footprint& fp, bool on_water,
                                const map::Terrain& terrain, const OccupancyGrid& grid, Cell from,
                                Cell to, f32 probe_x, f32 probe_z) {
    GridLine line(1, static_cast<f32>(from.x) + probe_x, static_cast<f32>(from.z) + probe_z,
                  static_cast<f32>(to.x) + probe_x, static_cast<f32>(to.z) + probe_z);
    // Tested before the end check: the first cell always is.
    for (;;) {
        const Cell raw = line.cell();
        const Cell c{static_cast<i16>(raw.x), static_cast<i16>(raw.z)};
        u8 caps = map_caps(fp, terrain, c.x, c.z);
        if (on_water) caps = static_cast<u8>(caps & ~blueprints::occupancy::kSub);
        if (footprint_fits(fp, terrain, grid, c.x, c.z, caps) == 0) return false;
        line.advance();
        if (line.beyond_end()) return true;
    }
}

bool cell_step_clear(const blueprints::Footprint& fp, bool on_water, const map::Terrain& terrain,
                     const OccupancyGrid& grid, Cell from, Cell to) {
    const auto along = [&](f32 px, f32 pz) {
        return footprint_clear_along_line(fp, on_water, terrain, grid, from, to, px, pz);
    };
    if (from.x == to.x || from.z == to.z) return along(kProbeCentre, kProbeCentre);
    const bool main_diagonal = (to.x > from.x && to.z > from.z) || (to.x < from.x && to.z < from.z);
    if (main_diagonal) return along(kProbeLow, kProbeHigh) && along(kProbeHigh, kProbeLow);
    return along(kProbeLow, kProbeLow) && along(kProbeHigh, kProbeHigh);
}

} // namespace osc::sim::path
