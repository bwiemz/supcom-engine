#pragma once

#include "renderer/vk_types.hpp"
#include "renderer/ui_renderer.hpp" // UIInstance, UIDrawGroup, ClipRect
#include "renderer/frustum.hpp"
#include "core/types.hpp"

#include <array>
#include <string>
#include <unordered_set>
#include <vector>

namespace osc::sim {
class FrameView;
struct WorldEvents;
}

namespace osc::renderer {

class Camera;
class TextureCache;

/// Every intel type a selected unit can show a range ring for.
inline const std::unordered_set<std::string> kAllIntelRingTypes{"Radar", "Sonar", "Omni",
                                                                 "Vision"};

/// Intel types to show rings for, given FA's active range-overlay filters:
/// the RangeOverlayParams names multifunction.lua passes to
/// SetOverlayFilters ("Radar", "Sonar", "Omni", or "AllIntel" for all
/// three). Military and counter-intel filters have no ring here.
std::unordered_set<std::string> intel_ring_types_for_filters(
    const std::vector<std::string>& filters);

/// Renders game overlays: health bars, selection rings, command lines.
/// Uses the same UI pipeline (UIInstance quads, pixel coords, fallback white texture).
class OverlayRenderer {
public:
    void init(VkDevice device, VmaAllocator allocator);

    /// Build overlay quads from the world as `view` draws it (between the
    /// last two ticks), the selection and the camera. Death flashes come
    /// from `events`, which this takes.
    /// game_result: 0=in progress, 1=victory, 2=defeat, 3=draw.
    void update(const sim::FrameView& view, sim::WorldEvents& events, const Camera& camera,
                const std::array<f32, 16>& vp_matrix,
                const std::unordered_set<u32>* selected_ids,
                TextureCache& tex_cache,
                u32 viewport_w, u32 viewport_h,
                i32 game_result = 0, f32 dt = 0.0f,
                const Frustum* frustum = nullptr);

    /// Issue draw calls. Caller must have the UI pipeline bound.
    void render(VkCommandBuffer cmd, VkPipelineLayout layout,
                u32 viewport_w, u32 viewport_h);

    void destroy(VkDevice device, VmaAllocator allocator);

    void set_frame_index(u32 fi) { fi_ = fi; }
    /// This frame's quads (the render-state dump reads them).
    const std::vector<UIInstance>& quads() const { return quads_; }

    /// Intel types whose range rings selected units show ("Radar", "Sonar",
    /// "Omni", "Vision"). FA shows them per its range-overlay filters; the
    /// C++ HUD (no FA game UI, or --legacy-hud) shows them all.
    void set_intel_ring_types(std::unordered_set<std::string> types) {
        intel_ring_types_ = std::move(types);
    }

    u32 quad_count() const { return quad_count_; }

    static constexpr u32 MAX_OVERLAY_QUADS = 8192;
    static constexpr u32 FRAMES_IN_FLIGHT = 2;

private:
    std::unordered_set<std::string> intel_ring_types_ = kAllIntelRingTypes;
    /// Project world position to screen pixel coordinates.
    /// Returns false if behind camera.
    static bool world_to_screen(f32 wx, f32 wy, f32 wz,
                                const std::array<f32, 16>& vp,
                                f32 screen_w, f32 screen_h,
                                f32& out_x, f32& out_y);

    void emit_quad(f32 x, f32 y, f32 w, f32 h,
                   f32 r, f32 g, f32 b, f32 a);

    AllocatedBuffer instance_buf_[FRAMES_IN_FLIGHT] = {};
    void* instance_mapped_[FRAMES_IN_FLIGHT] = {};
    u32 fi_ = 0;

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
