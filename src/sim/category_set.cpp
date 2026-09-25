#include "sim/category_set.hpp"

#include <mutex>
#include <string>
#include <unordered_map>

namespace osc::sim {

namespace {

struct Registry {
    std::mutex mutex; // the UI's unit objects intern too, on the same thread today
    std::unordered_map<std::string, u32> ids;
};

Registry& registry() {
    static Registry r;
    return r;
}

} // namespace

u32 CategoryIds::intern(std::string_view name) {
    auto& r = registry();
    const std::lock_guard lock(r.mutex);
    const auto [it, added] = r.ids.try_emplace(std::string(name), static_cast<u32>(r.ids.size()));
    return it->second;
}

std::optional<u32> CategoryIds::find(std::string_view name) {
    auto& r = registry();
    const std::lock_guard lock(r.mutex);
    const auto it = r.ids.find(std::string(name));
    if (it == r.ids.end()) return std::nullopt;
    return it->second;
}

} // namespace osc::sim
