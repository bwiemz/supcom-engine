#pragma once

// --click <label>, --click-at <x>,<y>: the front end clicked in turn
// (CONTRIBUTING.md, Goldens)

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

/// A step: the button so labelled, or with no label the point (x, y)
struct UiClick {
    std::string label;
    double x = 0;
    double y = 0;
};

class UiClicks {
public:
    explicit UiClicks(std::vector<UiClick> steps) : steps_(std::move(steps)) {}

    /// Each frame: the next click, once its button takes one; a point a
    /// frame after the step before, which may open what lies under it
    void frame(lua::LuaState& ui, ui::UIDispatch& input, ui::UIControlRegistry& controls);
    bool done() const { return next_ == steps_.size(); }
    /// Why the run ended before its steps: the one it waits for
    std::string never_clicked() const;

private:
    std::vector<UiClick> steps_;
    std::size_t next_ = 0;
    int frames_since_ = 0;
};

} // namespace osc::app
