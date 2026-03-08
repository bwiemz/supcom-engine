#include "renderer/water_renderer.hpp"
#include "renderer/vk_types.hpp"
#include "map/terrain.hpp"
#include "map/heightmap.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace osc::renderer {

void WaterRenderer::build(const osc::map::Terrain& terrain,
                          VkDevice device, VmaAllocator allocator,
                          VkCommandPool cmd_pool, VkQueue queue) {
    if (!terrain.has_water()) return;

    has_water_ = true;
    water_elevation_ = terrain.water_elevation();

    const auto& hm = terrain.heightmap();
    f32 map_w = static_cast<f32>(hm.map_width());
    f32 map_h = static_cast<f32>(hm.map_height());

    // Tessellated grid dimensions
    u32 cols = static_cast<u32>(map_w) / GRID_STEP + 1;
    u32 rows = static_cast<u32>(map_h) / GRID_STEP + 1;

    // Generate vertices with depth
    std::vector<WaterVertex> vertices(cols * rows);
    for (u32 rz = 0; rz < rows; rz++) {
        for (u32 cx = 0; cx < cols; cx++) {
            f32 wx = std::min(static_cast<f32>(cx * GRID_STEP), map_w);
            f32 wz = std::min(static_cast<f32>(rz * GRID_STEP), map_h);
            f32 terrain_h = hm.get_height(wx, wz);
            f32 depth = water_elevation_ - terrain_h;
            if (depth < 0.0f) depth = 0.0f;

            auto& v = vertices[rz * cols + cx];
            v.x = wx;
            v.y = water_elevation_;
            v.z = wz;
            v.depth = depth;
        }
    }

    // Generate indices (two triangles per quad)
    u32 quads_x = cols - 1;
    u32 quads_z = rows - 1;
    std::vector<u32> indices;
    indices.reserve(quads_x * quads_z * 6);

    for (u32 z = 0; z < quads_z; z++) {
        for (u32 x = 0; x < quads_x; x++) {
            u32 tl = z * cols + x;
            u32 tr = tl + 1;
            u32 bl = (z + 1) * cols + x;
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

    spdlog::info("Water mesh: {}x{} grid ({} step), {} verts, {} indices, "
                 "elevation={:.1f}",
                 cols, rows, GRID_STEP, vertices.size(), index_count_,
                 water_elevation_);

    // Upload to GPU
    vertex_buf_ = upload_buffer(device, allocator, cmd_pool, queue,
                                vertices.data(),
                                vertices.size() * sizeof(WaterVertex),
                                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    index_buf_ = upload_buffer(device, allocator, cmd_pool, queue,
                               indices.data(),
                               indices.size() * sizeof(u32),
                               VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
}

void WaterRenderer::destroy(VkDevice device, VmaAllocator allocator) {
    if (vertex_buf_.buffer)
        vmaDestroyBuffer(allocator, vertex_buf_.buffer, vertex_buf_.allocation);
    if (index_buf_.buffer)
        vmaDestroyBuffer(allocator, index_buf_.buffer, index_buf_.allocation);
    vertex_buf_ = {};
    index_buf_ = {};
    index_count_ = 0;
    has_water_ = false;
}

} // namespace osc::renderer
