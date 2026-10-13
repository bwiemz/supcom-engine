#pragma once

#include <algorithm>
#include <array>

namespace osc {

/// Moho's RollingFrameRates (faf-re CScApp): the last frame times in a ring
/// of ten slots, which holds nine (an empty ring has start == end), and their
/// median. CScApp::Main runs the world's session on that median, not on the
/// frame's own time, so a slow frame gives the game only a usual frame's
/// time: a hitch leaves the session's beats behind the wall clock for good.
class RollingFrameTimes {
public:
    /// A ring already holding nine frames of `seconds` each.
    static RollingFrameTimes full_of(double seconds) {
        RollingFrameTimes times;
        for (int i = 0; i < kSlots - 1; ++i) times.push(seconds);
        return times;
    }

    /// One frame's time; the oldest goes once nine are held.
    void push(double seconds) {
        const int next = (end_ + 1) % kSlots;
        vals_[end_] = seconds;
        if (next == start_) start_ = (start_ + 1) % kSlots;
        end_ = next;
    }

    /// The middle of the times held, sorted (of an even count, the upper of
    /// the two); 0 while it holds none.
    double median() const {
        std::array<double, kSlots> samples{};
        int count = 0;
        for (int i = start_; i != end_; i = (i + 1) % kSlots) samples[count++] = vals_[i];
        if (count == 0) return 0.0;
        std::sort(samples.begin(), samples.begin() + count);
        return samples[count / 2];
    }

private:
    static constexpr int kSlots = 10;
    std::array<double, kSlots> vals_{};
    int start_ = 0;
    int end_ = 0;
};

} // namespace osc
