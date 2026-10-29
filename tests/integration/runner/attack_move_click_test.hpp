#pragma once

#include "core/types.hpp"

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace osc::app {
struct Engine;
struct Frame;
} // namespace osc::app

namespace osc::test {

/// --attack-move-click-test, windowed on FAF's data: an attack-move click released at each
/// frame of a tick's period ends in an AggressiveMove to its point.
class AttackMoveClickProbe {
public:
    /// Once per frame, before the frame's input.
    void on_frame(app::Engine& e, app::Frame& frame);
    bool done() const { return step_ == Step::Done; }
    /// The checks, logged and counted; at the end of the run.
    void finish() const;

private:
    enum class Step : u8 { Spawn, Select, WaitTick, Press, Release, Settle, Done };
    struct Click {
        int frames_after_tick = 0;
        bool ticked_before_send = false;
        bool moved = false;
        std::string queues;
    };

    Step step_ = Step::Spawn;
    u32 last_tick_ = 0;
    u32 released_at_ = 0;
    int wait_ = 0;
    int after_release_ = 0;
    f32 x_ = 0;
    f32 z_ = 0;
    std::optional<std::array<f32, 2>> at_;
    bool covered_ = false;
    std::vector<u32> units_;
    std::vector<Click> clicks_;
};

} // namespace osc::test
