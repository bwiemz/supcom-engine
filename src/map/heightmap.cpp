#include "map/heightmap.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace osc::map {

Heightmap::Heightmap(u32 map_width, u32 map_height, f32 scale,
                     std::vector<u16> raw_data)
    : grid_width_(map_width + 1),
      grid_height_(map_height + 1),
      scale_(scale),
      data_(std::move(raw_data)) {
    assert(map_width >= 1 && map_height >= 1);
    assert(data_.size() == static_cast<size_t>(grid_width_) * grid_height_);
    if (!data_.empty())
        max_height_ = static_cast<f32>(*std::max_element(data_.begin(), data_.end())) * scale_;
}

f32 Heightmap::get_height_at_grid(u32 gx, u32 gz) const {
    return static_cast<f32>(data_[gz * grid_width_ + gx]) * scale_;
}

f32 Heightmap::get_height(f32 x, f32 z) const {
    // Clamp to valid world range
    f32 max_x = static_cast<f32>(grid_width_ - 1);
    f32 max_z = static_cast<f32>(grid_height_ - 1);
    x = std::clamp(x, 0.0f, max_x);
    z = std::clamp(z, 0.0f, max_z);

    // Grid integer coordinates
    u32 gx = static_cast<u32>(x);
    u32 gz = static_cast<u32>(z);

    // Clamp to avoid reading past the last grid cell
    if (gx >= grid_width_ - 1) gx = grid_width_ - 2;
    if (gz >= grid_height_ - 1) gz = grid_height_ - 2;

    // Fractional parts for interpolation
    f32 fx = x - static_cast<f32>(gx);
    f32 fz = z - static_cast<f32>(gz);

    // Four corner heights
    f32 h00 = get_height_at_grid(gx, gz);
    f32 h10 = get_height_at_grid(gx + 1, gz);
    f32 h01 = get_height_at_grid(gx, gz + 1);
    f32 h11 = get_height_at_grid(gx + 1, gz + 1);

    // Bilinear interpolation
    f32 h0 = h00 + (h10 - h00) * fx;
    f32 h1 = h01 + (h11 - h01) * fx;
    return h0 + (h1 - h0) * fz;
}

namespace {

struct Point {
    f32 x, y, z;
};

Point along(const Point& p, const Point& d, f32 t) {
    return {p.x + d.x * t, p.y + d.y * t, p.z + d.z * t};
}

f32 dot(const Point& a, const Point& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

} // namespace

std::optional<f32> Heightmap::intersect(f32 px, f32 py, f32 pz, f32 dx, f32 dy, f32 dz, f32 start,
                                        f32 end) const {
    for (const f32 v : {px, py, pz, dx, dy, dz, start, end})
        if (!std::isfinite(v)) return std::nullopt;
    const Point pos{px, py, pz};
    const Point dir{dx, dy, dz};

    // Clip the line to the grid, and to below its highest point
    // (CHeightField::ClipSegmentToWorld).
    const auto clip = [&](f32 p, f32 d, f32 hi) {
        if (d == 0.0f) return p >= 0.0f && p <= hi;
        const f32 t0 = (-1.0f / d) * p;
        const f32 t1 = (hi - p) / d;
        const f32 near_t = t1 <= t0 ? t1 : t0;
        const f32 far_t = t1 <= t0 ? t0 : t1;
        if (near_t > start) start = near_t;
        if (far_t <= end) end = far_t;
        return true;
    };
    if (!clip(px, dx, static_cast<f32>(grid_width_ - 1)) ||
        !clip(pz, dz, static_cast<f32>(grid_height_ - 1)))
        return std::nullopt;
    if (dy != 0.0f) {
        const f32 t = (max_height_ - py) / dy;
        if (dy >= 0.0f) {
            if (t <= end) end = t;
        } else if (t > start) {
            start = t;
        }
    }
    if (end < start) return std::nullopt;

    if (dx == 0.0f && dz == 0.0f) {
        const f32 t = (get_height(px, pz) - py) / dy;
        if (t >= start && end >= t) return t;
        return std::nullopt;
    }

    // One of a cell's two triangles against the stretch p1-p2 of the line
    // (DoIntersectionLL/UR). `from` is where the line entered the cell.
    const auto triangle = [&](bool upper, i32 x, i32 z, const Point& p1, const Point& p2,
                              f32 from) -> std::optional<f32> {
        const auto ux = static_cast<u32>(x);
        const auto uz = static_cast<u32>(z);
        const f32 y00 = get_height_at_grid(ux, uz);
        const f32 y11 = get_height_at_grid(ux + 1, uz + 1);
        Point normal{};
        if (upper) { // (x, z), (x+1, z), (x+1, z+1)
            const f32 y10 = get_height_at_grid(ux + 1, uz);
            normal = {y00 - y10, 1.0f, y10 - y11};
        } else { // (x, z), (x, z+1), (x+1, z+1)
            const f32 y01 = get_height_at_grid(ux, uz + 1);
            normal = {y01 - y11, 1.0f, y00 - y01};
        }
        const f32 inv_len = 1.0f / std::sqrt(dot(normal, normal));
        normal = {normal.x * inv_len, normal.y * inv_len, normal.z * inv_len};
        const f32 plane = dot(normal, Point{static_cast<f32>(x), y00, static_cast<f32>(z)});
        if (dot(p1, normal) - plane < 0.0f) return from;
        if (dot(p2, normal) - plane >= 0.0f) return std::nullopt;
        return -((dot(pos, normal) - plane) / dot(dir, normal));
    };

    // The cell's stretch t0..t1: the triangles it crosses, in order
    // (DoIntersection's inner step).
    const auto cell = [&](i32 x, i32 z, f32 t0, f32 t1) -> std::optional<f32> {
        const Point p_start = along(pos, dir, t0);
        const Point p_end = along(pos, dir, t1);
        const auto diag = static_cast<f32>(x - z);
        const f32 start_diag = p_start.x - p_start.z;
        const f32 end_diag = p_end.x - p_end.z;
        const auto split = [&] {
            return along(pos, dir, (diag - (pos.x - pos.z)) / (dir.x - dir.z));
        };
        if (diag <= start_diag) {
            if (end_diag < diag) {
                const Point s = split();
                if (auto hit = triangle(true, x, z, p_start, s, t0)) return hit;
                return triangle(false, x, z, s, p_end, t0);
            }
            return triangle(true, x, z, p_start, p_end, t0);
        }
        if (diag <= end_diag) {
            const Point s = split();
            if (auto hit = triangle(false, x, z, p_start, s, t0)) return hit;
            return triangle(true, x, z, s, p_end, t0);
        }
        return triangle(false, x, z, p_start, p_end, t0);
    };

    // Walk the cells the line crosses in order. (Moho walks a min/max pyramid
    // to skip cells far below the line; the cells it tests, and so the first
    // hit, are the same.)
    const Point first = along(pos, dir, start);
    auto x = static_cast<i32>(std::floor(first.x));
    auto z = static_cast<i32>(std::floor(first.z));
    const i32 step_x = dx > 0.0f ? 1 : (dx < 0.0f ? -1 : 0);
    const i32 step_z = dz > 0.0f ? 1 : (dz < 0.0f ? -1 : 0);
    const auto next_x = [&] {
        return step_x == 0 ? end : (static_cast<f32>(step_x > 0 ? x + 1 : x) - px) / dx;
    };
    const auto next_z = [&] {
        return step_z == 0 ? end : (static_cast<f32>(step_z > 0 ? z + 1 : z) - pz) / dz;
    };
    const auto last_x = static_cast<i32>(grid_width_) - 1;
    const auto last_z = static_cast<i32>(grid_height_) - 1;
    f32 t = start;
    while (true) {
        const f32 tx = next_x();
        const f32 tz = next_z();
        const f32 exit_t = std::min(tx, tz);
        const bool last = exit_t >= end;
        const f32 t1 = last ? end : exit_t;
        if (x >= 0 && x < last_x && z >= 0 && z < last_z) {
            if (auto hit = cell(x, z, t, t1)) return hit;
        }
        if (last) return std::nullopt;
        t = t1;
        if (tx <= tz) {
            x += step_x;
        } else {
            z += step_z;
        }
    }
}

bool terrain_blocks_shot(const Heightmap& heightmap, f32 ax, f32 ay, f32 az, f32 bx, f32 by, f32 bz,
                         ShotArc arc) {
    ay += 1.0f;
    by += 0.5f;
    // One chord: cast from its start, as far as it is long.
    const auto blocked = [&](const Point& from, const Point& to) {
        const Point v{to.x - from.x, to.y - from.y, to.z - from.z};
        const f32 length = std::sqrt(v.x * v.x + v.z * v.z + v.y * v.y);
        if (length == 0.0f) return false;
        const f32 inv = 1.0f / length;
        const auto hit = heightmap.intersect(from.x, from.y, from.z, v.x * inv, v.y * inv,
                                             v.z * inv, 0.0f, length);
        return hit && length >= *hit;
    };
    const Point a{ax, ay, az};
    const Point b{bx, by, bz};
    if (arc == ShotArc::Straight) return blocked(a, b);

    // Four chords; each step adds its share of the half-sine's height.
    constexpr f32 kSteps[] = {0.707f, 0.293f, -0.293f, -0.707f};
    const Point quarter{(b.x - a.x) * 0.25f, (b.y - a.y) * 0.25f, (b.z - a.z) * 0.25f};
    const f32 quarter_length =
        std::sqrt(quarter.z * quarter.z + quarter.y * quarter.y + quarter.x * quarter.x);
    const f32 scale = arc == ShotArc::Low ? 0.5f : 2.0f;
    Point previous = a;
    Point current = a;
    for (const f32 step : kSteps) {
        current = {current.x + quarter.x, current.y + quarter.y, current.z + quarter.z};
        current.y += step * quarter_length * scale;
        if (blocked(previous, current)) return true;
        previous = current;
    }
    return false;
}

} // namespace osc::map
