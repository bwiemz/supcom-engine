#pragma once

#include "renderer/vk_types.hpp"
#include "renderer/ui_renderer.hpp" // UIInstance, UIDrawGroup
#include "core/types.hpp"

#include <array>
#include <unordered_set>
#include <vector>

namespace osc::sim {
class SimState;
class Unit;
} // namespace osc::sim

namespace osc::renderer {

class Camera;
class TextureCache;

/// Icon types derived from unit categories.
enum class StrategicIconType : u8 {
    Land = 0,
    Air,
    Naval,
    Engineer,
    Commander,
    Structure,
    Generic,
    COUNT
};

/// Renders strategic zoom icons: when camera is zoomed out past a threshold,
/// units are replaced with 2D category icons in army colors.
class StrategicIconRenderer {
public:
    void init(VkDevice device, VmaAllocator allocator);

    /// Build the procedural icon atlas texture (called once after TextureCache ready).
    void build_atlas(TextureCache& tex_cache);

    /// Update icon quads from sim state. Returns true if strategic zoom is active.
    bool update(const sim::SimState& sim, const Camera& camera,
                const std::array<f32, 16>& vp_matrix,
                const std::unordered_set<u32>* selected_ids,
                TextureCache& tex_cache,
                u32 viewport_w, u32 viewport_h);

    /// Issue draw calls. Caller must have the UI pipeline bound.
    void render(VkCommandBuffer cmd, VkPipelineLayout layout,
                u32 viewport_w, u32 viewport_h);

    void destroy(VkDevice device, VmaAllocator allocator);

    void set_frame_index(u32 fi) { fi_ = fi; }

    u32 quad_count() const { return quad_count_; }
    bool is_strategic_zoom() const { return strategic_zoom_active_; }
    VkDescriptorSet atlas_descriptor() const { return atlas_ds_; }

    /// Camera distance threshold for switching to strategic icons.
    static constexpr f32 ZOOM_THRESHOLD = 250.0f;
    static constexpr u32 MAX_ICON_QUADS = 4096;
    static constexpr u32 FRAMES_IN_FLIGHT = 2;

    /// Classify a unit into an icon type based on its categories.
    static StrategicIconType classify_unit(const sim::Unit& unit);

    /// Atlas layout constants (public for reuse by SelectionInfoRenderer).
    static constexpr u32 ICON_CELL_SIZE = 32;
    static constexpr u32 ATLAS_COLS = static_cast<u32>(StrategicIconType::COUNT);
    static constexpr u32 ATLAS_W = ATLAS_COLS * ICON_CELL_SIZE; // 7 * 32 = 224
    static constexpr u32 ATLAS_H = ICON_CELL_SIZE;              // single row

private:
    /// Project world position to screen pixel coordinates.
    static bool world_to_screen(f32 wx, f32 wy, f32 wz,
                                const std::array<f32, 16>& vp,
                                f32 screen_w, f32 screen_h,
                                f32& out_x, f32& out_y);

    void emit_quad(f32 x, f32 y, f32 w, f32 h,
                   f32 u0, f32 v0, f32 u1, f32 v1,
                   f32 r, f32 g, f32 b, f32 a);

    /// Generate a single icon shape into pixel buffer.
    static void draw_icon_shape(u8* pixels, u32 atlas_w,
                                u32 cell_x, u32 cell_y, u32 cell_size,
                                StrategicIconType type);

    AllocatedBuffer instance_buf_[FRAMES_IN_FLIGHT] = {};
    void* instance_mapped_[FRAMES_IN_FLIGHT] = {};
    u32 fi_ = 0;

    std::vector<UIInstance> quads_;
    u32 quad_count_ = 0;

    VkDescriptorSet atlas_ds_ = VK_NULL_HANDLE;
    VkDescriptorSet white_ds_ = VK_NULL_HANDLE;
    u32 ring_count_ = 0;  // selection rings emitted first (use white_ds_)
    bool strategic_zoom_active_ = false;

};

} // namespace osc::renderer
