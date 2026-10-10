#include "sim/bounded_props.hpp"

#include <utility>

namespace osc::sim {

bool BoundedProps::lower(const Node& a, const Node& b) {
    if (a.priority != b.priority) {
        return a.priority < b.priority;
    }
    return a.tick < b.tick;
}

void BoundedProps::swap_nodes(i32 a, i32 b) {
    if (a == b) {
        return;
    }
    std::swap(heap_[a], heap_[b]);
    slots_[heap_[a].handle] = a;
    slots_[heap_[b].handle] = b;
}

i32 BoundedProps::sift_up(i32 index) {
    while (index != 0) {
        const i32 parent = (index - 1) / 2;
        if (lower(heap_[parent], heap_[index])) {
            break;
        }
        swap_nodes(parent, index);
        index = parent;
    }
    return index;
}

void BoundedProps::sift_down(i32 index, i32 count) {
    for (;;) {
        const i32 left = index * 2 + 1;
        if (left >= count) {
            return;
        }
        i32 best = index;
        if (lower(heap_[left], heap_[best])) {
            best = left;
        }
        const i32 right = left + 1;
        if (right < count && lower(heap_[right], heap_[best])) {
            best = right;
        }
        if (best == index) {
            return;
        }
        swap_nodes(index, best);
        index = best;
    }
}

i32 BoundedProps::insert(i32 priority, i32 tick, Prop* prop) {
    const i32 index = static_cast<i32>(heap_.size());
    i32 handle = free_handle_;
    if (handle == -1) {
        handle = static_cast<i32>(slots_.size());
        slots_.push_back(index);
    } else {
        free_handle_ = slots_[handle];
        slots_[handle] = index;
    }
    heap_.push_back({priority, tick, prop, handle});
    sift_up(index);
    return handle;
}

void BoundedProps::pop_at(i32 index) {
    const i32 last = static_cast<i32>(heap_.size()) - 1;
    if (index != last) {
        swap_nodes(index, last);
        sift_down(index, last);
    }
    const i32 released = heap_.back().handle;
    slots_[released] = free_handle_;
    free_handle_ = released;
    heap_.pop_back();
}

void BoundedProps::remove(i32 handle) {
    pop_at(slots_[handle]);
}

void BoundedProps::clear() {
    heap_.clear();
    slots_.clear();
    free_handle_ = -1;
}

} // namespace osc::sim
