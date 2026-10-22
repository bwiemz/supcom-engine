#include "sim/lua_profile.hpp"

extern "C" {
#include <lua.h>
}

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace osc::sim::lua_profile {

namespace {

struct Function {
    std::string where; ///< source:line it starts on (name)
    uint64_t samples = 0;
};

struct Samples {
    // Own the source: Lua can collect a chunk or close its whole state
    // while these process-wide samples remain.
    std::map<std::pair<std::string, int>, Function> functions;
    uint64_t total = 0;
};

Samples& samples() {
    static Samples s;
    return s;
}

void hook(lua_State* L, lua_Debug* ar) {
    if (ar->event != LUA_HOOKCOUNT || !lua_getinfo(L, "Sn", ar)) return;
    Samples& s = samples();
    ++s.total;
    Function& f = s.functions[{ar->source, ar->linedefined}];
    if (f.where.empty()) {
        f.where = std::string(ar->short_src) + ":" + std::to_string(ar->linedefined);
        if (ar->name) f.where += std::string(" (") + ar->name + ")";
    }
    ++f.samples;
}

} // namespace

void attach(lua_State* L) {
    if (L && lua_gethook(L) != hook) lua_sethook(L, hook, LUA_MASKCOUNT, kInstructions);
}

std::vector<std::pair<std::string, uint64_t>> top(size_t count) {
    std::vector<std::pair<std::string, uint64_t>> out;
    for (const auto& [key, f] : samples().functions) out.emplace_back(f.where, f.samples);
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        return a.second != b.second ? a.second > b.second : a.first < b.first;
    });
    if (out.size() > count) out.resize(count);
    return out;
}

uint64_t total() {
    return samples().total;
}

void log_report(size_t count) {
    const uint64_t all = total();
    if (all == 0) return;
    spdlog::info("=== Lua profile: {} samples, one per {} instructions ===", all, kInstructions);
    for (const auto& [where, n] : top(count))
        spdlog::info("  {:>5.1f}% {:>9}  {}",
                     100.0 * static_cast<double>(n) / static_cast<double>(all), n, where);
}

} // namespace osc::sim::lua_profile
