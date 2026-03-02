#include "renderer/terrain_mesh.hpp"
#include "renderer/vk_types.hpp"
#include "map/terrain.hpp"
#include "map/heightmap.hpp"

#include <spdlog/spdlog.h>

#include <cmath>
#include <vector>

namespace osc::renderer {

void TerrainMesh::build(const osc::map::Terrain& terrain, VkDevice device,
                        VmaAllocator allocator, VkCommandPool cmd_pool,
                        VkQueue queue) {
    const auto& hm = terrain.heightmap();
    u32 gw = hm.grid_width();
    u32 gh = hm.grid_height();

    // Decimated grid dimensions
    u32 dw = (gw - 1) / DECIMATE + 1;
    u32 dh = (gh - 1) / DECIMATE + 1;

    // Generate vertices with normals
    std::vector<TerrainVertex> vertices(dw * dh);

    for (u32 dz = 0; dz < dh; dz++) {
        for (u32 dx = 0; dx < dw; dx++) {
            u32 gx = std::min(dx * DECIMATE, gw - 1);
            u32 gz = std::min(dz * DECIMATE, gh - 1);

            f32 h = hm.get_height_at_grid(gx, gz);
            auto& v = vertices[dz * dw + dx];
            v.x = static_cast<f32>(gx);
            v.y = h;
            v.z = static_cast<f32>(gz);

            // Central-difference normal
            f32 hL = (gx > 0) ? hm.get_height_at_grid(gx - 1, gz) : h;
            f32 hR = (gx + 1 < gw) ? hm.get_height_at_grid(gx + 1, gz) : h;
            f32 hD = (gz > 0) ? hm.get_height_at_grid(gx, gz - 1) : h;
            f32 hU = (gz + 1 < gh) ? hm.get_height_at_grid(gx, gz + 1) : h;

            f32 nx = hL - hR;
            f32 nz = hD - hU;
            f32 ny = 2.0f;
            f32 len = std::sqrt(nx * nx + ny * ny + nz * nz);
            if (len > 0) { nx /= len; ny /= len; nz /= len; }

            v.nx = nx;
            v.ny = ny;
            v.nz = nz;
        }
    }

    // Generate indices (two triangles per quad)
    u32 quads_x = dw - 1;
    u32 quads_z = dh - 1;
    std::vector<u32> indices;
    indices.reserve(quads_x * quads_z * 6);

    for (u32 z = 0; z < quads_z; z++) {
        for (u32 x = 0; x < quads_x; x++) {
            u32 tl = z * dw + x;
            u32 tr = tl + 1;
            u32 bl = (z + 1) * dw + x;
            u32 br = bl + 1;

            indices.push_back(tl);
            indices.push_back(bl);
            indices.push_back(tr);

            indices.push_back(tr);
            indices.push_back(bl);
            indices.push_back(br);
        }
    }

    index_count_ = static_cast<u32>(indices.size());

    spdlog::info("Terrain mesh: {}x{} grid, {} vertices, {} indices",
                 dw, dh, vertices.size(), index_count_);

    // Upload to GPU
    vertex_buf_ = upload_buffer(device, allocator, cmd_pool, queue,
                                vertices.data(),
                                vertices.size() * sizeof(TerrainVertex),
                                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    index_buf_ = upload_buffer(device, allocator, cmd_pool, queue,
                               indices.data(),
                               indices.size() * sizeof(u32),
                               VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
}

void TerrainMesh::destroy(VkDevice device, VmaAllocator allocator) {
    if (vertex_buf_.buffer)
        vmaDestroyBuffer(allocator, vertex_buf_.buffer, vertex_buf_.allocation);
    if (index_buf_.buffer)
        vmaDestroyBuffer(allocator, index_buf_.buffer, index_buf_.allocation);
    vertex_buf_ = {};
    index_buf_ = {};
    index_count_ = 0;
}

} // namespace osc::renderer
