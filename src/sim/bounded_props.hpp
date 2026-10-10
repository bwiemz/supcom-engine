#pragma once

#include "core/types.hpp"

#include <cstddef>
#include <vector>

namespace osc::sim {

class Prop;

/// Moho's EntityDB::mBoundedProps (faf-re EntityDb.cpp).
class BoundedProps {
public:
    static constexpr size_t kLimit = 1000;

    i32 insert(i32 priority, i32 tick, Prop* prop);
    void remove(i32 handle);
    Prop* lowest() const { return heap_.empty() ? nullptr : heap_.front().prop; }
    void pop_lowest() { pop_at(0); }
    size_t size() const { return heap_.size(); }
    void clear();

private:
    struct Node {
        i32 priority;
        i32 tick;
        Prop* prop;
        i32 handle;
    };
    static bool lower(const Node& a, const Node& b);
    void swap_nodes(i32 a, i32 b);
    i32 sift_up(i32 index);
    void sift_down(i32 index, i32 count);
    void pop_at(i32 index);

    std::vector<Node> heap_;
    std::vector<i32> slots_;
    i32 free_handle_ = -1;
};

} // namespace osc::sim
