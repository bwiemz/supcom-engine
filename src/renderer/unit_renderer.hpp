#pragma once

#include "renderer/vk_types.hpp"
#include "core/types.hpp"

namespace osc::sim {
class SimState;
}

namespace osc::renderer {

struct UnitInstance {
    f32 x, y, z;     // world position
    f32 scale;        // footprint-based scale
    f32 r, g, b, a;  // army color + alpha
};

/// Renders units as instanced colored cubes.
class UnitRenderer {
public:
    /// Upload static cube mesh to GPU.
    void build(VkDevice device, VmaAllocator allocator,
               VkCommandPool cmd_pool, VkQueue queue);

    /// Update per-frame instance data from sim state.
    void update(const sim::SimState& sim);

    void destroy(VkDevice device, VmaAllocator allocator);

    VkBuffer vertex_buffer() const { return cube_verts_.buffer; }
    VkBuffer index_buffer() const { return cube_indices_.buffer; }
    VkBuffer instance_buffer() const { return instance_buf_.buffer; }
    u32 index_count() const { return 36; }
    u32 instance_count() const { return instance_count_; }

    static constexpr u32 MAX_INSTANCES = 2048;

private:
    AllocatedBuffer cube_verts_{};
    AllocatedBuffer cube_indices_{};
    AllocatedBuffer instance_buf_{};
    void* instance_mapped_ = nullptr; // persistently mapped
    u32 instance_count_ = 0;
};

} // namespace osc::renderer
