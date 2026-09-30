#pragma once

// --click <label>: the front end's buttons clicked in turn (CONTRIBUTING.md, Goldens)

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace osc::lua {
class LuaState;
}
namespace osc::ui {
class UIDispatch;
class UIControlRegistry;
} // namespace osc::ui

namespace osc::app {

class UiClicks {
public:
    explicit UiClicks(std::vector<std::string> labels) : labels_(std::move(labels)) {}

    /// Each frame: the next click, once its button takes one.
    void frame(lua::LuaState& ui, ui::UIDispatch& input, ui::UIControlRegistry& controls);
    bool done() const { return next_ == labels_.size(); }
    const std::string& waiting_for() const { return labels_[next_]; }

private:
    std::vector<std::string> labels_;
    std::size_t next_ = 0;
};

} // namespace osc::app
