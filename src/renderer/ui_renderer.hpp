#pragma once

#include "renderer/vk_types.hpp"
#include "renderer/texture_cache.hpp"
#include "renderer/font_cache.hpp"
#include "core/types.hpp"
#include "ui/ui_control.hpp"

#include <vulkan/vulkan.h>

#include <vector>

struct lua_State;

namespace osc::renderer {

/// Axis-aligned clip rectangle in pixel coordinates.
struct ClipRect {
    i32 x = 0;
    i32 y = 0;
    i32 w = 0;
    i32 h = 0;

    bool operator==(const ClipRect& o) const {
        return x == o.x && y == o.y && w == o.w && h == o.h;
    }
    bool operator!=(const ClipRect& o) const { return !(*this == o); }

    /// Intersect two clip rects. Returns a rect with w=0,h=0 if no overlap.
    static ClipRect intersect(const ClipRect& a, const ClipRect& b);
};

/// Per-instance GPU data for a UI quad.
struct UIInstance {
    f32 rect[4];   // x, y, w, h in pixels
    f32 uv[4];     // u0, v0, u1, v1
    f32 color[4];  // r, g, b, a
};

/// A batch of UI quads sharing the same texture descriptor and clip rect.
struct UIDrawGroup {
    VkDescriptorSet texture_ds = VK_NULL_HANDLE;
    ClipRect clip{};
    u32 instance_offset = 0;
    u32 instance_count = 0;
};

/// Walks the UI control tree, builds textured 2D quads, and renders them.
class UIRenderer {
public:
    void init(VkDevice device, VmaAllocator allocator);

    /// Walk all controls, read LazyVar positions from Lua, build quad list.
    void update(lua_State* L, const ui::UIControlRegistry& registry,
                TextureCache& tex_cache, FontCache& font_cache,
                u32 viewport_w, u32 viewport_h,
                f32 mouse_x = 0, f32 mouse_y = 0);

    /// Advance playing bitmap animations by delta_time seconds.
    void advance_animations(lua_State* L, const ui::UIControlRegistry& registry,
                            f32 delta_time);

    /// Issue draw calls for all UI quads. Caller must bind the UI pipeline first.
    void render(VkCommandBuffer cmd, VkPipelineLayout layout,
                u32 viewport_w, u32 viewport_h);

    void destroy(VkDevice device, VmaAllocator allocator);

    void set_frame_index(u32 fi) { fi_ = fi; }

    u32 quad_count() const { return quad_count_; }

    static constexpr u32 MAX_UI_QUADS = 2048;
    static constexpr u32 FRAMES_IN_FLIGHT = 2;

    /// Read a LazyVar float from a control's Lua table.
    static f32 read_lazyvar(lua_State* L, int table_idx, const char* field);

    /// Convert ARGB u32 color to float RGBA.
    static void argb_to_rgba(u32 argb, f32 out[4]);

    /// Intersect two clip rects (public for testing).
    static ClipRect intersect_clips(const ClipRect& a, const ClipRect& b) {
        return ClipRect::intersect(a, b);
    }

private:
    /// Collect quads by walking a control and its children recursively.
    void collect_control(lua_State* L, ui::UIControl* ctrl,
                         TextureCache& tex_cache, FontCache& font_cache,
                         u32 vp_w, u32 vp_h, const ClipRect& parent_clip);

    /// Emit glyph quads for a text control.
    void emit_text_quads(ui::UIControl* ctrl, FontCache& font_cache,
                         f32 left, f32 top, f32 width, f32 height, f32 depth,
                         const ClipRect& clip);

    /// Emit edit control quads: background, text, caret.
    void emit_edit_quads(ui::UIControl* ctrl, TextureCache& tex_cache,
                         FontCache& font_cache,
                         f32 left, f32 top, f32 width, f32 height, f32 depth,
                         const ClipRect& clip);

    /// Emit itemlist quads: multi-row text with selection highlight.
    void emit_itemlist_quads(ui::UIControl* ctrl, TextureCache& tex_cache,
                             FontCache& font_cache,
                             f32 left, f32 top, f32 width, f32 height,
                             f32 depth, const ClipRect& clip);

    /// Emit scrollbar quads: background + thumb textures.
    void emit_scrollbar_quads(lua_State* L, ui::UIControl* ctrl,
                              TextureCache& tex_cache,
                              f32 left, f32 top, f32 width, f32 height,
                              f32 depth, const ClipRect& clip);

    /// Emit cursor texture quad at mouse position (topmost depth).
    void emit_cursor_quad(lua_State* L, TextureCache& tex_cache,
                          u32 vp_w, u32 vp_h, const ClipRect& viewport_clip);

    AllocatedBuffer instance_buf_[FRAMES_IN_FLIGHT] = {};
    void* instance_mapped_[FRAMES_IN_FLIGHT] = {};
    u32 fi_ = 0;

    struct QuadEntry {
        UIInstance inst;
        VkDescriptorSet texture_ds = VK_NULL_HANDLE;
        ClipRect clip{};
        f32 depth = 0.0f;
    };
    std::vector<QuadEntry> quads_;
    std::vector<UIDrawGroup> groups_;
    u32 quad_count_ = 0;
    f32 mouse_x_ = 0;
    f32 mouse_y_ = 0;
};

} // namespace osc::renderer
