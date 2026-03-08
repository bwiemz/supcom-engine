#pragma once

#include "renderer/vk_types.hpp"
#include "renderer/ui_renderer.hpp" // UIInstance, UIDrawGroup, ClipRect
#include "core/types.hpp"

#include <array>
#include <unordered_set>
#include <vector>

namespace osc::sim {
class SimState;
}

namespace osc::renderer {

class Camera;
class TextureCache;

/// Renders game overlays: health bars, selection rings, command lines.
/// Uses the same UI pipeline (UIInstance quads, pixel coords, fallback white texture).
class OverlayRenderer {
public:
    void init(VkDevice device, VmaAllocator allocator);

    /// Build overlay quads from sim state + selection + camera.
    /// game_result: 0=in progress, 1=victory, 2=defeat, 3=draw.
    void update(sim::SimState& sim, const Camera& camera,
                const std::array<f32, 16>& vp_matrix,
                const std::unordered_set<u32>* selected_ids,
                TextureCache& tex_cache,
                u32 viewport_w, u32 viewport_h,
                i32 game_result = 0, f32 dt = 0.0f);

    /// Issue draw calls. Caller must have the UI pipeline bound.
    void render(VkCommandBuffer cmd, VkPipelineLayout layout,
                u32 viewport_w, u32 viewport_h);

    void destroy(VkDevice device, VmaAllocator allocator);

    u32 quad_count() const { return quad_count_; }

    static constexpr u32 MAX_OVERLAY_QUADS = 8192;

private:
    /// Project world position to screen pixel coordinates.
    /// Returns false if behind camera.
    static bool world_to_screen(f32 wx, f32 wy, f32 wz,
                                const std::array<f32, 16>& vp,
                                f32 screen_w, f32 screen_h,
                                f32& out_x, f32& out_y);

    void emit_quad(f32 x, f32 y, f32 w, f32 h,
                   f32 r, f32 g, f32 b, f32 a);

    AllocatedBuffer instance_buf_{};
    void* instance_mapped_ = nullptr;

    std::vector<UIInstance> quads_;
    u32 quad_count_ = 0;

    VkDescriptorSet white_ds_ = VK_NULL_HANDLE;

    // Active explosion effects (from death events)
    struct Explosion {
        f32 x, y, z;     // world position
        f32 scale;        // max radius
        f32 age;          // seconds since death (0..EXPLOSION_DURATION)
        f32 r, g, b;     // flash color
    };
    static constexpr f32 EXPLOSION_DURATION = 0.6f;
    static constexpr u32 MAX_EXPLOSIONS = 64;
    std::vector<Explosion> explosions_;
    f32 last_dt_ = 0.0f;
};

} // namespace osc::renderer
