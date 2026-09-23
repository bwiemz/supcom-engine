#pragma once

#include <algorithm>

namespace osc {

/// Where rendering sits between the last two sim ticks. The world is drawn
/// at prev + alpha * (cur - prev), one tick behind the sim, as Moho does;
/// alpha is the sim time since the newest tick over the tick length.
///
/// The clock follows real tick arrivals, not the loop's accumulator: in
/// multiplayer the accumulator paces frame sends and keeps cycling while a
/// lockstep stall holds the sim still, which would swing units between the
/// last two ticks. Here a stall holds alpha at 1 (the newest tick) and the
/// tick after it resumes interpolating.
class TickClock {
public:
    explicit TickClock(double tick_seconds) : tick_(tick_seconds) {}

    /// Sim time passed this frame (dt * game speed). Not called while the
    /// sim is paused or stopped, so the drawn world holds still.
    void advance(double sim_dt) { since_ += std::max(sim_dt, 0.0); }

    /// A tick completed.
    void on_tick() { since_ = std::clamp(since_ - tick_, 0.0, tick_); }

    /// A new session: nothing has been drawn between ticks yet.
    void reset() { since_ = 0.0; }

    float alpha() const {
        return static_cast<float>(std::clamp(since_ / tick_, 0.0, 1.0));
    }

private:
    double tick_;
    double since_ = 0.0;
};

} // namespace osc
