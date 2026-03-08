#pragma once

#include "renderer/vk_types.hpp"
#include "core/types.hpp"

namespace osc::map { class Terrain; }

namespace osc::renderer {

/// Renders a tessellated water plane with wave animation, depth-based coloring,
/// Fresnel specular highlights, and shoreline foam.
class WaterRenderer {
public:
    void build(const osc::map::Terrain& terrain,
               VkDevice device, VmaAllocator allocator,
               VkCommandPool cmd_pool, VkQueue queue);

    void destroy(VkDevice device, VmaAllocator allocator);

    VkBuffer vertex_buffer() const { return vertex_buf_.buffer; }
    VkBuffer index_buffer() const { return index_buf_.buffer; }
    u32 index_count() const { return index_count_; }
    bool has_water() const { return has_water_; }
    f32 water_elevation() const { return water_elevation_; }

    /// Water push constants: viewProj(64) + time(4) + eyePos(12) + waterElev(4) = 84 bytes
    struct WaterPushConstants {
        f32 view_proj[16];
        f32 time;
        f32 eye_x, eye_y, eye_z;
        f32 water_elevation;
    };

    static constexpr u32 PUSH_CONSTANT_SIZE = sizeof(WaterPushConstants);

private:
    /// Per-vertex data: position (x,y,z) + depth below water surface
    struct WaterVertex {
        f32 x, y, z;
        f32 depth; // water_elevation - terrain_height (0 at shore, positive in deep water)
    };

    static constexpr u32 GRID_STEP = 8; // tessellation step in world units

    AllocatedBuffer vertex_buf_{};
    AllocatedBuffer index_buf_{};
    u32 index_count_ = 0;
    f32 water_elevation_ = 0;
    bool has_water_ = false;
};

} // namespace osc::renderer
