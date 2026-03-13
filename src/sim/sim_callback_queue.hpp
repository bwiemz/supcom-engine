#pragma once

#include "core/types.hpp"
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace osc::sim {

struct SimCallbackEntry {
    std::string func_name;
    // Args: simple key→value map. Values can be string, number, or bool.
    // This covers the vast majority of FA SimCallback usage.
    std::unordered_map<std::string, std::variant<std::string, f64, bool>> args;
    // Optional: selected unit entity IDs (when addUnitSelection=true)
    std::vector<u32> unit_ids;
};

class SimCallbackQueue {
public:
    void push(SimCallbackEntry entry) {
        queue_.push_back(std::move(entry));
    }

    std::vector<SimCallbackEntry> drain() {
        std::vector<SimCallbackEntry> result;
        result.swap(queue_);
        return result;
    }

    bool empty() const { return queue_.empty(); }

private:
    std::vector<SimCallbackEntry> queue_;
};

} // namespace osc::sim
