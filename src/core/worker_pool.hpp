#pragma once

#include "core/types.hpp"

#include <cstddef>
#include <functional>

namespace osc {

/// Spare cores for the sim's work that splits into independent pieces
/// (WOPC, 2026-10-20: a late eight-AI match ticks on one core of six).
///
/// for_each(n, fn) runs fn(begin, end) over [0, n) in chunks, on the pool's
/// threads and the caller's, and returns when every chunk is done. The game
/// stays deterministic as long as each index's work reads what no other
/// index writes and writes only its own result (a slot of a vector sized
/// beforehand): the caller then applies the results in index order on its
/// own thread. Nothing that runs Lua, sends an event or touches shared
/// state belongs in fn.
///
/// One for_each at a time: another thread's call waits for it, and a call
/// from inside fn (or with too little work to share) runs on the caller.
class WorkerPool {
public:
    using Range = std::function<void(size_t begin, size_t end)>;

    /// The process's pool: the hardware's threads less two (the main thread
    /// and the sim's), at least none.
    static WorkerPool& instance();

    /// fn over [0, count) in chunks of at least `grain` indices.
    void for_each(size_t count, size_t grain, const Range& fn);

    /// Threads besides the caller's that share the work.
    u32 workers() const;

    /// Use no workers (the caller's thread alone): a run to compare with.
    void set_enabled(bool enabled);

    struct Impl;

private:
    WorkerPool();
    ~WorkerPool();
    Impl* impl_;
};

} // namespace osc
