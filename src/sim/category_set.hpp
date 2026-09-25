#pragma once

#include "core/types.hpp"

#include <optional>
#include <string_view>
#include <vector>

namespace osc::sim {

/// Category names ("LAND", "TECH2", ...) as small ids, so a unit's
/// categories are a bitset and a compiled category expression tests one in a
/// few instructions rather than hashing strings. Ids are given in the order
/// names are first seen, per process, and never reused. The sim only ever
/// asks whether a unit has a category, never an id's value, so ids can't
/// change a game's outcome.
class CategoryIds {
public:
    /// The id of `name`, giving it one if it has none yet.
    static u32 intern(std::string_view name);
    /// The id of `name` if some entity has had it, else nothing (a category
    /// nobody has matches no one).
    static std::optional<u32> find(std::string_view name);
};

/// A set of category ids.
class CategoryBits {
public:
    void set(u32 id) {
        const size_t word = id / 64;
        if (word >= words_.size()) words_.resize(word + 1, 0);
        words_[word] |= u64{1} << (id % 64);
    }
    bool test(u32 id) const {
        const size_t word = id / 64;
        return word < words_.size() && ((words_[word] >> (id % 64)) & 1) != 0;
    }

private:
    std::vector<u64> words_;
};

} // namespace osc::sim
