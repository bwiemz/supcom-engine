#include "sim/lua_profile.hpp"

extern "C" {
#include <lua.h>
}

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <unordered_map>
#include <map>
#include <set>
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

using Clock = std::chrono::steady_clock;

/// An engine function's calls and their time.
struct Binding {
    uint64_t calls = 0;
    double ms = 0;
};

/// The engine functions called, by name, and each Lua state's calls under
/// way (a coroutine's own; a C function calls none that hook).
struct Bindings {
    std::map<std::string, Binding> by_name;
    std::unordered_map<lua_State*, std::vector<std::pair<std::string, Clock::time_point>>> open;
};

Bindings& bindings() {
    static Bindings b;
    return b;
}

void time_binding(lua_State* L, lua_Debug* ar) {
    if (!lua_getinfo(L, "Sn", ar) || ar->what == nullptr || ar->what[0] != 'C') return;
    auto& open = bindings().open[L];
    if (ar->event == LUA_HOOKCALL) {
        open.emplace_back(ar->name ? ar->name : "?", Clock::now());
    } else if (!open.empty()) {
        Binding& b = bindings().by_name[open.back().first];
        ++b.calls;
        b.ms +=
            std::chrono::duration<double, std::milli>(Clock::now() - open.back().second).count();
        open.pop_back();
    }
}

void hook(lua_State* L, lua_Debug* ar) {
    if (ar->event == LUA_HOOKCALL || ar->event == LUA_HOOKRET) {
        time_binding(L, ar);
        return;
    }
    // OSC_LUA_PROFILE_LINES: by the line running, not the function it is in
    static const bool by_line = std::getenv("OSC_LUA_PROFILE_LINES") != nullptr;
    if (ar->event != LUA_HOOKCOUNT || !lua_getinfo(L, by_line ? "Snl" : "Sn", ar)) return;
    Samples& s = samples();
    ++s.total;
    const int line = by_line ? ar->currentline : ar->linedefined;
    Function& f = s.functions[{ar->source, line}];
    if (f.where.empty()) {
        f.where = std::string(ar->short_src) + ":" + std::to_string(line);
        if (ar->name && !by_line) f.where += std::string(" (") + ar->name + ")";
    }
    ++f.samples;
}

} // namespace

void attach(lua_State* L) {
    if (L && lua_gethook(L) != hook)
        lua_sethook(L, hook, LUA_MASKCOUNT | LUA_MASKCALL | LUA_MASKRET, kInstructions);
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

std::vector<EngineCall> engine_calls(size_t count) {
    // (Not what waits: a thread's yield or wait spans the ticks it sleeps.
    // A call that calls Lua -- pcall, a sort's compare -- counts that too.)
    static const std::set<std::string> kWaits{"WaitTicks",           "WaitFrames", "WaitFor",
                                              "WaitSeconds",         "yield",      "CoroutineYield",
                                              "SuspendCurrentThread"};
    std::vector<EngineCall> out;
    for (const auto& [name, b] : bindings().by_name)
        if (!kWaits.contains(name)) out.push_back({name, b.calls, b.ms});
    std::sort(out.begin(), out.end(), [](const EngineCall& a, const EngineCall& b) {
        return a.ms != b.ms ? a.ms > b.ms : a.name < b.name;
    });
    if (out.size() > count) out.resize(count);
    return out;
}

void log_report(size_t count) {
    const uint64_t all = total();
    if (all == 0) return;
    spdlog::info("=== Lua profile: {} samples, one per {} instructions ===", all, kInstructions);
    for (const auto& [where, n] : top(count))
        spdlog::info("  {:>5.1f}% {:>9}  {}",
                     100.0 * static_cast<double>(n) / static_cast<double>(all), n, where);
    spdlog::info("=== Engine functions the scripts called, by time ===");
    for (const auto& c : engine_calls(count))
        spdlog::info("  {:>10.1f} ms {:>10} calls {:>8.3f} ms each  {}", c.ms, c.calls,
                     c.calls ? c.ms / static_cast<double>(c.calls) : 0.0, c.name);
}

} // namespace osc::sim::lua_profile
