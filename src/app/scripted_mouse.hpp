#pragma once

// --mouse "<when> <verb> <what>": a player's mouse through the window's input
// (CONTRIBUTING.md, Goldens)

#include "core/cursor.hpp"
#include "core/types.hpp"

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace osc::app {

struct MouseStep {
    enum class Clock : u8 { Frame, Tick };
    enum class Verb : u8 { Move, Press, Release };
    Clock clock = Clock::Frame;
    u32 at = 0;
    Verb verb = Verb::Move;
    bool world = false;
    f64 x = 0;
    f64 y = 0;
    i32 button = -1;
    i32 key = 0;
    std::string text;
};

std::optional<MouseStep> parse_mouse_step(std::string_view text);

struct MouseSink {
    std::function<void(f64 x, f64 y)> cursor;
    std::function<void(i32 button, i32 action, i32 mods)> button;
    std::function<void(i32 key, i32 action, i32 mods)> key;
    std::function<std::optional<std::array<f64, 2>>(f64 x, f64 z)> world_to_screen;
};

class ScriptedMouse {
public:
    explicit ScriptedMouse(std::vector<MouseStep> steps) : steps_(std::move(steps)) {}

    void frame(std::optional<u32> tick, const MouseSink& sink);
    const core::ScriptedPointer& pointer() const { return pointer_; }
    bool done() const { return next_ == steps_.size(); }
    std::string never_done() const;

private:
    bool deliver(const MouseStep& step, const MouseSink& sink);

    std::vector<MouseStep> steps_;
    std::size_t next_ = 0;
    u32 frames_ = 0;
    core::ScriptedPointer pointer_;
};

} // namespace osc::app
