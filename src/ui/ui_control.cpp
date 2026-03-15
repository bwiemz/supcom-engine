#include "ui/ui_control.hpp"
#include "video/video_decoder.hpp"

#include <algorithm>

namespace osc::ui {

// Destructor defined here so unique_ptr<VideoDecoder> sees complete type
UIControl::~UIControl() = default;

// --- UIControl ---

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

u32 UIControlRegistry::count() const {
    u32 n = 0;
    for (auto& c : controls_) {
        if (c && !c->destroyed()) n++;
    }
    return n;
}

} // namespace osc::ui
