#pragma once

#include "core/types.hpp"

#include <string>
#include <vector>

struct lua_State;
struct lua_Debug;

namespace osc::sim {

struct ThreadEntry {
    lua_State* coroutine = nullptr;
    int lua_ref = -2;       // LUA_NOREF — no registry ref yet
    i32 wait_until_tick = 0; // Tick at which to resume (0 = resume next tick)
    bool dead = false;
    std::string source;     // Debug: where this thread was forked from
};

class ThreadManager {
public:
    static constexpr i32 DEFAULT_INSTRUCTION_BUDGET = 1'000'000;

    explicit ThreadManager(lua_State* L);

    /// ForkThread: creates a coroutine from the function at stack position 1.
    /// Expects stack: [1]=function, [2..n]=args.
    /// Returns a wrapper table with Destroy() support (like CThread).
    int fork_thread(lua_State* L);

    /// Kill a thread by its registry ref.
    void kill_thread(int ref);

    /// Store a pointer to this ThreadManager in the Lua registry
    /// so thread wrapper Destroy() can find it.
    void register_in_registry(lua_State* L);

    /// Resume all eligible threads for the given tick.
    void resume_all(u32 current_tick);

    /// Number of active (non-dead) threads.
    size_t active_count() const;

    /// Set the maximum number of Lua VM instructions per coroutine resume.
    /// Set to 0 to disable the instruction limit.
    void set_instruction_budget(i32 budget) { instruction_budget_ = budget; }

private:
    lua_State* L_;
    std::vector<ThreadEntry> threads_;
    std::vector<ThreadEntry> pending_threads_; // buffered during resume_all
    bool resuming_ = false; // true while inside resume_all loop
    i32 instruction_budget_ = DEFAULT_INSTRUCTION_BUDGET;

    void cleanup_dead_threads();

    /// Create and cache the shared metatable for thread wrapper tables.
    static void create_thread_metatable(lua_State* L);

    /// Hook callback fired when a coroutine exceeds its instruction budget.
    static void instruction_hook(lua_State* L, lua_Debug* ar);
};

} // namespace osc::sim
