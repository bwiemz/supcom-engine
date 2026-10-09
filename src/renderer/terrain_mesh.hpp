#pragma once

#include "renderer/vk_types.hpp"
#include "core/types.hpp"

#include <vector>

namespace osc::map {
class Terrain;
}

namespace osc::renderer {

struct TerrainVertex {
    f32 x, y, z;    // position
    f32 nx, ny, nz; // normal
};

/// Generates a decimated heightmap mesh and uploads to GPU.
class TerrainMesh {
public:
    /// Generate mesh from terrain heightmap (every DECIMATE-th sample).
    void build(const osc::map::Terrain& terrain, VkDevice device,
               VmaAllocator allocator, VkCommandPool cmd_pool, VkQueue queue);

    void destroy(VkDevice device, VmaAllocator allocator);

    VkBuffer vertex_buffer() const { return vertex_buf_.buffer; }
    VkBuffer index_buffer() const { return index_buf_.buffer; }
    u32 index_count() const { return index_count_; }
    /// Moho's TTerrainSkirt walls from the map's edges down to its lowest
    /// height (CTesselator::TesselateData), after the terrain's indices.
    u32 skirt_first_index() const { return index_count_; }
    u32 skirt_index_count() const { return skirt_index_count_; }

    /// Append the indices of its quads (two triangles each, as it draws
    /// them) that lie in the world rectangle [min_x, max_x] x [min_z,
    /// max_z]: the terrain's own triangles, which a decal draws (Moho's
    /// CollectClippedCollisionIndicesInRect; M212b). Nothing for a
    /// rectangle off the map.
    void collect_indices(f32 min_x, f32 min_z, f32 max_x, f32 max_z, std::vector<u32>& out) const;

    static constexpr u32 DECIMATE = 2; // sample every 2nd point

private:
    AllocatedBuffer vertex_buf_{};
    AllocatedBuffer index_buf_{};
    u32 index_count_ = 0;
    u32 skirt_index_count_ = 0;
    u32 grid_w_ = 0, grid_h_ = 0; ///< its vertices across and down
};

} // namespace osc::renderer
