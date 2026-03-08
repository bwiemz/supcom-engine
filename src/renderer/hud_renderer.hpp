#pragma once

#include "renderer/vk_types.hpp"
#include "renderer/ui_renderer.hpp" // UIInstance
#include "core/types.hpp"

#include <string>
#include <vector>

namespace osc::sim {
class SimState;
}

namespace osc::renderer {

class FontCache;
class TextureCache;

/// Renders the in-game HUD: resource economy bars (mass/energy) at top of screen.
/// Uses the UI pipeline (UIInstance quads) with draw groups for texture switching.
class HudRenderer {
public:
    void init(VkDevice device, VmaAllocator allocator);

    /// Update HUD quads from sim state.
    /// player_army: 0-based index of the player's army.
    void update(const sim::SimState& sim, i32 player_army,
                FontCache& font_cache, TextureCache& tex_cache,
                u32 viewport_w, u32 viewport_h);

    /// Issue draw calls. Caller must have the UI pipeline bound.
    void render(VkCommandBuffer cmd, VkPipelineLayout layout,
                u32 viewport_w, u32 viewport_h);

    void destroy(VkDevice device, VmaAllocator allocator);

    u32 quad_count() const { return quad_count_; }

    static constexpr u32 MAX_HUD_QUADS = 2048;

private:
    struct DrawGroup {
        VkDescriptorSet ds = VK_NULL_HANDLE;
        u32 offset = 0;
        u32 count = 0;
    };

    void emit_quad(f32 x, f32 y, f32 w, f32 h,
                   f32 u0, f32 v0, f32 u1, f32 v1,
                   f32 r, f32 g, f32 b, f32 a);

    /// Emit solid-colored quad (white texture, full UV).
    void emit_solid(f32 x, f32 y, f32 w, f32 h,
                    f32 r, f32 g, f32 b, f32 a);

    /// Emit text glyphs. Returns total width in pixels.
    /// Caller must set current_ds_ to the font atlas DS before calling.
    f32 emit_text(const std::string& text, f32 x, f32 y,
                  f32 r, f32 g, f32 b, f32 a,
                  FontCache& font_cache,
                  const std::string& font_family, i32 font_size);

    /// Start a new draw group with the given descriptor set.
    void begin_group(VkDescriptorSet ds);
    /// End current draw group.
    void end_group();

    AllocatedBuffer instance_buf_{};
    void* instance_mapped_ = nullptr;

    std::vector<UIInstance> quads_;
    std::vector<DrawGroup> groups_;
    u32 quad_count_ = 0;

    VkDescriptorSet current_ds_ = VK_NULL_HANDLE;
    u32 current_group_start_ = 0;

    static constexpr f32 BAR_WIDTH = 300.0f;
    static constexpr f32 BAR_HEIGHT = 16.0f;
    static constexpr f32 BAR_Y = 8.0f;
    static constexpr f32 BAR_SPACING = 8.0f;
    static constexpr i32 FONT_SIZE = 14;
};

} // namespace osc::renderer
