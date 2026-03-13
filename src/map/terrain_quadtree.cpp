#include "map/terrain_quadtree.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace osc::map {

// ---------------------------------------------------------------------------
// Build
// ---------------------------------------------------------------------------

void TerrainQuadtree::build(const f32* heights, u32 width, u32 height,
                             f32 cell_size) {
    heights_   = heights;
    hmap_w_    = width;
    hmap_h_    = height;
    cell_size_ = cell_size;

    nodes_.clear();
    if (width == 0 || height == 0 || !heights) return;

    // ~4/3 * (cells / leaf_area) nodes; generous 2x for non-square maps
    nodes_.reserve(static_cast<size_t>((width / 4 + 1) * (height / 4 + 1) * 2));

    nodes_.push_back(Node{});
    build_recursive(0, heights, width, height, cell_size,
                    0, 0, width, height);
}

void TerrainQuadtree::build_recursive(i32 node_idx,
                                       const f32* heights,
                                       u32 hmap_w, u32 hmap_h,
                                       f32 cell_size,
                                       u32 cx0, u32 cz0,
                                       u32 cx1, u32 cz1) {
    nodes_[node_idx].x0 = static_cast<f32>(cx0) * cell_size;
    nodes_[node_idx].z0 = static_cast<f32>(cz0) * cell_size;
    nodes_[node_idx].x1 = static_cast<f32>(cx1) * cell_size;
    nodes_[node_idx].z1 = static_cast<f32>(cz1) * cell_size;

    // Compute min/max heights across all samples in the region.
    f32 min_h =  std::numeric_limits<f32>::max();
    f32 max_h = -std::numeric_limits<f32>::max();

    u32 sx1 = std::min(cx1, hmap_w - 1);
    u32 sz1 = std::min(cz1, hmap_h - 1);

    for (u32 z = cz0; z <= sz1; ++z) {
        for (u32 x = cx0; x <= sx1; ++x) {
            f32 h = heights[z * hmap_w + x];
            if (h < min_h) min_h = h;
            if (h > max_h) max_h = h;
        }
    }

    nodes_[node_idx].min_h = min_h;
    nodes_[node_idx].max_h = max_h;

    u32 w = cx1 - cx0;
    u32 h = cz1 - cz0;
    if (w <= 4 && h <= 4) {
        // Leaf node.
        return;
    }

    u32 mx = cx0 + w / 2;
    u32 mz = cz0 + h / 2;

    struct Quad { u32 x0, z0, x1, z1; };
    const Quad quads[4] = {
        {cx0, cz0, mx,  mz },
        {mx,  cz0, cx1, mz },
        {cx0, mz,  mx,  cz1},
        {mx,  mz,  cx1, cz1},
    };

    for (int c = 0; c < 4; ++c) {
        if (quads[c].x0 >= quads[c].x1 || quads[c].z0 >= quads[c].z1)
            continue;

        i32 child_idx = static_cast<i32>(nodes_.size());
        nodes_.push_back(Node{});
        // Access parent by index — not by reference — since push_back may
        // have reallocated the vector.
        nodes_[node_idx].children[c] = child_idx;

        build_recursive(child_idx, heights, hmap_w, hmap_h, cell_size,
                        quads[c].x0, quads[c].z0,
                        quads[c].x1, quads[c].z1);
    }
}

// ---------------------------------------------------------------------------
// Ray-AABB slab test
// ---------------------------------------------------------------------------

bool TerrainQuadtree::ray_aabb(f32 ox, f32 oy, f32 oz,
                                f32 dx, f32 dy, f32 dz,
                                f32 x0, f32 y0, f32 z0,
                                f32 x1, f32 y1, f32 z1,
                                f32& t_enter, f32& t_exit) {
    f32 tmin = -std::numeric_limits<f32>::max();
    f32 tmax =  std::numeric_limits<f32>::max();

    auto slab = [&](f32 o, f32 d, f32 lo, f32 hi) -> bool {
        if (std::abs(d) < 1e-9f) {
            return (o >= lo && o <= hi);
        }
        f32 inv_d = 1.0f / d;
        f32 t0 = (lo - o) * inv_d;
        f32 t1 = (hi - o) * inv_d;
        if (t0 > t1) std::swap(t0, t1);
        tmin = std::max(tmin, t0);
        tmax = std::min(tmax, t1);
        return tmin <= tmax;
    };

    if (!slab(ox, dx, x0, x1)) return false;
    if (!slab(oy, dy, y0, y1)) return false;
    if (!slab(oz, dz, z0, z1)) return false;

    if (tmax < 0.0f) return false;

    t_enter = tmin;
    t_exit  = tmax;
    return true;
}

// ---------------------------------------------------------------------------
// Bilinear height at world position (px, pz) within one cell
// ---------------------------------------------------------------------------
static f32 bilinear_height(f32 px, f32 pz,
                            f32 wx0, f32 wz0, f32 wx1, f32 wz1,
                            f32 h00, f32 h10, f32 h01, f32 h11) {
    f32 u = (px - wx0) / (wx1 - wx0);
    f32 v = (pz - wz0) / (wz1 - wz0);
    u = std::max(0.0f, std::min(1.0f, u));
    v = std::max(0.0f, std::min(1.0f, v));
    return h00*(1-u)*(1-v) + h10*u*(1-v) + h01*(1-u)*v + h11*u*v;
}

// ---------------------------------------------------------------------------
// Raycast public entry
// ---------------------------------------------------------------------------

bool TerrainQuadtree::raycast(f32 ox, f32 oy, f32 oz,
                               f32 dx, f32 dy, f32 dz,
                               f32& hx, f32& hy, f32& hz) const {
    if (nodes_.empty()) return false;

    f32 best_t = std::numeric_limits<f32>::max();
    return raycast_node(0, ox, oy, oz, dx, dy, dz, best_t, hx, hy, hz);
}

// ---------------------------------------------------------------------------
// Raycast node (recursive)
// ---------------------------------------------------------------------------

bool TerrainQuadtree::raycast_node(i32 idx,
                                    f32 ox, f32 oy, f32 oz,
                                    f32 dx, f32 dy, f32 dz,
                                    f32& best_t,
                                    f32& hx, f32& hy, f32& hz) const {
    const Node& node = nodes_[idx];

    // Test ray against node AABB.
    f32 t_enter, t_exit;
    if (!ray_aabb(ox, oy, oz, dx, dy, dz,
                  node.x0, node.min_h, node.z0,
                  node.x1, node.max_h, node.z1,
                  t_enter, t_exit)) {
        return false;
    }
    if (t_exit < 0.0f || t_enter >= best_t) return false;

    // Check leaf.
    bool is_leaf = true;
    for (int c = 0; c < 4; ++c) {
        if (node.children[c] >= 0) { is_leaf = false; break; }
    }

    if (is_leaf) {
        // Iterate every cell in this leaf region and find the closest
        // intersection using step-marching along the ray.
        u32 cx0 = static_cast<u32>(std::max(0.0f, node.x0 / cell_size_));
        u32 cz0 = static_cast<u32>(std::max(0.0f, node.z0 / cell_size_));
        // cx1/cz1 are the last valid cell indices (not sample indices).
        u32 cx1 = (hmap_w_ >= 2)
                  ? std::min(static_cast<u32>(node.x1 / cell_size_), hmap_w_ - 2)
                  : 0;
        u32 cz1 = (hmap_h_ >= 2)
                  ? std::min(static_cast<u32>(node.z1 / cell_size_), hmap_h_ - 2)
                  : 0;

        bool hit = false;
        for (u32 cz = cz0; cz <= cz1; ++cz) {
            for (u32 cx = cx0; cx <= cx1; ++cx) {
                f32 wx0 = static_cast<f32>(cx)     * cell_size_;
                f32 wx1 = static_cast<f32>(cx + 1) * cell_size_;
                f32 wz0 = static_cast<f32>(cz)     * cell_size_;
                f32 wz1 = static_cast<f32>(cz + 1) * cell_size_;

                f32 h00 = heights_[cz       * hmap_w_ + cx    ];
                f32 h10 = heights_[cz       * hmap_w_ + cx + 1];
                f32 h01 = heights_[(cz + 1) * hmap_w_ + cx    ];
                f32 h11 = heights_[(cz + 1) * hmap_w_ + cx + 1];

                f32 cell_min = std::min({h00, h10, h01, h11});
                f32 cell_max = std::max({h00, h10, h01, h11});

                // Per-cell AABB test.
                f32 te, tx;
                if (!ray_aabb(ox, oy, oz, dx, dy, dz,
                              wx0, cell_min, wz0,
                              wx1, cell_max, wz1,
                              te, tx)) continue;
                if (tx < 0.0f || te >= best_t) continue;

                // Step-march from te to tx sampling bilinear height.
                // We use small enough steps to catch the crossing.
                const f32 step = cell_size_ * 0.1f;
                f32 t_lo = std::max(te, 0.0f);
                f32 t_hi = std::min(tx, best_t);

                // Evaluate height above/below terrain at t_lo.
                // Sample one step *before* t_lo to seed prev_diff so
                // that a touch at t_lo (diff == 0) is detected.
                f32 t_prev = t_lo - step;
                f32 prev_py  = oy + dy * t_prev;
                f32 prev_px  = ox + dx * t_prev;
                f32 prev_pz  = oz + dz * t_prev;
                f32 prev_terrain = bilinear_height(prev_px, prev_pz,
                                                   wx0, wz0, wx1, wz1,
                                                   h00, h10, h01, h11);
                f32 prev_diff = prev_py - prev_terrain;

                // If prev point is already underground, ray comes from below;
                // no top-down intersection for this cell.
                if (prev_diff < 0.0f) continue;

                bool cell_hit = false;
                f32 t_cur = t_lo;
                while (t_cur <= t_hi + step * 0.5f) {
                    f32 tt = std::min(t_cur, t_hi);
                    f32 px = ox + dx * tt;
                    f32 py = oy + dy * tt;
                    f32 pz = oz + dz * tt;

                    f32 terrain_h = bilinear_height(px, pz,
                                                    wx0, wz0, wx1, wz1,
                                                    h00, h10, h01, h11);
                    f32 diff = py - terrain_h;

                    if (prev_diff >= 0.0f && diff <= 0.0f) {
                        // Binary-search for precise crossing within [t_prev, tt].
                        f32 ta = t_prev, tb = tt;
                        for (int iter = 0; iter < 8; ++iter) {
                            f32 tm = (ta + tb) * 0.5f;
                            f32 pm_x = ox + dx * tm;
                            f32 pm_y = oy + dy * tm;
                            f32 pm_z = oz + dz * tm;
                            f32 tm_h = bilinear_height(pm_x, pm_z,
                                                       wx0, wz0, wx1, wz1,
                                                       h00, h10, h01, h11);
                            if (pm_y - tm_h >= 0.0f) ta = tm;
                            else                       tb = tm;
                        }
                        f32 hit_t = (ta + tb) * 0.5f;
                        if (hit_t < best_t) {
                            best_t = hit_t;
                            hx = ox + dx * hit_t;
                            hy = oy + dy * hit_t;
                            hz = oz + dz * hit_t;
                            hit = true;
                            cell_hit = true;
                        }
                        break;
                    }

                    prev_diff = diff;
                    t_prev    = tt;

                    if (tt == t_hi) break;
                    t_cur += step;
                }
                (void)cell_hit;
            }
        }
        return hit;
    }

    // Interior node: visit children closest-first.
    struct ChildHit { i32 idx; f32 t_enter; };
    std::array<ChildHit, 4> hits;
    int n_hits = 0;

    for (int c = 0; c < 4; ++c) {
        i32 ci = node.children[c];
        if (ci < 0) continue;
        const Node& cn = nodes_[ci];
        f32 te, tx;
        if (ray_aabb(ox, oy, oz, dx, dy, dz,
                     cn.x0, cn.min_h, cn.z0,
                     cn.x1, cn.max_h, cn.z1,
                     te, tx) && tx >= 0.0f && te < best_t) {
            hits[n_hits++] = {ci, te};
        }
    }

    std::sort(hits.begin(), hits.begin() + n_hits,
              [](const ChildHit& a, const ChildHit& b) {
                  return a.t_enter < b.t_enter;
              });

    bool hit = false;
    for (int i = 0; i < n_hits; ++i) {
        if (hits[i].t_enter >= best_t) break;
        if (raycast_node(hits[i].idx, ox, oy, oz, dx, dy, dz,
                         best_t, hx, hy, hz)) {
            hit = true;
        }
    }
    return hit;
}

} // namespace osc::map
