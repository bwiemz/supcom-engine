#pragma once

#include "renderer/vk_types.hpp"
#include "renderer/ui_renderer.hpp" // UIInstance
#include "core/types.hpp"

#include <array>
#include <unordered_set>
#include <vector>

namespace osc::map {
class Terrain;
}

namespace osc::sim {
class SimState;
}

namespace osc::renderer {

class Camera;
class TextureCache;

/// Renders a minimap in the bottom-left corner showing terrain, unit dots,
/// and camera frustum. Supports click-to-jump via hit_test().
class MinimapRenderer {
public:
    void init(VkDevice device, VmaAllocator allocator);

    /// Generate the terrain background texture from heightmap.
    /// Call once from build_scene.
    void build_terrain_texture(const map::Terrain& terrain,
                               TextureCache& tex_cache);

    /// Build minimap quads each frame.
    void update(const sim::SimState& sim, const Camera& camera,
                TextureCache& tex_cache,
                const std::unordered_set<u32>* selected_ids,
                u32 viewport_w, u32 viewport_h);

    /// Issue draw calls. Caller must have the UI pipeline bound.
    void render(VkCommandBuffer cmd, VkPipelineLayout layout,
                u32 viewport_w, u32 viewport_h);

    void destroy(VkDevice device, VmaAllocator allocator);

    void set_frame_index(u32 fi) { fi_ = fi; }

    u32 quad_count() const { return quad_count_; }

    /// Test if screen-space point (mx, my) is inside the minimap.
    /// If true, writes world coordinates to out_wx, out_wz.
    bool hit_test(f32 mx, f32 my, u32 viewport_w, u32 viewport_h,
                  f32 map_w, f32 map_h,
                  f32& out_wx, f32& out_wz) const;

    static constexpr u32 MINIMAP_SIZE = 200; // pixels
    static constexpr u32 MINIMAP_MARGIN = 10; // from bottom-left corner
    static constexpr u32 MINIMAP_TEX_SIZE = 256; // terrain texture resolution
    static constexpr u32 MAX_MINIMAP_QUADS = 2048;
    static constexpr u32 FRAMES_IN_FLIGHT = 2;

private:
    void emit_quad(f32 x, f32 y, f32 w, f32 h,
                   f32 r, f32 g, f32 b, f32 a,
                   VkDescriptorSet ds = VK_NULL_HANDLE);

    AllocatedBuffer instance_buf_[FRAMES_IN_FLIGHT] = {};
    void* instance_mapped_[FRAMES_IN_FLIGHT] = {};
    u32 fi_ = 0;

    std::vector<UIInstance> quads_;
    u32 quad_count_ = 0;

    // Terrain background texture descriptor
    VkDescriptorSet terrain_ds_ = VK_NULL_HANDLE;
    VkDescriptorSet white_ds_ = VK_NULL_HANDLE;

    // Cached map dimensions
    f32 map_w_ = 0;
    f32 map_h_ = 0;

    // Minimap screen rect (computed in update)
    f32 mm_x_ = 0, mm_y_ = 0; // top-left of minimap in screen coords
    f32 mm_size_ = 0; // actual rendered size (square)

    struct DrawGroup {
        VkDescriptorSet ds = VK_NULL_HANDLE;
        u32 offset = 0;
        u32 count = 0;
    };
    std::vector<DrawGroup> draw_groups_;
};

} // namespace osc::renderer
