#pragma once

#include "core/types.hpp"

#include <deque>
#include <set>
#include <vector>

namespace osc::sim {

/// Moho's IdPool (sim/IdPool.cpp): a serial is its least free one, else the next; one given back
/// is free again kHeldUpdates updates later.
struct IdPool {
    static constexpr size_t kHeldUpdates = 99;

    u32 next = 0;
    std::set<u32> released;
    /// The serials given back in each of the last updates, the newest open: Moho's ring.
    std::deque<std::vector<u32>> held = std::deque<std::vector<u32>>(1);

    u32 take();
    void give_back(u32 serial) { held.back().push_back(serial); }
    void update();
};

} // namespace osc::sim
