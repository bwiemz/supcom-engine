#include "renderer/ui_renderer.hpp"

#include <spdlog/spdlog.h>
#include <vk_mem_alloc.h>

#include <algorithm>
#include <cstring>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

namespace osc::renderer {

ClipRect ClipRect::intersect(const ClipRect& a, const ClipRect& b) {
    i32 x0 = std::max(a.x, b.x);
    i32 y0 = std::max(a.y, b.y);
    i32 x1 = std::min(a.x + a.w, b.x + b.w);
    i32 y1 = std::min(a.y + a.h, b.y + b.h);
    if (x1 <= x0 || y1 <= y0) return {0, 0, 0, 0};
    return {x0, y0, x1 - x0, y1 - y0};
}

void UIRenderer::init(VkDevice device, VmaAllocator allocator) {
    // Create persistently-mapped instance buffer for UI quads
    VkBufferCreateInfo buf_info{};
    buf_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buf_info.size = sizeof(UIInstance) * MAX_UI_QUADS;
    buf_info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;

    VmaAllocationCreateInfo alloc_info{};
    alloc_info.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
    alloc_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                       VMA_ALLOCATION_CREATE_MAPPED_BIT;

    VmaAllocationInfo result_info{};
    if (vmaCreateBuffer(allocator, &buf_info, &alloc_info,
                        &instance_buf_.buffer, &instance_buf_.allocation,
                        &result_info) != VK_SUCCESS) {
        spdlog::error("UIRenderer: failed to create instance buffer");
        return;
    }
    instance_mapped_ = result_info.pMappedData;
}

f32 UIRenderer::read_lazyvar(lua_State* L, int table_idx, const char* field) {
    // Normalize index to absolute
    if (table_idx < 0) table_idx = lua_gettop(L) + table_idx + 1;

    lua_pushstring(L, field);
    lua_rawget(L, table_idx); // get LazyVar table
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return 0.0f;
    }

    // LazyVar uses __call metamethod: lazyvar() returns the value.
    lua_pushvalue(L, -1); // push LazyVar table as the function to call
    if (lua_pcall(L, 0, 1, 0) != 0) {
        lua_pop(L, 2); // error msg + LazyVar table
        return 0.0f;
    }

    f32 val = static_cast<f32>(lua_tonumber(L, -1));
    lua_pop(L, 2); // result + LazyVar table
    return val;
}

void UIRenderer::argb_to_rgba(u32 argb, f32 out[4]) {
    out[0] = static_cast<f32>((argb >> 16) & 0xFF) / 255.0f; // R
    out[1] = static_cast<f32>((argb >> 8) & 0xFF) / 255.0f;  // G
    out[2] = static_cast<f32>((argb >> 0) & 0xFF) / 255.0f;  // B
    out[3] = static_cast<f32>((argb >> 24) & 0xFF) / 255.0f;  // A
}

void UIRenderer::emit_text_quads(ui::UIControl* ctrl, FontCache& font_cache,
                                  f32 left, f32 top, f32 width, f32 height,
                                  f32 depth, const ClipRect& clip) {
    const auto& text = ctrl->text_content();
    if (text.empty()) return;

    const FontAtlas* atlas = font_cache.get(ctrl->font_family(),
                                             ctrl->font_pointsize());
    if (!atlas || atlas->descriptor_set == VK_NULL_HANDLE) return;

    // Compute text color
    f32 color[4];
    argb_to_rgba(ctrl->text_color(), color);
    color[3] *= ctrl->alpha();

    // Starting position
    f32 cursor_x = left;
    f32 baseline_y = top + atlas->metrics.ascent;

    // Vertical centering
    if (ctrl->centered_vertically()) {
        f32 text_height = atlas->metrics.ascent + atlas->metrics.descent;
        baseline_y = top + (height - text_height) * 0.5f + atlas->metrics.ascent;
    }

    // Compute total advance for horizontal centering
    f32 total_advance = 0.0f;
    if (ctrl->centered_horizontally()) {
        total_advance = font_cache.string_advance(ctrl->font_family(),
                                                   ctrl->font_pointsize(),
                                                   text);
        cursor_x = left + (width - total_advance) * 0.5f;
    }

    // Emit one quad per glyph
    for (unsigned char c : text) {
        if (quad_count_ >= MAX_UI_QUADS) break;

        auto git = atlas->glyphs.find(static_cast<u32>(c));
        if (git == atlas->glyphs.end()) {
            // Unknown glyph — advance by space width
            auto space = atlas->glyphs.find(32);
            if (space != atlas->glyphs.end())
                cursor_x += space->second.x_advance;
            else
                cursor_x += static_cast<f32>(ctrl->font_pointsize()) * 0.6f;
            continue;
        }

        const auto& gi = git->second;

        // Skip spaces (no visible glyph)
        if (gi.width > 0 && gi.height > 0) {
            // Clip to control bounds
            f32 gx = cursor_x + gi.x_offset;
            f32 gy = baseline_y + gi.y_offset;

            if (ctrl->clip_to_width() && gx + gi.width > left + width) break;

            QuadEntry entry{};
            entry.texture_ds = atlas->descriptor_set;
            entry.clip = clip;
            entry.inst.rect[0] = gx;
            entry.inst.rect[1] = gy;
            entry.inst.rect[2] = gi.width;
            entry.inst.rect[3] = gi.height;
            entry.inst.uv[0] = gi.u0;
            entry.inst.uv[1] = gi.v0;
            entry.inst.uv[2] = gi.u1;
            entry.inst.uv[3] = gi.v1;
            std::memcpy(entry.inst.color, color, sizeof(color));
            entry.depth = depth + 0.01f; // text slightly above background

            // Drop shadow
            if (ctrl->drop_shadow()) {
                if (quad_count_ < MAX_UI_QUADS) {
                    QuadEntry shadow = entry;
                    shadow.inst.rect[0] += 1.0f;
                    shadow.inst.rect[1] += 1.0f;
                    shadow.inst.color[0] = 0.0f;
                    shadow.inst.color[1] = 0.0f;
                    shadow.inst.color[2] = 0.0f;
                    shadow.inst.color[3] = color[3] * 0.7f;
                    shadow.depth = depth + 0.005f; // shadow behind text
                    quads_.push_back(shadow);
                    quad_count_++;
                }
            }

            quads_.push_back(entry);
            quad_count_++;
        }

        cursor_x += gi.x_advance;
    }
}

void UIRenderer::emit_edit_quads(ui::UIControl* ctrl, TextureCache& tex_cache,
                                  FontCache& font_cache,
                                  f32 left, f32 top, f32 width, f32 height,
                                  f32 depth, const ClipRect& clip) {
    f32 alpha = ctrl->alpha();
    f32 full_uv[4] = {0.0f, 0.0f, 1.0f, 1.0f};

    // 1. Background rect
    if (ctrl->bg_visible() && quad_count_ < MAX_UI_QUADS) {
        QuadEntry bg{};
        bg.texture_ds = tex_cache.fallback_descriptor();
        bg.clip = clip;
        bg.depth = depth - 0.001f;
        bg.inst.rect[0] = left; bg.inst.rect[1] = top;
        bg.inst.rect[2] = width; bg.inst.rect[3] = height;
        std::memcpy(bg.inst.uv, full_uv, sizeof(full_uv));
        argb_to_rgba(ctrl->background_color(), bg.inst.color);
        bg.inst.color[3] *= alpha;
        quads_.push_back(bg);
        quad_count_++;
    }

    // 2. Text content (uses foreground color)
    const auto& text = ctrl->text_content();
    if (!text.empty()) {
        // Temporarily override text_color for rendering
        u32 saved_color = ctrl->text_color();
        ctrl->set_text_color(ctrl->foreground_color());
        emit_text_quads(ctrl, font_cache, left, top, width, height, depth, clip);
        ctrl->set_text_color(saved_color);
    }

    // 3. Caret (blinking vertical line)
    if (ctrl->caret_visible() && ctrl->input_enabled() && quad_count_ < MAX_UI_QUADS) {
        const FontAtlas* atlas = font_cache.get(ctrl->font_family(),
                                                 ctrl->font_pointsize());
        f32 caret_x = left + 2.0f; // small left padding
        if (atlas) {
            // Advance cursor_x through glyphs up to caret_position
            i32 pos = ctrl->caret_position();
            for (i32 i = 0; i < pos && i < static_cast<i32>(text.size()); i++) {
                auto git = atlas->glyphs.find(static_cast<u32>(
                    static_cast<unsigned char>(text[i])));
                if (git != atlas->glyphs.end()) {
                    caret_x += git->second.x_advance;
                } else {
                    caret_x += static_cast<f32>(ctrl->font_pointsize()) * 0.6f;
                }
            }
        }

        f32 caret_w = 2.0f;
        f32 caret_h = height - 4.0f;
        if (caret_h < 4.0f) caret_h = height;
        f32 caret_y = top + 2.0f;

        QuadEntry caret{};
        caret.texture_ds = tex_cache.fallback_descriptor();
        caret.clip = clip;
        caret.depth = depth + 0.02f; // caret on top of text
        caret.inst.rect[0] = caret_x; caret.inst.rect[1] = caret_y;
        caret.inst.rect[2] = caret_w; caret.inst.rect[3] = caret_h;
        std::memcpy(caret.inst.uv, full_uv, sizeof(full_uv));
        argb_to_rgba(ctrl->caret_color(), caret.inst.color);
        caret.inst.color[3] *= alpha;
        quads_.push_back(caret);
        quad_count_++;
    }
}

void UIRenderer::emit_itemlist_quads(ui::UIControl* ctrl,
                                      TextureCache& tex_cache,
                                      FontCache& font_cache,
                                      f32 left, f32 top, f32 width, f32 height,
                                      f32 depth, const ClipRect& clip) {
    const auto& items = ctrl->items();
    if (items.empty()) return;

    const FontAtlas* atlas = font_cache.get(ctrl->font_family(),
                                             ctrl->font_pointsize());
    f32 row_height = static_cast<f32>(ctrl->font_pointsize()) + 4.0f;
    if (atlas) row_height = atlas->metrics.ascent + atlas->metrics.descent + 4.0f;

    f32 alpha = ctrl->alpha();
    f32 full_uv[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    i32 scroll_top = ctrl->scroll_top();
    i32 visible_rows = static_cast<i32>(height / row_height);
    if (visible_rows < 1) visible_rows = 1;
    i32 end_row = std::min(scroll_top + visible_rows,
                            static_cast<i32>(items.size()));

    // Background
    if (quad_count_ < MAX_UI_QUADS) {
        QuadEntry bg{};
        bg.texture_ds = tex_cache.fallback_descriptor();
        bg.clip = clip;
        bg.depth = depth - 0.001f;
        bg.inst.rect[0] = left; bg.inst.rect[1] = top;
        bg.inst.rect[2] = width; bg.inst.rect[3] = height;
        std::memcpy(bg.inst.uv, full_uv, sizeof(full_uv));
        argb_to_rgba(ctrl->item_bg_color(), bg.inst.color);
        bg.inst.color[3] *= alpha;
        quads_.push_back(bg);
        quad_count_++;
    }

    for (i32 i = scroll_top; i < end_row && quad_count_ < MAX_UI_QUADS; i++) {
        f32 row_y = top + static_cast<f32>(i - scroll_top) * row_height;

        // Selection highlight row
        if (i == ctrl->selection() && ctrl->show_selection()) {
            if (quad_count_ < MAX_UI_QUADS) {
                QuadEntry sel{};
                sel.texture_ds = tex_cache.fallback_descriptor();
                sel.clip = clip;
                sel.depth = depth;
                sel.inst.rect[0] = left; sel.inst.rect[1] = row_y;
                sel.inst.rect[2] = width; sel.inst.rect[3] = row_height;
                std::memcpy(sel.inst.uv, full_uv, sizeof(full_uv));
                argb_to_rgba(ctrl->item_sel_bg_color(), sel.inst.color);
                sel.inst.color[3] *= alpha;
                quads_.push_back(sel);
                quad_count_++;
            }
        }

        // Row text — emit glyphs directly
        if (!items[i].empty() && atlas && atlas->descriptor_set != VK_NULL_HANDLE) {
            u32 text_color = (i == ctrl->selection() && ctrl->show_selection())
                ? ctrl->item_sel_fg_color() : ctrl->item_fg_color();
            f32 color[4];
            argb_to_rgba(text_color, color);
            color[3] *= alpha;

            f32 baseline_y = row_y + (atlas ? atlas->metrics.ascent : row_height * 0.8f);
            f32 cursor_x = left + 2.0f;

            for (unsigned char c : items[i]) {
                if (quad_count_ >= MAX_UI_QUADS) break;
                auto git = atlas->glyphs.find(static_cast<u32>(c));
                if (git == atlas->glyphs.end()) {
                    auto sp = atlas->glyphs.find(32);
                    cursor_x += sp != atlas->glyphs.end()
                        ? sp->second.x_advance
                        : static_cast<f32>(ctrl->font_pointsize()) * 0.6f;
                    continue;
                }
                const auto& gi = git->second;
                if (gi.width > 0 && gi.height > 0) {
                    f32 gx = cursor_x + gi.x_offset;
                    f32 gy = baseline_y + gi.y_offset;
                    if (gx + gi.width > left + width) break;

                    QuadEntry glyph{};
                    glyph.texture_ds = atlas->descriptor_set;
                    glyph.clip = clip;
                    glyph.depth = depth + 0.01f;
                    glyph.inst.rect[0] = gx; glyph.inst.rect[1] = gy;
                    glyph.inst.rect[2] = gi.width; glyph.inst.rect[3] = gi.height;
                    glyph.inst.uv[0] = gi.u0; glyph.inst.uv[1] = gi.v0;
                    glyph.inst.uv[2] = gi.u1; glyph.inst.uv[3] = gi.v1;
                    std::memcpy(glyph.inst.color, color, sizeof(color));
                    quads_.push_back(glyph);
                    quad_count_++;
                }
                cursor_x += gi.x_advance;
            }
        }
    }
}

void UIRenderer::emit_scrollbar_quads(lua_State* L, ui::UIControl* ctrl,
                                       TextureCache& tex_cache,
                                       f32 left, f32 top, f32 width, f32 height,
                                       f32 depth, const ClipRect& clip) {
    f32 alpha = ctrl->alpha();
    f32 full_uv[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    f32 white[4] = {1.0f, 1.0f, 1.0f, alpha};
    bool is_vert = (ctrl->scroll_axis() == "Vert");

    // Background texture
    if (!ctrl->sb_bg_texture().empty() && quad_count_ < MAX_UI_QUADS) {
        const GPUTexture* tex = tex_cache.get(ctrl->sb_bg_texture());
        if (tex) {
            QuadEntry bg{};
            bg.texture_ds = tex->descriptor_set;
            bg.clip = clip;
            bg.depth = depth - 0.001f;
            bg.inst.rect[0] = left; bg.inst.rect[1] = top;
            bg.inst.rect[2] = width; bg.inst.rect[3] = height;
            std::memcpy(bg.inst.uv, full_uv, sizeof(full_uv));
            std::memcpy(bg.inst.color, white, sizeof(white));
            quads_.push_back(bg);
            quad_count_++;
        }
    }

    // Query scrollable for scroll position (range_min, range_max, visible, top)
    f32 range_min = 0, range_max = 1, visible = 1, scroll_pos = 0;
    int sref = ctrl->scrollable_ref();
    if (sref >= 0) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, sref);
        if (lua_istable(L, -1)) {
            lua_pushstring(L, "GetScrollValues");
            lua_gettable(L, -2);
            if (lua_isfunction(L, -1)) {
                lua_pushvalue(L, -2); // self
                lua_pushstring(L, ctrl->scroll_axis().c_str());
                if (lua_pcall(L, 2, 4, 0) == 0) {
                    range_min = static_cast<f32>(lua_tonumber(L, -4));
                    range_max = static_cast<f32>(lua_tonumber(L, -3));
                    visible = static_cast<f32>(lua_tonumber(L, -2));
                    scroll_pos = static_cast<f32>(lua_tonumber(L, -1));
                    lua_pop(L, 4);
                } else {
                    lua_pop(L, 1); // error
                }
            } else {
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1); // scrollable table
    }

    // Compute thumb position and size
    f32 range = range_max - range_min;
    if (range <= 0) range = 1;
    f32 thumb_frac = std::min(visible / range, 1.0f);
    f32 pos_frac = (scroll_pos - range_min) / range;

    f32 track_len = is_vert ? height : width;
    f32 thumb_len = std::max(thumb_frac * track_len, 16.0f);
    f32 thumb_pos = pos_frac * (track_len - thumb_len);

    // Thumb middle texture
    if (!ctrl->sb_thumb_mid().empty() && quad_count_ < MAX_UI_QUADS) {
        const GPUTexture* tex = tex_cache.get(ctrl->sb_thumb_mid());
        if (tex) {
            QuadEntry thumb{};
            thumb.texture_ds = tex->descriptor_set;
            thumb.clip = clip;
            thumb.depth = depth;
            if (is_vert) {
                thumb.inst.rect[0] = left;
                thumb.inst.rect[1] = top + thumb_pos;
                thumb.inst.rect[2] = width;
                thumb.inst.rect[3] = thumb_len;
            } else {
                thumb.inst.rect[0] = left + thumb_pos;
                thumb.inst.rect[1] = top;
                thumb.inst.rect[2] = thumb_len;
                thumb.inst.rect[3] = height;
            }
            std::memcpy(thumb.inst.uv, full_uv, sizeof(full_uv));
            std::memcpy(thumb.inst.color, white, sizeof(white));
            quads_.push_back(thumb);
            quad_count_++;
        }
    }
}

void UIRenderer::collect_control(lua_State* L, ui::UIControl* ctrl,
                                 TextureCache& tex_cache,
                                 FontCache& font_cache,
                                 u32 vp_w, u32 vp_h,
                                 const ClipRect& parent_clip) {
    if (!ctrl || ctrl->destroyed() || ctrl->hidden()) return;
    if (quad_count_ >= MAX_UI_QUADS) return;

    // Get the control's Lua table
    int ref = ctrl->lua_table_ref();
    if (ref < 0) return;
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return;
    }

    int tbl_idx = lua_gettop(L);

    // Read layout LazyVars
    f32 left = read_lazyvar(L, tbl_idx, "Left");
    f32 top = read_lazyvar(L, tbl_idx, "Top");
    f32 width = read_lazyvar(L, tbl_idx, "Width");
    f32 height = read_lazyvar(L, tbl_idx, "Height");
    f32 depth = read_lazyvar(L, tbl_idx, "Depth");

    // Compute this control's clip rect: intersect its bounds with parent's clip
    ClipRect self_clip = ClipRect::intersect(
        parent_clip,
        {static_cast<i32>(left), static_cast<i32>(top),
         static_cast<i32>(width), static_cast<i32>(height)});

    // Skip entirely if clipped away
    if (self_clip.w <= 0 || self_clip.h <= 0) {
        lua_pop(L, 1);
        return;
    }

    // Only render controls with valid dimensions
    bool has_visual = false;
    QuadEntry entry{};

    if (width > 0 && height > 0) {
        // Determine what to render
        if (!ctrl->texture_path().empty()) {
            // Textured bitmap — select frame texture for animations
            std::string tex_path = ctrl->texture_path();
            if (ctrl->textures().size() > 1) {
                i32 frame = ctrl->current_frame();
                if (!ctrl->frame_pattern().empty()) {
                    i32 pi = ctrl->pattern_index();
                    i32 ps = static_cast<i32>(ctrl->frame_pattern().size());
                    if (pi >= 0 && pi < ps)
                        frame = ctrl->frame_pattern()[pi];
                }
                if (frame >= 0 && frame < static_cast<i32>(ctrl->textures().size()))
                    tex_path = ctrl->textures()[frame];
            }
            const GPUTexture* tex = tex_cache.get(tex_path);
            if (tex) {
                entry.texture_ds = tex->descriptor_set;
                f32 cm[4];
                argb_to_rgba(ctrl->color_mask(), cm);
                cm[3] *= ctrl->alpha();
                std::memcpy(entry.inst.color, cm, sizeof(cm));
                if (ctrl->tiled() && ctrl->bitmap_width() > 0 && ctrl->bitmap_height() > 0) {
                    entry.inst.uv[0] = 0.0f;
                    entry.inst.uv[1] = 0.0f;
                    entry.inst.uv[2] = width / static_cast<f32>(ctrl->bitmap_width());
                    entry.inst.uv[3] = height / static_cast<f32>(ctrl->bitmap_height());
                } else {
                    entry.inst.uv[0] = ctrl->uv_u0();
                    entry.inst.uv[1] = ctrl->uv_v0();
                    entry.inst.uv[2] = ctrl->uv_u1();
                    entry.inst.uv[3] = ctrl->uv_v1();
                }
                has_visual = true;
            }
        } else if (ctrl->has_solid_color()) {
            // Solid color — use white fallback texture
            entry.texture_ds = tex_cache.fallback_descriptor();
            argb_to_rgba(ctrl->solid_color(), entry.inst.color);
            entry.inst.color[3] *= ctrl->alpha();
            entry.inst.uv[0] = 0.0f; entry.inst.uv[1] = 0.0f;
            entry.inst.uv[2] = 1.0f; entry.inst.uv[3] = 1.0f;
            has_visual = true;
        }

        // Border rendering: solid color fill OR 8-piece ninepatch
        if (ctrl->has_border_solid_color() && !has_visual) {
            entry.texture_ds = tex_cache.fallback_descriptor();
            argb_to_rgba(ctrl->border_solid_color(), entry.inst.color);
            entry.inst.color[3] *= ctrl->alpha();
            entry.inst.uv[0] = 0.0f; entry.inst.uv[1] = 0.0f;
            entry.inst.uv[2] = 1.0f; entry.inst.uv[3] = 1.0f;
            has_visual = true;
        }

        // 8-piece border: 4 corners + 4 edges
        if (!ctrl->border_tex_ul().empty()) {
            f32 bw = read_lazyvar(L, tbl_idx, "BorderWidth");
            f32 bh = read_lazyvar(L, tbl_idx, "BorderHeight");
            if (bw > 0 && bh > 0) {
                f32 alpha = ctrl->alpha();
                f32 white[4] = {1.0f, 1.0f, 1.0f, alpha};
                f32 full_uv[4] = {0.0f, 0.0f, 1.0f, 1.0f};

                auto emit_border_quad = [&](const std::string& tex_path,
                                            f32 qx, f32 qy, f32 qw, f32 qh) {
                    if (quad_count_ >= MAX_UI_QUADS || tex_path.empty()) return;
                    const GPUTexture* tex = tex_cache.get(tex_path);
                    if (!tex) return;
                    QuadEntry q{};
                    q.texture_ds = tex->descriptor_set;
                    q.clip = parent_clip;
                    q.depth = depth - 0.001f; // border behind content
                    q.inst.rect[0] = qx; q.inst.rect[1] = qy;
                    q.inst.rect[2] = qw; q.inst.rect[3] = qh;
                    std::memcpy(q.inst.uv, full_uv, sizeof(full_uv));
                    std::memcpy(q.inst.color, white, sizeof(white));
                    quads_.push_back(q);
                    quad_count_++;
                };

                f32 inner_w = width - 2.0f * bw;
                f32 inner_h = height - 2.0f * bh;
                if (inner_w < 0) inner_w = 0;
                if (inner_h < 0) inner_h = 0;

                // Corners
                emit_border_quad(ctrl->border_tex_ul(), left,              top,               bw, bh);
                emit_border_quad(ctrl->border_tex_ur(), left + bw + inner_w, top,             bw, bh);
                emit_border_quad(ctrl->border_tex_ll(), left,              top + bh + inner_h, bw, bh);
                emit_border_quad(ctrl->border_tex_lr(), left + bw + inner_w, top + bh + inner_h, bw, bh);
                // Edges
                emit_border_quad(ctrl->border_tex_horiz(), left + bw, top,               inner_w, bh); // top
                emit_border_quad(ctrl->border_tex_horiz(), left + bw, top + bh + inner_h, inner_w, bh); // bottom
                emit_border_quad(ctrl->border_tex_vert(),  left,      top + bh,           bw, inner_h); // left
                emit_border_quad(ctrl->border_tex_vert(),  left + bw + inner_w, top + bh, bw, inner_h); // right
            }
        }

        if (has_visual) {
            entry.inst.rect[0] = left;
            entry.inst.rect[1] = top;
            entry.inst.rect[2] = width;
            entry.inst.rect[3] = height;
            entry.clip = parent_clip;
            entry.depth = depth;
            quads_.push_back(entry);
            quad_count_++;
        }

        // Edit control: background + text + caret
        if (ctrl->control_type() == ui::UIControl::ControlType::Edit) {
            emit_edit_quads(ctrl, tex_cache, font_cache, left, top, width,
                            height, depth, parent_clip);
        }
        // ItemList control: multi-row text + selection highlight
        else if (ctrl->control_type() == ui::UIControl::ControlType::ItemList) {
            emit_itemlist_quads(ctrl, tex_cache, font_cache, left, top, width,
                                height, depth, parent_clip);
        }
        // Scrollbar control: background + thumb
        else if (ctrl->control_type() == ui::UIControl::ControlType::Scrollbar) {
            emit_scrollbar_quads(L, ctrl, tex_cache, left, top, width, height,
                                  depth, parent_clip);
        }
        // Regular text quads (for non-edit controls with text content)
        else if (!ctrl->text_content().empty()) {
            emit_text_quads(ctrl, font_cache, left, top, width, height, depth,
                            parent_clip);
        }
    }

    lua_pop(L, 1); // pop Lua table

    // Recurse into children — children are clipped to this control's bounds
    for (auto* child : ctrl->children()) {
        collect_control(L, child, tex_cache, font_cache, vp_w, vp_h,
                        self_clip);
    }
}

void UIRenderer::update(lua_State* L, const ui::UIControlRegistry& registry,
                        TextureCache& tex_cache, FontCache& font_cache,
                        u32 viewport_w, u32 viewport_h,
                        f32 mouse_x, f32 mouse_y) {
    mouse_x_ = mouse_x;
    mouse_y_ = mouse_y;
    quads_.clear();
    groups_.clear();
    quad_count_ = 0;

    if (!instance_mapped_) return;

    // Find the root frame from Lua registry
    lua_pushstring(L, "__osc_root_frame");
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return;
    }

    // Get _c_object from root frame
    lua_pushstring(L, "_c_object");
    lua_rawget(L, -2);
    auto* root = static_cast<ui::UIControl*>(lua_touserdata(L, -1));
    lua_pop(L, 2); // pop _c_object + root table

    if (!root) return;

    // Walk the control tree from root with full viewport as initial clip
    ClipRect viewport_clip{0, 0, static_cast<i32>(viewport_w),
                           static_cast<i32>(viewport_h)};
    collect_control(L, root, tex_cache, font_cache, viewport_w, viewport_h,
                    viewport_clip);

    // Emit cursor quad at mouse position (topmost depth)
    emit_cursor_quad(L, tex_cache, viewport_w, viewport_h, viewport_clip);

    if (quads_.empty()) return;

    // Sort by depth (lower depth = drawn first = further back)
    std::stable_sort(quads_.begin(), quads_.end(),
                     [](const QuadEntry& a, const QuadEntry& b) {
                         return a.depth < b.depth;
                     });

    // Upload instances and build draw groups (batch by consecutive same texture)
    auto* dst = static_cast<UIInstance*>(instance_mapped_);
    u32 count = static_cast<u32>(quads_.size());
    if (count > MAX_UI_QUADS) count = MAX_UI_QUADS;

    for (u32 i = 0; i < count; i++) {
        dst[i] = quads_[i].inst;
    }

    // Build draw groups — batch consecutive quads with same descriptor set AND clip rect
    UIDrawGroup current{};
    current.texture_ds = quads_[0].texture_ds;
    current.clip = quads_[0].clip;
    current.instance_offset = 0;
    current.instance_count = 1;

    for (u32 i = 1; i < count; i++) {
        if (quads_[i].texture_ds == current.texture_ds &&
            quads_[i].clip == current.clip) {
            current.instance_count++;
        } else {
            groups_.push_back(current);
            current.texture_ds = quads_[i].texture_ds;
            current.clip = quads_[i].clip;
            current.instance_offset = i;
            current.instance_count = 1;
        }
    }
    groups_.push_back(current);
}

void UIRenderer::render(VkCommandBuffer cmd, VkPipelineLayout layout,
                        u32 viewport_w, u32 viewport_h) {
    if (groups_.empty() || !instance_buf_.buffer) return;

    // Push viewport dimensions
    f32 push[2] = {static_cast<f32>(viewport_w), static_cast<f32>(viewport_h)};
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 8, push);

    // Bind instance buffer at binding 0
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &instance_buf_.buffer, &offset);

    // Track current scissor to avoid redundant state changes
    ClipRect current_scissor{-1, -1, -1, -1};

    // Draw each group
    for (const auto& group : groups_) {
        // Set scissor rect per group (clip children to parent bounds)
        if (group.clip != current_scissor) {
            VkRect2D scissor{};
            scissor.offset.x = std::max(group.clip.x, 0);
            scissor.offset.y = std::max(group.clip.y, 0);
            scissor.extent.width = static_cast<u32>(
                std::max(group.clip.w, 0));
            scissor.extent.height = static_cast<u32>(
                std::max(group.clip.h, 0));
            vkCmdSetScissor(cmd, 0, 1, &scissor);
            current_scissor = group.clip;
        }

        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                layout, 0, 1, &group.texture_ds,
                                0, nullptr);
        // 6 vertices per quad (2 triangles), N instances
        vkCmdDraw(cmd, 6, group.instance_count, 0, group.instance_offset);
    }

    // Restore full viewport scissor after UI rendering
    VkRect2D full_scissor{};
    full_scissor.extent = {viewport_w, viewport_h};
    vkCmdSetScissor(cmd, 0, 1, &full_scissor);
}

void UIRenderer::emit_cursor_quad(lua_State* L, TextureCache& tex_cache,
                                   u32 vp_w, u32 vp_h,
                                   const ClipRect& viewport_clip) {
    // Read active cursor from registry
    lua_pushstring(L, "__osc_active_cursor");
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return; }

    // Get _c_object
    lua_pushstring(L, "_c_object");
    lua_rawget(L, -2);
    auto* cursor = static_cast<ui::UIControl*>(lua_touserdata(L, -1));
    lua_pop(L, 2);

    if (!cursor || !cursor->cursor_visible()) return;

    const std::string& tex_path = cursor->cursor_texture();
    if (tex_path.empty()) return;

    const GPUTexture* tex = tex_cache.get(tex_path);
    if (!tex) return;

    // Get cursor texture dimensions for quad size
    f32 cw = static_cast<f32>(cursor->bitmap_width());
    f32 ch = static_cast<f32>(cursor->bitmap_height());
    if (cw <= 0 || ch <= 0) { cw = 32.0f; ch = 32.0f; } // fallback size

    f32 cx = mouse_x_ - cursor->cursor_hotspot_x();
    f32 cy = mouse_y_ - cursor->cursor_hotspot_y();

    QuadEntry entry{};
    entry.inst.rect[0] = cx;
    entry.inst.rect[1] = cy;
    entry.inst.rect[2] = cw;
    entry.inst.rect[3] = ch;
    entry.inst.uv[0] = 0.0f;
    entry.inst.uv[1] = 0.0f;
    entry.inst.uv[2] = 1.0f;
    entry.inst.uv[3] = 1.0f;
    entry.inst.color[0] = 1.0f;
    entry.inst.color[1] = 1.0f;
    entry.inst.color[2] = 1.0f;
    entry.inst.color[3] = 1.0f;
    entry.texture_ds = tex->descriptor_set;
    entry.clip = viewport_clip;
    entry.depth = 999999.0f; // topmost
    quads_.push_back(entry);
}

void UIRenderer::advance_animations(lua_State* L,
                                     const ui::UIControlRegistry& registry,
                                     f32 delta_time) {
    for (auto& uptr : registry.all()) {
        auto* ctrl = uptr.get();
        if (!ctrl || !ctrl->anim_playing()) continue;
        if (ctrl->textures().size() <= 1) continue;

        f32 rate = ctrl->frame_rate();
        if (rate <= 0.0f) continue;

        f32 accum = ctrl->anim_accumulator() + delta_time;
        f32 frame_dur = 1.0f / rate;

        while (accum >= frame_dur) {
            accum -= frame_dur;

            if (!ctrl->frame_pattern().empty()) {
                i32 pi = ctrl->pattern_index() + 1;
                i32 ps = static_cast<i32>(ctrl->frame_pattern().size());
                if (pi >= ps) {
                    if (ctrl->anim_looping()) {
                        pi = 0;
                    } else {
                        pi = ps - 1;
                        ctrl->set_anim_playing(false);
                        accum = 0.0f;
                        break;
                    }
                }
                ctrl->set_pattern_index(pi);
                ctrl->set_current_frame(ctrl->frame_pattern()[pi]);
            } else {
                i32 f = ctrl->current_frame() + 1;
                i32 n = ctrl->num_frames();
                if (f >= n) {
                    if (ctrl->anim_looping()) {
                        f = 0;
                    } else {
                        f = n - 1;
                        ctrl->set_anim_playing(false);
                        accum = 0.0f;
                        break;
                    }
                }
                ctrl->set_current_frame(f);
            }
        }
        ctrl->set_anim_accumulator(accum);
    }
}

void UIRenderer::destroy(VkDevice device, VmaAllocator allocator) {
    if (instance_buf_.buffer) {
        vmaDestroyBuffer(allocator, instance_buf_.buffer, instance_buf_.allocation);
        instance_buf_ = {};
    }
    instance_mapped_ = nullptr;
}

} // namespace osc::renderer
