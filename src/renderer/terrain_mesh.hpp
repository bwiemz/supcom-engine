#pragma once

#include "renderer/vk_types.hpp"
#include "core/types.hpp"

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

    static constexpr u32 DECIMATE = 2; // sample every 2nd point

private:
    AllocatedBuffer vertex_buf_{};
    AllocatedBuffer index_buf_{};
    u32 index_count_ = 0;
};

} // namespace osc::renderer
