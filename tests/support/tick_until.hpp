#pragma once

#include <catch2/catch_test_macros.hpp>

#include <string_view>

namespace osc::test {

/// Calls `tick` until `done` holds, at most `limit` times, and returns the calls it took (0 if
/// `done` held at once); fails the test case, naming `what`, if `done` never holds.
template <typename Tick, typename Done>
int tick_until(Tick&& tick, std::string_view what, int limit, Done&& done) {
    for (int ticks = 0;; ++ticks) {
        if (done()) {
            return ticks;
        }
        if (ticks == limit) {
            break;
        }
        tick();
    }
    FAIL(what << ": not within " << limit << " ticks");
    return limit;
}

} // namespace osc::test
