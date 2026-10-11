#include "renderer/terrain_mesh.hpp"
#include "renderer/vk_types.hpp"
#include "map/terrain.hpp"
#include "map/heightmap.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace osc::renderer {

namespace {

/// The six indices of quad (x, z) in a grid `w` vertices across.
void emit_quad(u32 x, u32 z, u32 w, std::vector<u32>& out) {
    const u32 tl = z * w + x;
    const u32 tr = tl + 1;
    const u32 bl = (z + 1) * w + x;
    const u32 br = bl + 1;
    out.insert(out.end(), {tl, bl, tr, tr, bl, br});
}

TerrainVertex vertex_at(const osc::map::Heightmap& hm, u32 dx, u32 dz) {
    const u32 gw = hm.grid_width();
    const u32 gh = hm.grid_height();
    const u32 gx = std::min(dx * TerrainMesh::DECIMATE, gw - 1);
    const u32 gz = std::min(dz * TerrainMesh::DECIMATE, gh - 1);

    const f32 h = hm.get_height_at_grid(gx, gz);
    TerrainVertex v{};
    v.x = static_cast<f32>(gx);
    v.y = h;
    v.z = static_cast<f32>(gz);

    // Central-difference normal
    const f32 hL = (gx > 0) ? hm.get_height_at_grid(gx - 1, gz) : h;
    const f32 hR = (gx + 1 < gw) ? hm.get_height_at_grid(gx + 1, gz) : h;
    const f32 hD = (gz > 0) ? hm.get_height_at_grid(gx, gz - 1) : h;
    const f32 hU = (gz + 1 < gh) ? hm.get_height_at_grid(gx, gz + 1) : h;

    f32 nx = hL - hR;
    f32 nz = hD - hU;
    f32 ny = 2.0f;
    const f32 len = std::sqrt(nx * nx + ny * ny + nz * nz);
    if (len > 0) {
        nx /= len;
        ny /= len;
        nz /= len;
    }
    v.nx = nx;
    v.ny = ny;
    v.nz = nz;
    return v;
}

} // namespace

void TerrainMesh::record_update(VkCommandBuffer cmd, const osc::map::Heightmap& hm, i32 x0, i32 z0,
                                i32 x1, i32 z1) {
    if (!vertex_buf_.buffer || grid_w_ == 0 || grid_h_ == 0) {
        return;
    }
    const auto first = [](i32 g) { return static_cast<u32>(std::max(g, 0) + 1) / DECIMATE; };
    const auto last = [](i32 g, u32 n) {
        return std::min(static_cast<u32>(std::max(g, 0)) / DECIMATE, n - 1);
    };
    const u32 dx0 = first(x0 - 1);
    const u32 dz0 = first(z0 - 1);
    const u32 dx1 = last(x1, grid_w_);
    const u32 dz1 = last(z1, grid_h_);
    if (dx0 > dx1 || dz0 > dz1) {
        return;
    }

    VkBufferMemoryBarrier before{};
    before.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    before.srcAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
    before.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    before.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    before.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    before.buffer = vertex_buf_.buffer;
    before.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, nullptr, 1, &before, 0, nullptr);

    std::vector<TerrainVertex> row(dx1 - dx0 + 1);
    for (u32 dz = dz0; dz <= dz1; ++dz) {
        for (u32 dx = dx0; dx <= dx1; ++dx) {
            row[dx - dx0] = vertex_at(hm, dx, dz);
        }
        vkCmdUpdateBuffer(cmd, vertex_buf_.buffer,
                          static_cast<VkDeviceSize>(dz * grid_w_ + dx0) * sizeof(TerrainVertex),
                          row.size() * sizeof(TerrainVertex), row.data());
    }

    VkBufferMemoryBarrier after = before;
    after.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    after.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0,
                         0, nullptr, 1, &after, 0, nullptr);
}

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
            vertices[dz * dw + dx] = vertex_at(hm, dx, dz);
        }
    }

    // Generate indices (two triangles per quad)
    u32 quads_x = dw - 1;
    u32 quads_z = dh - 1;
    std::vector<u32> indices;
    indices.reserve(quads_x * quads_z * 6);

    for (u32 z = 0; z < quads_z; z++)
        for (u32 x = 0; x < quads_x; x++) emit_quad(x, z, dw, indices);

    index_count_ = static_cast<u32>(indices.size());
    grid_w_ = dw;
    grid_h_ = dh;

    f32 min_height = hm.get_height_at_grid(0, 0);
    for (u32 gz = 0; gz < gh; ++gz) {
        for (u32 gx = 0; gx < gw; ++gx) {
            min_height = std::min(min_height, hm.get_height_at_grid(gx, gz));
        }
    }
    const auto skirt_edge = [&](u32 start, u32 step, u32 count) {
        const auto first = static_cast<u32>(vertices.size());
        for (u32 i = 0; i < count; ++i) {
            TerrainVertex v = vertices[start + i * step];
            v.y = min_height;
            vertices.push_back(v);
        }
        for (u32 i = 1; i < count; ++i) {
            const u32 top0 = start + (i - 1) * step;
            const u32 top1 = start + i * step;
            indices.insert(indices.end(),
                           {first + i - 1, first + i, top1, first + i - 1, top1, top0});
        }
    };
    skirt_edge(0, 1, dw);
    skirt_edge(0, dw, dh);
    skirt_edge(dw - 1, dw, dh);
    skirt_edge((dh - 1) * dw, 1, dw);
    skirt_index_count_ = static_cast<u32>(indices.size()) - index_count_;

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
    skirt_index_count_ = 0;
    grid_w_ = grid_h_ = 0;
}

void TerrainMesh::collect_indices(f32 min_x, f32 min_z, f32 max_x, f32 max_z,
                                  std::vector<u32>& out) const {
    if (grid_w_ < 2 || grid_h_ < 2) return;
    const u32 quads_x = grid_w_ - 1;
    const u32 quads_z = grid_h_ - 1;
    const auto span = static_cast<f32>(DECIMATE);
    // Quad q covers the world from q * DECIMATE to (q + 1) * DECIMATE.
    if (max_x < 0.0f || max_z < 0.0f || min_x >= static_cast<f32>(quads_x) * span ||
        min_z >= static_cast<f32>(quads_z) * span)
        return;
    const auto quad = [span](f32 v, u32 quads) {
        return static_cast<u32>(
            std::clamp(std::floor(v / span), 0.0f, static_cast<f32>(quads - 1)));
    };
    for (u32 z = quad(min_z, quads_z); z <= quad(max_z, quads_z); ++z)
        for (u32 x = quad(min_x, quads_x); x <= quad(max_x, quads_x); ++x)
            emit_quad(x, z, grid_w_, out);
}

} // namespace osc::renderer
