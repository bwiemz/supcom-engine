#include "lua/lua_thread_check.hpp"

#include <spdlog/spdlog.h>

#include <atomic>
#include <functional>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

// The vendored Lua's (lapi.c): the check lua_lock calls, and a state's global
extern void (*osc_lua_lock_check)(lua_State* L);
void* osc_lua_global(lua_State* L);

namespace osc::lua {

namespace {

std::atomic<void*> watched{nullptr};
std::atomic<size_t> owner_hash{0}; ///< 0: anyone
std::atomic<u64> violations{0};

void check(lua_State* L) {
    if (osc_lua_global(L) != watched.load(std::memory_order_relaxed)) return;
    const size_t owner = owner_hash.load(std::memory_order_relaxed);
    if (owner == 0 || owner == std::hash<std::thread::id>{}(std::this_thread::get_id())) return;
    const u64 n = violations.fetch_add(1) + 1;
    if (n > 5) return; // the first few say where
#ifdef _WIN32
    void* frames[16];
    const USHORT count = CaptureStackBackTrace(1, 16, frames, nullptr);
    std::string at;
    for (USHORT i = 0; i < count; ++i) at += fmt::format(" {}", frames[i]);
    spdlog::error("[lua] a watched state used off its thread (module {}):{}",
                  static_cast<void*>(GetModuleHandleW(nullptr)), at);
#else
    spdlog::error("[lua] a watched state used off its thread");
#endif
}

} // namespace

void install_lua_thread_check() {
    osc_lua_lock_check = check;
}

void watch_lua_state(lua_State* L, std::thread::id owner) {
    watched = L ? osc_lua_global(L) : nullptr;
    owner_hash = owner == std::thread::id{} ? 0 : std::hash<std::thread::id>{}(owner);
}

u64 lua_thread_violations() {
    return violations.load();
}

} // namespace osc::lua
