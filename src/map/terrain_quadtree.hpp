#pragma once

#include "core/types.hpp"
#include <vector>

namespace osc::map {

/// Hierarchical heightmap for O(log N) screen ray → terrain intersection.
/// Caller must ensure the heights buffer outlives this object (not copied).
class TerrainQuadtree {
public:
    void build(const f32* heights, u32 width, u32 height, f32 cell_size);

    bool raycast(f32 ox, f32 oy, f32 oz,
                 f32 dx, f32 dy, f32 dz,
                 f32& hx, f32& hy, f32& hz) const;

    bool built() const { return !nodes_.empty(); }

private:
    struct Node {
        f32 min_h, max_h;
        f32 x0, z0, x1, z1;
        i32 children[4] = {-1, -1, -1, -1};
    };

    void build_recursive(i32 node_idx, const f32* heights,
                         u32 hmap_w, u32 hmap_h, f32 cell_size,
                         u32 cx0, u32 cz0, u32 cx1, u32 cz1);

    bool raycast_node(i32 idx, f32 ox, f32 oy, f32 oz,
                      f32 dx, f32 dy, f32 dz,
                      f32& best_t, f32& hx, f32& hy, f32& hz) const;

    static bool ray_aabb(f32 ox, f32 oy, f32 oz,
                         f32 dx, f32 dy, f32 dz,
                         f32 x0, f32 y0, f32 z0,
                         f32 x1, f32 y1, f32 z1,
                         f32& t_enter, f32& t_exit);

    std::vector<Node> nodes_;
    const f32* heights_ = nullptr;
    u32 hmap_w_ = 0, hmap_h_ = 0;
    f32 cell_size_ = 1.0f;
};

} // namespace osc::map
