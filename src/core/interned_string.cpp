#include "core/interned_string.hpp"

#include <mutex>
#include <unordered_set>

namespace osc {

namespace {

struct Pool {
    std::mutex mutex;
    // A node set: its strings never move as it grows.
    std::unordered_set<std::string> strings;
};

Pool& pool() {
    static Pool* p = new Pool; // kept past static destruction: snapshots may outlive it
    return *p;
}

} // namespace

const std::string& InternedString::empty_string() {
    static const std::string* empty = new std::string;
    return *empty;
}

const std::string& InternedString::intern(std::string_view text) {
    if (text.empty()) return empty_string();
    Pool& p = pool();
    const std::lock_guard<std::mutex> lock(p.mutex);
    return *p.strings.emplace(text).first;
}

} // namespace osc
