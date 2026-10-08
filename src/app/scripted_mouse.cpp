#include "app/scripted_mouse.hpp"

#include <GLFW/glfw3.h>
#include <fmt/format.h>

#include <algorithm>
#include <charconv>
#include <cstdlib>

namespace osc::app {

namespace {

std::vector<std::string_view> words(std::string_view text) {
    std::vector<std::string_view> out;
    std::size_t i = 0;
    while (i < text.size()) {
        if (text[i] == ' ') {
            ++i;
            continue;
        }
        const std::size_t end = std::min(text.find(' ', i), text.size());
        out.push_back(text.substr(i, end - i));
        i = end;
    }
    return out;
}

std::optional<f64> number(std::string_view text) {
    const std::string s(text);
    char* end = nullptr;
    const f64 value = std::strtod(s.c_str(), &end);
    if (s.empty() || end != s.c_str() + s.size()) {
        return std::nullopt;
    }
    return value;
}

struct Control {
    std::string_view name;
    i32 button;
    i32 key;
    i32 mod;
};

constexpr Control kControls[] = {
    {"left", GLFW_MOUSE_BUTTON_LEFT, 0, 0},
    {"right", GLFW_MOUSE_BUTTON_RIGHT, 0, 0},
    {"middle", GLFW_MOUSE_BUTTON_MIDDLE, 0, 0},
    {"shift", -1, GLFW_KEY_LEFT_SHIFT, GLFW_MOD_SHIFT},
    {"ctrl", -1, GLFW_KEY_LEFT_CONTROL, GLFW_MOD_CONTROL},
    {"alt", -1, GLFW_KEY_LEFT_ALT, GLFW_MOD_ALT},
};

i32 mod_of(i32 key) {
    for (const Control& c : kControls) {
        if (c.button < 0 && c.key == key) {
            return c.mod;
        }
    }
    return 0;
}

} // namespace

std::optional<MouseStep> parse_mouse_step(std::string_view text) {
    const auto w = words(text);
    if (w.size() != 3) {
        return std::nullopt;
    }
    MouseStep step;
    step.text = std::string(text);
    std::string_view when = w[0];
    if (!when.empty() && when.front() == 't') {
        step.clock = MouseStep::Clock::Tick;
        when.remove_prefix(1);
    }
    const auto [end, ec] = std::from_chars(when.data(), when.data() + when.size(), step.at);
    if (when.empty() || ec != std::errc{} || end != when.data() + when.size()) {
        return std::nullopt;
    }
    if (w[1] == "move") {
        std::string_view where = w[2];
        if (!where.empty() && where.front() == 'w') {
            step.world = true;
            where.remove_prefix(1);
        }
        const std::size_t comma = where.find(',');
        if (comma == std::string_view::npos) {
            return std::nullopt;
        }
        const auto x = number(where.substr(0, comma));
        const auto y = number(where.substr(comma + 1));
        if (!x || !y) {
            return std::nullopt;
        }
        step.x = *x;
        step.y = *y;
        return step;
    }
    if (w[1] != "press" && w[1] != "release") {
        return std::nullopt;
    }
    step.verb = w[1] == "press" ? MouseStep::Verb::Press : MouseStep::Verb::Release;
    for (const Control& c : kControls) {
        if (c.name == w[2]) {
            step.button = c.button;
            step.key = c.key;
            return step;
        }
    }
    return std::nullopt;
}

void ScriptedMouse::frame(std::optional<u32> tick, const MouseSink& sink) {
    ++frames_;
    while (!done()) {
        const MouseStep& step = steps_[next_];
        const bool due =
            step.clock == MouseStep::Clock::Frame ? frames_ >= step.at : tick && *tick >= step.at;
        if (!due || !deliver(step, sink)) {
            return;
        }
        ++next_;
    }
}

bool ScriptedMouse::deliver(const MouseStep& step, const MouseSink& sink) {
    if (step.verb == MouseStep::Verb::Move) {
        std::array<f64, 2> at{step.x, step.y};
        if (step.world) {
            const auto screen =
                sink.world_to_screen ? sink.world_to_screen(step.x, step.y) : std::nullopt;
            if (!screen) {
                return false;
            }
            at = *screen;
        }
        pointer_.x = at[0];
        pointer_.y = at[1];
        if (sink.cursor) {
            sink.cursor(at[0], at[1]);
        }
        return true;
    }
    const i32 action = step.verb == MouseStep::Verb::Press ? GLFW_PRESS : GLFW_RELEASE;
    if (step.button >= 0) {
        const u32 bit = 1u << step.button;
        pointer_.buttons = action == GLFW_PRESS ? pointer_.buttons | bit : pointer_.buttons & ~bit;
        if (sink.button) {
            sink.button(step.button, action, pointer_.mods);
        }
        return true;
    }
    const i32 mod = mod_of(step.key);
    pointer_.mods = action == GLFW_PRESS ? pointer_.mods | mod : pointer_.mods & ~mod;
    if (sink.key) {
        sink.key(step.key, action, pointer_.mods);
    }
    return true;
}

std::string ScriptedMouse::never_done() const {
    if (done()) {
        return {};
    }
    return fmt::format("--mouse '{}': the run ended first", steps_[next_].text);
}

} // namespace osc::app
