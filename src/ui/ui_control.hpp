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
