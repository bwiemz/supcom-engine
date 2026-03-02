#pragma once

#include "renderer/vk_types.hpp"
#include "core/types.hpp"

namespace osc::renderer {

/// Renders a semi-transparent water plane at the water elevation.
class WaterRenderer {
public:
    void build(f32 map_width, f32 map_height, f32 water_elevation,
               VkDevice device, VmaAllocator allocator,
               VkCommandPool cmd_pool, VkQueue queue);

    void destroy(VkDevice device, VmaAllocator allocator);

    VkBuffer vertex_buffer() const { return vertex_buf_.buffer; }
    VkBuffer index_buffer() const { return index_buf_.buffer; }
    u32 index_count() const { return 6; }
    bool has_water() const { return has_water_; }

private:
    AllocatedBuffer vertex_buf_{};
    AllocatedBuffer index_buf_{};
    bool has_water_ = false;
};

} // namespace osc::renderer
