#include "ui/ui_control.hpp"
#include "video/movie_player.hpp"

#include <algorithm>
#include <iterator>

namespace osc::ui {

// Destructor defined here so unique_ptr<MoviePlayer> sees complete type
UIControl::~UIControl() = default;

void UIControl::set_movie_player(std::unique_ptr<video::MoviePlayer> m) {
    movie_player_ = std::move(m);
}

// --- UIControl ---

void UIControl::advance_caret_cycle(f32 dt) {
    caret_time_ += dt;
    if (caret_time_ > caret_cycle_secs_) {
        caret_time_ = 0.0f;
    }
}

f32 UIControl::caret_alpha() const {
    const f32 half = caret_cycle_secs_ * 0.5f;
    const f32 rise = caret_time_ <= half ? caret_time_ : caret_cycle_secs_ - caret_time_;
    const f32 blend = caret_cycle_secs_ > 0.0f ? rise / half : 1.0f;
    return caret_min_alpha_ + (caret_max_alpha_ - caret_min_alpha_) * blend;
}

void UIControl::set_parent(UIControl* p) {
    if (parent_ == p) return;
    if (parent_) parent_->remove_child(this);
    parent_ = p;
    if (parent_) parent_->add_child(this);
}

void UIControl::add_child(UIControl* c) {
    if (std::find(children_.begin(), children_.end(), c) == children_.end())
        children_.push_back(c);
}

void UIControl::remove_child(UIControl* c) {
    children_.erase(
        std::remove(children_.begin(), children_.end(), c),
        children_.end());
}

void UIControl::clear_children() {
    for (auto* c : children_)
        c->parent_ = nullptr;
    children_.clear();
}

// --- UIControlRegistry ---

u32 UIControlRegistry::create() {
    auto ctrl = std::make_unique<UIControl>();
    u32 id = next_id_++;
    ctrl->set_control_id(id);
    controls_.push_back(std::move(ctrl));
    return id;
}

u32 UIControlRegistry::add(std::unique_ptr<UIControl> ctrl) {
    u32 id = next_id_++;
    ctrl->set_control_id(id);
    controls_.push_back(std::move(ctrl));
    return id;
}

UIControl* UIControlRegistry::get(u32 id) {
    for (auto& c : controls_) {
        if (c && c->control_id() == id && !c->destroyed())
            return c.get();
    }
    return nullptr;
}

void UIControlRegistry::destroy(u32 id) {
    for (auto& c : controls_) {
        if (c && c->control_id() == id) {
            c->mark_destroyed();
            if (keyboard_focus_ == c.get())
                keyboard_focus_ = nullptr;
            return;
        }
    }
}

void UIControlRegistry::push_input_capture(UIControl* c) {
    if (c) input_capture_.push_back(c);
}

void UIControlRegistry::compact_input_capture() {
    std::erase_if(input_capture_, [](const UIControl* c) { return c->destroyed(); });
}

void UIControlRegistry::remove_input_capture(UIControl* c) {
    compact_input_capture();
    for (auto it = input_capture_.rbegin(); it != input_capture_.rend(); ++it) {
        if (*it == c) {
            input_capture_.erase(std::next(it).base());
            return;
        }
    }
}

UIControl* UIControlRegistry::input_capture() {
    compact_input_capture();
    return input_capture_.empty() ? nullptr : input_capture_.back();
}

u32 UIControlRegistry::count() const {
    u32 n = 0;
    for (auto& c : controls_) {
        if (c && !c->destroyed()) n++;
    }
    return n;
}

} // namespace osc::ui
