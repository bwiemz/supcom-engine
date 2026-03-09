#pragma once

#include "renderer/vk_types.hpp"
#include "renderer/ui_renderer.hpp" // UIInstance
#include "core/types.hpp"

#include <string>
#include <vector>

namespace osc::renderer {

class FontCache;
class TextureCache;

/// Renders a real-time performance profiling overlay in the top-right corner.
/// Shows per-zone timings (bars + text) and a frame time sparkline graph.
/// Uses the UI pipeline (UIInstance quads) with draw groups for texture switching.
class ProfileOverlay {
public:
    void init(VkDevice device, VmaAllocator allocator);

    /// Update overlay quads from profiler stats.
    void update(FontCache& font_cache, TextureCache& tex_cache,
                u32 viewport_w, u32 viewport_h);

    /// Issue draw calls. Caller must have the UI pipeline bound.
    void render(VkCommandBuffer cmd, VkPipelineLayout layout,
                u32 viewport_w, u32 viewport_h);

    void destroy(VkDevice device, VmaAllocator allocator);

    void set_frame_index(u32 fi) { fi_ = fi; }

    u32 quad_count() const { return quad_count_; }

    static constexpr u32 MAX_PROFILE_QUADS = 4096;
    static constexpr u32 FRAMES_IN_FLIGHT = 2;

private:
    struct DrawGroup {
        VkDescriptorSet ds = VK_NULL_HANDLE;
        u32 offset = 0;
        u32 count = 0;
    };

    void emit_quad(f32 x, f32 y, f32 w, f32 h,
                   f32 u0, f32 v0, f32 u1, f32 v1,
                   f32 r, f32 g, f32 b, f32 a);

    void emit_solid(f32 x, f32 y, f32 w, f32 h,
                    f32 r, f32 g, f32 b, f32 a);

    f32 emit_text(const std::string& text, f32 x, f32 y,
                  f32 r, f32 g, f32 b, f32 a,
                  FontCache& font_cache,
                  const std::string& font_family, i32 font_size);

    void begin_group(VkDescriptorSet ds);
    void end_group();

    AllocatedBuffer instance_buf_[FRAMES_IN_FLIGHT] = {};
    void* instance_mapped_[FRAMES_IN_FLIGHT] = {};
    u32 fi_ = 0;

    std::vector<UIInstance> quads_;
    std::vector<DrawGroup> groups_;
    u32 quad_count_ = 0;

    VkDescriptorSet current_ds_ = VK_NULL_HANDLE;
    u32 current_group_start_ = 0;

    static constexpr f32 PANEL_WIDTH = 340.0f;
    static constexpr f32 ROW_HEIGHT = 16.0f;
    static constexpr f32 MARGIN = 8.0f;
    static constexpr f32 BAR_MAX_WIDTH = 120.0f;
    static constexpr f32 GRAPH_HEIGHT = 50.0f;
    static constexpr i32 FONT_SIZE = 12;
};

} // namespace osc::renderer
