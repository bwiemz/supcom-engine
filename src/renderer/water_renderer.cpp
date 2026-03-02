#include "renderer/water_renderer.hpp"
#include "renderer/vk_types.hpp"

#include <spdlog/spdlog.h>

namespace osc::renderer {

void WaterRenderer::build(f32 map_width, f32 map_height, f32 water_elevation,
                          VkDevice device, VmaAllocator allocator,
                          VkCommandPool cmd_pool, VkQueue queue) {
    has_water_ = true;
    f32 y = water_elevation;

    // Quad covering entire map at water elevation
    f32 vertices[] = {
        0.0f,      y, 0.0f,       // 0: bottom-left
        map_width, y, 0.0f,       // 1: bottom-right
        map_width, y, map_height, // 2: top-right
        0.0f,      y, map_height, // 3: top-left
    };

    u32 indices[] = { 0, 1, 2, 2, 3, 0 };

    vertex_buf_ = upload_buffer(device, allocator, cmd_pool, queue,
                                vertices, sizeof(vertices),
                                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    index_buf_ = upload_buffer(device, allocator, cmd_pool, queue,
                               indices, sizeof(indices),
                               VK_BUFFER_USAGE_INDEX_BUFFER_BIT);

    spdlog::info("Water plane at y={:.1f}, map {}x{}", y, map_width,
                 map_height);
}

void WaterRenderer::destroy(VkDevice device, VmaAllocator allocator) {
    if (vertex_buf_.buffer)
        vmaDestroyBuffer(allocator, vertex_buf_.buffer, vertex_buf_.allocation);
    if (index_buf_.buffer)
        vmaDestroyBuffer(allocator, index_buf_.buffer, index_buf_.allocation);
    vertex_buf_ = {};
    index_buf_ = {};
    has_water_ = false;
}

} // namespace osc::renderer
