#include "sim/id_pool.hpp"

#include <iterator>

namespace osc::sim {

u32 IdPool::take() {
    if (released.empty()) {
        return next++;
    }
    const u32 serial = *released.begin();
    released.erase(released.begin());
    return serial;
}

void IdPool::update() {
    if (held.size() == kHeldUpdates) {
        const std::vector<u32>& oldest = held.front();
        if (!oldest.empty()) {
            released.insert(oldest.begin(), oldest.end());
            auto top = released.lower_bound(next);
            u32 low = next;
            while (top != released.begin() && *std::prev(top) == low - 1) {
                --top;
                --low;
            }
            if (low != next) {
                released.erase(top, released.end());
                next = low;
            }
        }
        held.pop_front();
    }
    held.emplace_back();
}

} // namespace osc::sim
