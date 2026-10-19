#pragma once

#include <algorithm>

namespace osc {

/// The game time the world is drawn at, in ticks, behind the newest tick the
/// sim has published. The renderer draws between the two published ticks
/// either side of it (WorldHistory keeps three), so a unit is where it was
/// between them, as Moho draws one tick behind.
///
/// It runs at the game's speed, eased to keep a set distance behind the
/// newest tick: three quarters of the ticks a publish brings, averaged. Its
/// distance swings a publish's ticks with each publish; the pace follows
/// that distance averaged over many frames, not its swing. A
/// sped-up game publishes a frame's several ticks at once, and not every
/// frame (the sim thread's job): drawn right up to the newest, the world
/// stood still until the next came and then leapt to catch up. Kept behind
/// it, the drawn time has published ticks ahead to go on into while the
/// next job runs, and it moves on evenly.
///
/// The clock follows real tick arrivals, not the loop's accumulator: in
/// multiplayer the accumulator paces frame sends and keeps cycling while a
/// lockstep stall holds the sim still. Here a stall holds the drawn time at
/// the newest tick, and the ticks after it are crossed a little faster
/// until it is back its distance behind.
class TickClock {
public:
    /// How far the pace may stray from the game's speed.
    static constexpr double kSlowest = 0.5;
    static constexpr double kFastest = 1.5;
    /// How far behind the newest tick the drawn time is kept, in publishes.
    static constexpr double kBehind = 1.0;
    /// How hard the pace steers toward that distance (per publish's worth),
    /// and how much of a frame's distance goes into its average.
    static constexpr double kSteer = 0.5;
    static constexpr double kSmooth = 0.05;
    /// Within this much of a publish of the newest tick, it slows.
    static constexpr double kEaseIn = 0.5;

    explicit TickClock(double tick_seconds) : tick_(tick_seconds) {}

    /// Sim time passed this frame (dt * game speed). Not called while the
    /// sim is paused or stopped, so the drawn world holds still.
    void advance(double sim_dt) {
        if (!started_) return;
        sim_dt = std::max(sim_dt, 0.0);
        const double off = (newest_ - kBehind * span_) - drawn_;
        off_ += kSmooth * (off - off_);
        pace_ = std::clamp(1.0 + kSteer * off_ / span_, kSlowest, kFastest);
        // Closing on the newest tick (the sim slow a moment), it eases in
        // rather than run into it and stand
        const double room = newest_ - drawn_;
        const double ease = std::clamp(room / (kEaseIn * span_), kSlowest, 1.0);
        drawn_ = std::min(drawn_ + sim_dt / tick_ * pace_ * ease, newest_);
    }

    /// A newer tick published, `span` ticks on from the one before it; the
    /// world can be drawn from `oldest` on (the history's oldest tick kept).
    void published(double newest, double oldest, double span) {
        span = std::max(span, 1.0);
        span_ = started_ ? 0.75 * span_ + 0.25 * span : span;
        if (!started_) drawn_ = newest - kBehind * span_;
        started_ = true;
        newest_ = newest;
        drawn_ = std::clamp(drawn_, std::min(oldest, newest), newest);
    }

    /// A new session: nothing has been drawn yet.
    void reset() {
        started_ = false;
        drawn_ = newest_ = off_ = 0.0;
        span_ = 1.0;
        pace_ = 1.0;
    }

    /// The drawn time, in ticks.
    double drawn() const { return drawn_; }
    /// The pace against the game's speed (1: the game's).
    double pace() const { return pace_; }

private:
    double tick_;
    bool started_ = false;
    double drawn_ = 0.0;
    double newest_ = 0.0;
    double off_ = 0.0;  ///< how far it is off its distance behind, averaged
    double span_ = 1.0; ///< ticks a publish brings, averaged
    double pace_ = 1.0;
};

} // namespace osc
