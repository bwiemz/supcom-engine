#include "core/worker_pool.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace osc {

namespace {

/// One for_each's work, shared with the threads that take part: a worker
/// that wakes late finds its chunks all taken and leaves, never touching the
/// next call's.
struct Job {
    const WorkerPool::Range* fn = nullptr;
    size_t count = 0;
    size_t grain = 1;
    size_t chunks = 0;
    std::atomic<size_t> next{0};
    std::atomic<size_t> done{0};
};

/// Whether this thread is running a chunk (a for_each inside one runs inline).
thread_local bool g_in_chunk = false;

} // namespace

struct WorkerPool::Impl {
    std::vector<std::thread> threads;
    std::mutex call; ///< one for_each at a time
    std::mutex m;    ///< guards job, generation, stop
    std::condition_variable work, finished;
    std::shared_ptr<Job> job;
    u64 generation = 0;
    bool stop = false;
    std::atomic<bool> enabled{true};

    void run(Job& j) {
        const bool outer = g_in_chunk;
        g_in_chunk = true;
        for (;;) {
            const size_t begin = j.next.fetch_add(j.grain);
            if (begin >= j.count) break;
            (*j.fn)(begin, std::min(begin + j.grain, j.count));
            if (j.done.fetch_add(1) + 1 == j.chunks) {
                const std::lock_guard<std::mutex> lock(m);
                finished.notify_all();
            }
        }
        g_in_chunk = outer;
    }

    void worker() {
        u64 seen = 0;
        for (;;) {
            std::shared_ptr<Job> mine;
            {
                std::unique_lock<std::mutex> lock(m);
                work.wait(lock, [&] { return stop || generation != seen; });
                if (stop) return;
                seen = generation;
                mine = job;
            }
            if (mine) run(*mine);
        }
    }
};

WorkerPool::WorkerPool() : impl_(new Impl) {
    const u32 hardware = std::max(1u, std::thread::hardware_concurrency());
    const u32 count = hardware > 2 ? hardware - 2 : 0;
    for (u32 i = 0; i < count; ++i) impl_->threads.emplace_back([this] { impl_->worker(); });
}

WorkerPool::~WorkerPool() {
    {
        const std::lock_guard<std::mutex> lock(impl_->m);
        impl_->stop = true;
    }
    impl_->work.notify_all();
    for (auto& t : impl_->threads) t.join();
    delete impl_;
}

WorkerPool& WorkerPool::instance() {
    static WorkerPool pool;
    return pool;
}

u32 WorkerPool::workers() const {
    return impl_->enabled ? static_cast<u32>(impl_->threads.size()) : 0;
}

void WorkerPool::set_enabled(bool enabled) {
    impl_->enabled = enabled;
}

void WorkerPool::for_each(size_t count, size_t grain, const Range& fn) {
    if (count == 0) return;
    grain = std::max<size_t>(grain, 1);
    if (!impl_->enabled || impl_->threads.empty() || g_in_chunk || count <= grain) {
        fn(0, count);
        return;
    }
    const std::lock_guard<std::mutex> one(impl_->call);
    auto job = std::make_shared<Job>();
    job->fn = &fn;
    job->count = count;
    job->grain = grain;
    job->chunks = (count + grain - 1) / grain;
    {
        const std::lock_guard<std::mutex> lock(impl_->m);
        impl_->job = job;
        ++impl_->generation;
    }
    impl_->work.notify_all();
    impl_->run(*job);
    std::unique_lock<std::mutex> lock(impl_->m);
    impl_->finished.wait(lock, [&] { return job->done.load() == job->chunks; });
    impl_->job.reset();
}

} // namespace osc
