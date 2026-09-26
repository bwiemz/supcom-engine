#include "sim/thread_manager.hpp"
#include "core/test_status.hpp"
#include "sim/waitable.hpp"

#include <algorithm>
#include <climits>
#include <cstring>
#include <spdlog/spdlog.h>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

namespace osc::sim {

void ThreadManager::instruction_hook(lua_State* L, lua_Debug*) {
    luaL_error(L, "instruction count exceeded");
}

ThreadManager::ThreadManager(lua_State* L) : L_(L) {}

// Destroy method for thread wrapper tables.
// Reads _c_ref from self, looks up ThreadManager via registry, kills thread.
static int l_thread_destroy(lua_State* L) {
    lua_pushstring(L, "_c_ref");
    lua_rawget(L, 1);
    if (!lua_isnumber(L, -1)) {
        lua_pop(L, 1);
        return 0;
    }
    int ref = static_cast<int>(lua_tonumber(L, -1));
    lua_pop(L, 1);
    lua_pushstring(L, "_c_serial");
    lua_rawget(L, 1);
    const u64 serial = lua_isnumber(L, -1) ? static_cast<u64>(lua_tonumber(L, -1)) : 0;
    lua_pop(L, 1);

    lua_pushstring(L, "osc_thread_mgr");
    lua_rawget(L, LUA_REGISTRYINDEX);
    auto* mgr = static_cast<ThreadManager*>(lua_touserdata(L, -1));
    lua_pop(L, 1);

    if (mgr && ref >= 0) {
        mgr->kill_thread(ref, serial);
    }
    return 0;
}

void ThreadManager::create_thread_metatable(lua_State* L) {
    // Create: { __index = { Destroy = l_thread_destroy } }
    lua_newtable(L); // metatable
    lua_pushstring(L, "__index");
    lua_newtable(L); // methods
    lua_pushstring(L, "Destroy");
    lua_pushcfunction(L, l_thread_destroy);
    lua_rawset(L, -3); // methods.Destroy = fn
    lua_rawset(L, -3); // mt.__index = methods

    // Cache in registry
    lua_pushstring(L, "__osc_thread_mt");
    lua_pushvalue(L, -2);
    lua_rawset(L, LUA_REGISTRYINDEX);
}

void ThreadManager::rebind(lua_State* L) {
    threads_.clear();
    pending_threads_.clear();
    L_ = L;
}

void ThreadManager::register_in_registry(lua_State* L) {
    lua_pushstring(L, "osc_thread_mgr");
    lua_pushlightuserdata(L, this);
    lua_rawset(L, LUA_REGISTRYINDEX);
}

int ThreadManager::fork_thread(lua_State* L) {
    // Stack: [1]=function, [2..n]=args
    // No extra diagnostics needed — just the standard check
    luaL_checktype(L, 1, LUA_TFUNCTION);
    int nargs = lua_gettop(L) - 1;

    // Create a new coroutine
    lua_State* co = lua_newthread(L);
    // Stack: [1]=fn, [2..n]=args, [n+1]=thread

    // Store thread ref in registry to prevent GC
    lua_pushvalue(L, -1);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);

    // Move function and args to the coroutine's stack
    lua_pushvalue(L, 1); // function
    lua_xmove(L, co, 1);
    for (int i = 2; i <= nargs + 1; i++) {
        lua_pushvalue(L, i);
        lua_xmove(L, co, 1);
    }

    // Capture source info for diagnostics (safe: doesn't modify L's stack)
    // Try level 2 first (actual call site, e.g. OnStopBeingBuilt line 2409),
    // then fall back to level 1 (wrapper method, e.g. Unit:ForkThread line 414).
    std::string source_info;
    {
        lua_Debug ar;
        bool found = false;
        for (int level = 2; level >= 1 && !found; --level) {
            if (lua_getstack(L, level, &ar)) {
                lua_getinfo(L, "Sl", &ar);
                if (ar.source && ar.currentline > 0) {
                    source_info = std::string(ar.source) + ":" +
                                  std::to_string(ar.currentline);
                    found = true;
                }
            }
        }
    }

    ThreadEntry entry;
    entry.coroutine = co;
    entry.lua_ref = ref;
    entry.serial = next_serial_++;
    entry.wait_until_tick = 0; // resume immediately on next tick
    entry.dead = false;
    entry.source = std::move(source_info);

    // Pop the raw thread — we return a wrapper table instead.
    // The raw thread stays alive via the registry ref.
    lua_pop(L, 1);

    // Build wrapper: { _c_ref, _c_serial } with shared metatable that has Destroy.
    // This matches the original engine's CThread userdata behavior.
    lua_newtable(L);
    int wrapper = lua_gettop(L);

    lua_pushstring(L, "_c_ref");
    lua_pushnumber(L, ref);
    lua_rawset(L, wrapper);
    // Which thread, not only which ref: refs are reused (see kill_thread).
    lua_pushstring(L, "_c_serial");
    lua_pushnumber(L, static_cast<lua_Number>(entry.serial));
    lua_rawset(L, wrapper);

    // Get or create the shared "__osc_thread_mt" metatable
    lua_pushstring(L, "__osc_thread_mt");
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        create_thread_metatable(L);
    }
    lua_setmetatable(L, wrapper);

    lua_pushvalue(L, wrapper);
    entry.wrapper_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    // If we're inside resume_all, buffer to avoid iterator invalidation
    if (resuming_) {
        pending_threads_.push_back(std::move(entry));
    } else {
        threads_.push_back(std::move(entry));
    }

    return 1;
}

void ThreadManager::kill_thread(int ref, u64 serial) {
    auto match = [&](const ThreadEntry& t) {
        return t.lua_ref == ref && (serial == 0 || t.serial == serial);
    };
    for (auto& t : threads_) {
        if (match(t)) {
            t.dead = true;
            return;
        }
    }
    for (auto& t : pending_threads_) {
        if (match(t)) {
            t.dead = true;
            return;
        }
    }
}

void ThreadManager::resume_all(u32 current_tick) {
    resuming_ = true;

    // Use index-based loop since pending_threads_ may grow via fork_thread,
    // but threads_ itself won't be modified during this loop.
    for (size_t i = 0; i < threads_.size(); i++) {
        auto& t = threads_[i];
        if (t.dead || t.wait_until_tick > static_cast<i32>(current_tick))
            continue;

        // Set instruction count hook to prevent infinite loops.
        // Skip hook setup for very large budgets (>= 1M) since they're
        // effectively unlimited — avoids per-instruction callback overhead.
        bool need_hook = (instruction_budget_ > 0 &&
                          instruction_budget_ < 1000000);
        if (need_hook) {
            lua_sethook(t.coroutine, instruction_hook,
                        LUA_MASKCOUNT, instruction_budget_);
        }

        // Lua 5.0 lua_resume(L, nargs): for initial call, the function
        // is at L->top - (nargs+1).  On first resume the coroutine stack
        // is [fn, arg1, ..., argN] so nargs = gettop - 1.  After a yield
        // we clear the stack (settop 0), so gettop - 1 = -1 → clamp to 0.
        int nargs = std::max(0, lua_gettop(t.coroutine) - 1);
        int status = lua_resume(t.coroutine, nargs);

        // Clear hook after resume
        if (need_hook) {
            lua_sethook(t.coroutine, nullptr, 0, 0);
        }

        if (status == 0) {
            // Lua 5.0: resume returns 0 for both yield and normal return.
            // Check if the coroutine is dead or suspended using the same
            // heuristic as Lua 5.0's coroutine.status (lbaselib.c):
            lua_Debug ar;
            if (lua_getstack(t.coroutine, 0, &ar) == 0) {
                // Thread finished normally (dead) — no active frames.
                // Discard any return values the thread may have produced.
                lua_settop(t.coroutine, 0);
                t.dead = true;
            } else {
                // Thread yielded. Check what it yielded:
                // - number → WaitTicks(n), resume after n ticks
                // - lightuserdata → WaitFor(manipulator), sleep until woken
                i32 wait_ticks = 1;
                if (lua_gettop(t.coroutine) > 0) {
                    if (lua_type(t.coroutine, -1) == LUA_TNUMBER) {
                        wait_ticks = std::max(
                            1,
                            static_cast<i32>(lua_tonumber(t.coroutine, -1)));
                    } else if (lua_type(t.coroutine, -1) == LUA_TLIGHTUSERDATA) {
                        // WaitFor(waitable) — store thread ref on
                        // waitable, sleep until it completes
                        auto* waitable = static_cast<Waitable*>(
                            lua_touserdata(t.coroutine, -1));
                        waitable->set_waiting_thread(t.lua_ref, t.serial);
                        lua_settop(t.coroutine, 0);
                        t.wait_until_tick = INT32_MAX;
                        continue;
                    }
                }
                lua_settop(t.coroutine, 0); // clear yielded values
                t.wait_until_tick =
                    static_cast<i32>(current_tick) + wait_ticks;
            }
        } else {
            // Thread errored
            const char* err = lua_tostring(t.coroutine, -1);
            spdlog::warn("Thread error: {} [forked at {}]",
                         err ? err : "(unknown)",
                         t.source.empty() ? "?" : t.source);
            // In test modes a dying script thread is a failed run (the Lua
            // error budget), even if the test's own checks passed.
            if (test_status::count_lua_failures()) {
                test_status::record_failure(fmt::format(
                    "Lua thread error: {} [forked at {}]", err ? err : "(unknown)",
                    t.source.empty() ? "?" : t.source));
            }
            t.dead = true;
        }
    }

    resuming_ = false;

    // Merge any threads that were forked during this tick
    if (!pending_threads_.empty()) {
        threads_.insert(threads_.end(), pending_threads_.begin(),
                        pending_threads_.end());
        pending_threads_.clear();
    }

    cleanup_dead_threads();
}

std::vector<std::string> ThreadManager::describe_threads() const {
    std::vector<std::string> out;
    for (const auto& t : threads_) {
        if (t.dead) continue;
        std::string where;
        lua_Debug ar;
        for (int level = 0; level < 4 && lua_getstack(t.coroutine, level, &ar); ++level) {
            lua_getinfo(t.coroutine, "Sln", &ar);
            if (!where.empty()) where += " <- ";
            where += std::string(ar.short_src) + ":" + std::to_string(ar.currentline);
            if (ar.name) where += std::string(" (") + ar.name + ")";
        }
        out.push_back("next tick " + std::to_string(t.wait_until_tick) + " | at " +
                      (where.empty() ? std::string("?") : where) + " | forked at " +
                      (t.source.empty() ? std::string("?") : t.source));
    }
    return out;
}

size_t ThreadManager::active_count() const {
    size_t count = 0;
    for (const auto& t : threads_) {
        if (!t.dead)
            count++;
    }
    return count;
}

void ThreadManager::wake(Waitable& w, u32 current_tick) {
    const int ref = w.waiting_thread_ref();
    const u64 serial = w.waiting_thread_serial();
    w.clear_waiting_thread();
    if (ref < 0) return;
    for (auto* list : {&threads_, &pending_threads_}) {
        for (auto& t : *list) {
            if (t.lua_ref == ref && t.serial == serial && !t.dead) {
                t.wait_until_tick = static_cast<i32>(current_tick);
                return;
            }
        }
    }
}

void ThreadManager::cleanup_dead_threads() {
    // Remove dead threads and release their registry refs
    auto it = threads_.begin();
    while (it != threads_.end()) {
        if (it->dead) {
            // The handle's ref first: Lua reuses the last one freed, and
            // the next thread forked takes the coroutine's.
            if (it->wrapper_ref >= 0) {
                luaL_unref(L_, LUA_REGISTRYINDEX, it->wrapper_ref);
                it->wrapper_ref = -2;
            }
            if (it->lua_ref >= 0) {
                luaL_unref(L_, LUA_REGISTRYINDEX, it->lua_ref);
                it->lua_ref = -2; // LUA_NOREF — prevent double-unref
            }
            it = threads_.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace osc::sim
