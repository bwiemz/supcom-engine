#pragma once

#include "core/types.hpp"

#include <string>
#include <vector>

namespace osc::ui {

/// C++ backing object for all MAUI UI controls (Group, Frame, Bitmap, etc.).
/// Lua side holds a lightuserdata pointer to this via _c_object.
/// Layout (Left/Top/Width/Height/Depth) is handled entirely in Lua LazyVars.
class UIControl {
public:
    UIControl() = default;
    virtual ~UIControl() = default;

    // --- Identity ---
    u32 control_id() const { return control_id_; }
    void set_control_id(u32 id) { control_id_ = id; }

    const std::string& name() const { return name_; }
    void set_name(const std::string& n) { name_ = n; }

    // --- Lua table reference ---
    int lua_table_ref() const { return lua_table_ref_; }
    void set_lua_table_ref(int ref) { lua_table_ref_ = ref; }

    // --- Parent/children tree ---
    UIControl* parent() const { return parent_; }
    void set_parent(UIControl* p);
    void add_child(UIControl* c);
    void remove_child(UIControl* c);
    void clear_children();
    const std::vector<UIControl*>& children() const { return children_; }

    // --- Visibility ---
    bool hidden() const { return hidden_; }
    void set_hidden(bool h) { hidden_ = h; }

    // --- Alpha ---
    f32 alpha() const { return alpha_; }
    void set_alpha(f32 a) { alpha_ = a; }

    // --- Hit test ---
    bool hit_test_disabled() const { return hit_test_disabled_; }
    void set_hit_test_disabled(bool d) { hit_test_disabled_ = d; }

    // --- Render pass ---
    i32 render_pass() const { return render_pass_; }
    void set_render_pass(i32 p) { render_pass_ = p; }

    // --- Frame update ---
    bool needs_frame_update() const { return needs_frame_update_; }
    void set_needs_frame_update(bool n) { needs_frame_update_ = n; }

    // --- Keyboard focus ---
    bool has_keyboard_focus() const { return has_keyboard_focus_; }
    void set_keyboard_focus(bool f) { has_keyboard_focus_ = f; }
    bool blocks_key_down() const { return blocks_key_down_; }
    void set_blocks_key_down(bool b) { blocks_key_down_ = b; }

    // --- Destroyed flag ---
    bool destroyed() const { return destroyed_; }
    void mark_destroyed() { destroyed_ = true; }

    // --- Type flags ---
    virtual bool is_frame() const { return false; }

    // --- Bitmap state ---
    const std::string& texture_path() const { return texture_path_; }
    void set_texture_path(const std::string& p) { texture_path_ = p; }
    i32 texture_border() const { return texture_border_; }
    void set_texture_border(i32 b) { texture_border_ = b; }
    i32 bitmap_width() const { return bitmap_width_; }
    void set_bitmap_width(i32 w) { bitmap_width_ = w; }
    i32 bitmap_height() const { return bitmap_height_; }
    void set_bitmap_height(i32 h) { bitmap_height_ = h; }

    u32 solid_color() const { return solid_color_; }
    void set_solid_color(u32 c) { solid_color_ = c; }
    bool has_solid_color() const { return has_solid_color_; }
    void set_has_solid_color(bool h) { has_solid_color_ = h; }

    u32 color_mask() const { return color_mask_; }
    void set_color_mask(u32 c) { color_mask_ = c; }

    f32 uv_u0() const { return uv_u0_; }
    f32 uv_v0() const { return uv_v0_; }
    f32 uv_u1() const { return uv_u1_; }
    f32 uv_v1() const { return uv_v1_; }
    void set_uv(f32 u0, f32 v0, f32 u1, f32 v1) {
        uv_u0_ = u0; uv_v0_ = v0; uv_u1_ = u1; uv_v1_ = v1;
    }

    bool tiled() const { return tiled_; }
    void set_tiled(bool t) { tiled_ = t; }
    bool alpha_hit_test() const { return alpha_hit_test_; }
    void set_alpha_hit_test(bool a) { alpha_hit_test_ = a; }

    // Animation
    i32 current_frame() const { return current_frame_; }
    void set_current_frame(i32 f) { current_frame_ = f; }
    i32 num_frames() const { return num_frames_; }
    void set_num_frames(i32 n) { num_frames_ = n; }
    f32 frame_rate() const { return frame_rate_; }
    void set_frame_rate(f32 r) { frame_rate_ = r; }
    bool anim_playing() const { return anim_playing_; }
    void set_anim_playing(bool p) { anim_playing_ = p; }
    bool anim_looping() const { return anim_looping_; }
    void set_anim_looping(bool l) { anim_looping_ = l; }
    const std::vector<i32>& frame_pattern() const { return frame_pattern_; }
    void set_frame_pattern(std::vector<i32> p) { frame_pattern_ = std::move(p); }

    // Multi-texture support (SetNewTexture can take multiple filenames)
    const std::vector<std::string>& textures() const { return textures_; }
    void set_textures(std::vector<std::string> t) {
        textures_ = std::move(t);
        num_frames_ = static_cast<i32>(textures_.size());
    }

    // --- Text state ---
    const std::string& text_content() const { return text_content_; }
    void set_text_content(const std::string& t) { text_content_ = t; }

    const std::string& font_family() const { return font_family_; }
    void set_font_family(const std::string& f) { font_family_ = f; }
    i32 font_pointsize() const { return font_pointsize_; }
    void set_font_pointsize(i32 s) { font_pointsize_ = s; }

    u32 text_color() const { return text_color_; }
    void set_text_color(u32 c) { text_color_ = c; }

    bool drop_shadow() const { return drop_shadow_; }
    void set_drop_shadow(bool s) { drop_shadow_ = s; }
    bool clip_to_width() const { return clip_to_width_; }
    void set_clip_to_width(bool c) { clip_to_width_ = c; }
    bool centered_vertically() const { return centered_vertically_; }
    void set_centered_vertically(bool c) { centered_vertically_ = c; }
    bool centered_horizontally() const { return centered_horizontally_; }
    void set_centered_horizontally(bool c) { centered_horizontally_ = c; }

    f32 font_ascent() const { return font_ascent_; }
    void set_font_ascent(f32 a) { font_ascent_ = a; }
    f32 font_descent() const { return font_descent_; }
    void set_font_descent(f32 d) { font_descent_ = d; }
    f32 font_external_leading() const { return font_external_leading_; }
    void set_font_external_leading(f32 l) { font_external_leading_ = l; }
    f32 text_advance() const { return text_advance_; }
    void set_text_advance(f32 a) { text_advance_ = a; }

    // --- Edit state ---
    u32 foreground_color() const { return foreground_color_; }
    void set_foreground_color(u32 c) { foreground_color_ = c; }
    u32 background_color() const { return background_color_; }
    void set_background_color(u32 c) { background_color_ = c; }
    u32 caret_color() const { return caret_color_; }
    void set_caret_color(u32 c) { caret_color_ = c; }
    u32 highlight_fg_color() const { return highlight_fg_color_; }
    void set_highlight_fg_color(u32 c) { highlight_fg_color_ = c; }
    u32 highlight_bg_color() const { return highlight_bg_color_; }
    void set_highlight_bg_color(u32 c) { highlight_bg_color_ = c; }
    i32 caret_position() const { return caret_position_; }
    void set_caret_position(i32 p) { caret_position_ = p; }
    bool caret_visible() const { return caret_visible_; }
    void set_caret_visible(bool v) { caret_visible_ = v; }
    bool bg_visible() const { return bg_visible_; }
    void set_bg_visible(bool v) { bg_visible_ = v; }
    bool input_enabled() const { return input_enabled_; }
    void set_input_enabled(bool e) { input_enabled_ = e; }
    i32 max_chars() const { return max_chars_; }
    void set_max_chars(i32 m) { max_chars_ = m; }
    f32 caret_cycle_secs() const { return caret_cycle_secs_; }
    f32 caret_min_alpha() const { return caret_min_alpha_; }
    f32 caret_max_alpha() const { return caret_max_alpha_; }
    void set_caret_cycle(f32 secs, f32 min_a, f32 max_a) {
        caret_cycle_secs_ = secs; caret_min_alpha_ = min_a; caret_max_alpha_ = max_a;
    }

    // --- ItemList state ---
    const std::vector<std::string>& items() const { return items_; }
    void add_item(const std::string& text) { items_.push_back(text); }
    void delete_item(i32 index) {
        if (index >= 0 && index < static_cast<i32>(items_.size()))
            items_.erase(items_.begin() + index);
    }
    void delete_all_items() { items_.clear(); selection_ = -1; }
    void modify_item(i32 index, const std::string& text) {
        if (index >= 0 && index < static_cast<i32>(items_.size()))
            items_[index] = text;
    }
    const std::string& get_item(i32 index) const {
        static const std::string empty;
        if (index >= 0 && index < static_cast<i32>(items_.size()))
            return items_[index];
        return empty;
    }
    i32 item_count() const { return static_cast<i32>(items_.size()); }
    i32 selection() const { return selection_; }
    void set_selection(i32 s) { selection_ = s; }
    bool show_selection() const { return show_selection_; }
    void set_show_selection(bool s) { show_selection_ = s; }
    bool show_mouseover() const { return show_mouseover_; }
    void set_show_mouseover(bool m) { show_mouseover_ = m; }
    i32 scroll_top() const { return scroll_top_; }
    void set_scroll_top(i32 t) { scroll_top_ = t; }
    u32 item_fg_color() const { return item_fg_color_; }
    void set_item_fg_color(u32 c) { item_fg_color_ = c; }
    u32 item_bg_color() const { return item_bg_color_; }
    void set_item_bg_color(u32 c) { item_bg_color_ = c; }
    u32 item_sel_fg_color() const { return item_sel_fg_color_; }
    void set_item_sel_fg_color(u32 c) { item_sel_fg_color_ = c; }
    u32 item_sel_bg_color() const { return item_sel_bg_color_; }
    void set_item_sel_bg_color(u32 c) { item_sel_bg_color_ = c; }
    u32 item_mo_fg_color() const { return item_mo_fg_color_; }
    void set_item_mo_fg_color(u32 c) { item_mo_fg_color_ = c; }
    u32 item_mo_bg_color() const { return item_mo_bg_color_; }
    void set_item_mo_bg_color(u32 c) { item_mo_bg_color_ = c; }

    // --- Scrollbar state ---
    std::string scroll_axis() const { return scroll_axis_; }
    void set_scroll_axis(const std::string& a) { scroll_axis_ = a; }
    const std::string& sb_bg_texture() const { return sb_bg_texture_; }
    void set_sb_bg_texture(const std::string& t) { sb_bg_texture_ = t; }
    const std::string& sb_thumb_mid() const { return sb_thumb_mid_; }
    void set_sb_thumb_mid(const std::string& t) { sb_thumb_mid_ = t; }
    const std::string& sb_thumb_top() const { return sb_thumb_top_; }
    void set_sb_thumb_top(const std::string& t) { sb_thumb_top_ = t; }
    const std::string& sb_thumb_bot() const { return sb_thumb_bot_; }
    void set_sb_thumb_bot(const std::string& t) { sb_thumb_bot_ = t; }
    int scrollable_ref() const { return scrollable_ref_; }
    void set_scrollable_ref(int ref) { scrollable_ref_ = ref; }

private:
    u32 control_id_ = 0;
    std::string name_;
    int lua_table_ref_ = -2; // LUA_NOREF
    UIControl* parent_ = nullptr;
    std::vector<UIControl*> children_;
    bool hidden_ = false;
    f32 alpha_ = 1.0f;
    bool hit_test_disabled_ = false;
    i32 render_pass_ = 0;
    bool needs_frame_update_ = false;
    bool has_keyboard_focus_ = false;
    bool blocks_key_down_ = false;
    bool destroyed_ = false;

    // Bitmap state
    std::string texture_path_;
    i32 texture_border_ = 1;
    i32 bitmap_width_ = 0;
    i32 bitmap_height_ = 0;
    u32 solid_color_ = 0;
    bool has_solid_color_ = false;
    u32 color_mask_ = 0xFFFFFFFF;
    f32 uv_u0_ = 0.0f, uv_v0_ = 0.0f, uv_u1_ = 1.0f, uv_v1_ = 1.0f;
    bool tiled_ = false;
    bool alpha_hit_test_ = false;
    i32 current_frame_ = 0;
    i32 num_frames_ = 0;
    f32 frame_rate_ = 10.0f;
    bool anim_playing_ = false;
    bool anim_looping_ = false;
    std::vector<i32> frame_pattern_;
    std::vector<std::string> textures_;

    // Text state
    std::string text_content_;
    std::string font_family_ = "Arial";
    i32 font_pointsize_ = 14;
    u32 text_color_ = 0xFFFFFFFF;
    bool drop_shadow_ = false;
    bool clip_to_width_ = false;
    bool centered_vertically_ = false;
    bool centered_horizontally_ = false;
    f32 font_ascent_ = 0.0f;
    f32 font_descent_ = 0.0f;
    f32 font_external_leading_ = 0.0f;
    f32 text_advance_ = 0.0f;

    // Edit state
    u32 foreground_color_ = 0xFFFFFFFF;
    u32 background_color_ = 0xFF000000;
    u32 caret_color_ = 0xFFFFFFFF;
    u32 highlight_fg_color_ = 0xFFFFFFFF;
    u32 highlight_bg_color_ = 0xFF0000FF;
    i32 caret_position_ = 0;
    bool caret_visible_ = true;
    bool bg_visible_ = true;
    bool input_enabled_ = true;
    i32 max_chars_ = 0; // 0 = unlimited
    f32 caret_cycle_secs_ = 1.0f;
    f32 caret_min_alpha_ = 0.0f;
    f32 caret_max_alpha_ = 1.0f;

    // ItemList state
    std::vector<std::string> items_;
    i32 selection_ = -1;
    bool show_selection_ = true;
    bool show_mouseover_ = true;
    i32 scroll_top_ = 0;
    u32 item_fg_color_ = 0xFFFFFFFF;
    u32 item_bg_color_ = 0xFF000000;
    u32 item_sel_fg_color_ = 0xFFFFFFFF;
    u32 item_sel_bg_color_ = 0xFF0000FF;
    u32 item_mo_fg_color_ = 0xFFFFFFFF;
    u32 item_mo_bg_color_ = 0xFF333333;

    // Scrollbar state
    std::string scroll_axis_ = "Vert";
    std::string sb_bg_texture_;
    std::string sb_thumb_mid_;
    std::string sb_thumb_top_;
    std::string sb_thumb_bot_;
    int scrollable_ref_ = -2; // LUA_NOREF
};

/// Registry of all live UI controls, analogous to EntityRegistry.
class UIControlRegistry {
public:
    /// Create a new control, return its ID.
    u32 create();

    /// Get control by ID, nullptr if invalid.
    UIControl* get(u32 id);

    /// Destroy a control by ID (marks destroyed, does not immediately free).
    void destroy(u32 id);

    /// Get all live controls (for frame update iteration).
    const std::vector<std::unique_ptr<UIControl>>& all() const { return controls_; }

    /// Total live control count.
    u32 count() const;

    /// The control that currently has keyboard focus.
    UIControl* keyboard_focus() const { return keyboard_focus_; }
    void set_keyboard_focus(UIControl* c) { keyboard_focus_ = c; }

private:
    std::vector<std::unique_ptr<UIControl>> controls_;
    u32 next_id_ = 1;
    UIControl* keyboard_focus_ = nullptr;
};

} // namespace osc::ui
