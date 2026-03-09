#pragma once

#include "renderer/vk_types.hpp"
#include "renderer/ui_renderer.hpp" // UIInstance
#include "renderer/strategic_icon_renderer.hpp" // StrategicIconType, classify_unit
#include "core/types.hpp"

#include <string>
#include <unordered_set>
#include <vector>

namespace osc::sim {
class SimState;
class Unit;
}

namespace osc::renderer {

class FontCache;
class TextureCache;

/// Renders a selection info panel at the bottom-center of the screen.
/// Single unit: name, HP bar, build progress, current command.
/// Multi-unit: count + type breakdown grid with strategic icons.
class SelectionInfoRenderer {
public:
    void init(VkDevice device, VmaAllocator allocator);

    void update(const sim::SimState& sim,
                const std::unordered_set<u32>* selected_ids,
                FontCache& font_cache, TextureCache& tex_cache,
                VkDescriptorSet icon_atlas_ds,
                u32 viewport_w, u32 viewport_h);

    void render(VkCommandBuffer cmd, VkPipelineLayout layout,
                u32 viewport_w, u32 viewport_h);

    void destroy(VkDevice device, VmaAllocator allocator);

    void set_frame_index(u32 fi) { fi_ = fi; }

    u32 quad_count() const { return quad_count_; }

    static constexpr u32 MAX_INFO_QUADS = 2048;
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

    void build_single_unit(const sim::Unit& unit,
                           FontCache& font_cache, TextureCache& tex_cache,
                           VkDescriptorSet icon_atlas_ds,
                           f32 panel_x, f32 panel_y, f32 panel_w, f32 panel_h);

    void build_multi_unit(const sim::SimState& sim,
                          const std::unordered_set<u32>& selected_ids,
                          FontCache& font_cache, TextureCache& tex_cache,
                          VkDescriptorSet icon_atlas_ds,
                          f32 panel_x, f32 panel_y, f32 panel_w, f32 panel_h);

    AllocatedBuffer instance_buf_[FRAMES_IN_FLIGHT] = {};
    void* instance_mapped_[FRAMES_IN_FLIGHT] = {};
    u32 fi_ = 0;

    std::vector<UIInstance> quads_;
    std::vector<DrawGroup> groups_;
    u32 quad_count_ = 0;

    VkDescriptorSet current_ds_ = VK_NULL_HANDLE;
    u32 current_group_start_ = 0;

    static constexpr f32 PANEL_WIDTH = 360.0f;
    static constexpr f32 PANEL_HEIGHT_SINGLE = 100.0f;
    static constexpr f32 PANEL_HEIGHT_MULTI = 120.0f;
    static constexpr f32 PANEL_MARGIN_BOTTOM = 10.0f;
    static constexpr i32 FONT_SIZE = 13;
    static constexpr i32 FONT_SIZE_SMALL = 11;
};

} // namespace osc::renderer
