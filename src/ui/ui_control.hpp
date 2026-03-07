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
