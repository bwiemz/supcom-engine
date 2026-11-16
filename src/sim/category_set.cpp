#include "sim/category_set.hpp"

#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace osc::sim {

namespace {

/// Hashes a name as a string_view, so a lookup copies no string.
struct NameHash {
    using is_transparent = void;
    size_t operator()(std::string_view name) const { return std::hash<std::string_view>{}(name); }
};

struct Registry {
    std::mutex mutex; // the UI's unit objects intern too, on the same thread today
    std::unordered_map<std::string, u32, NameHash, std::equal_to<>> ids;
};

Registry& registry() {
    static Registry r;
    return r;
}

} // namespace

u32 CategoryIds::intern(std::string_view name) {
    auto& r = registry();
    const std::lock_guard lock(r.mutex);
    if (const auto it = r.ids.find(name); it != r.ids.end()) return it->second;
    const u32 id = static_cast<u32>(r.ids.size());
    r.ids.emplace(std::string(name), id);
    return id;
}

std::optional<u32> CategoryIds::find(std::string_view name) {
    auto& r = registry();
    const std::lock_guard lock(r.mutex);
    const auto it = r.ids.find(name);
    if (it == r.ids.end()) return std::nullopt;
    return it->second;
}

} // namespace osc::sim
