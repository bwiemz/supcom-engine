#pragma once

#include <algorithm>

namespace osc {

/// Fixed-timestep scheduling for the sim: add this frame's (speed-scaled)
/// time to `accumulator` and return how many whole `step`s to run now, at
/// most `max_steps`. If more were due, the backlog is dropped (keeping only
/// the fractional remainder) so a sim that cannot keep up slows the game
/// down instead of making every frame run ever more ticks -- the "spiral of
/// death" that freezes rendering and input.
inline int consume_fixed_steps(double& accumulator, double frame_dt, double step,
                               int max_steps) {
    accumulator += std::max(frame_dt, 0.0);
    int steps = 0;
    while (accumulator >= step && steps < max_steps) {
        accumulator -= step;
        ++steps;
    }
    if (accumulator >= step) accumulator = std::min(accumulator, step * 0.999);
    return steps;
}

} // namespace osc
