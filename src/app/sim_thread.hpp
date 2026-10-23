#pragma once

// The sim's own thread (OE-134; docs/plans/2026-10-16-sim-thread-design.md):
// a frame starts its ticks here and draws while they run; the next frame
// waits for them before anything reads the live sim again. One job at a time.

#include <condition_variable>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>

namespace osc::app {

class SimThread {
public:
    SimThread();
    ~SimThread();
    SimThread(const SimThread&) = delete;
    SimThread& operator=(const SimThread&) = delete;
    SimThread(SimThread&&) = delete;
    SimThread& operator=(SimThread&&) = delete;

    /// Run `job` on the sim thread. Only one at a time: the last must have
    /// been waited for.
    void start(std::function<void()> job);
    /// Until the job started last is done (at once with none). What it
    /// threw is thrown here, on the caller's thread.
    void wait();
    /// A job started and not yet waited for.
    bool busy() const { return pending_; }
    /// The job started last has finished: wait() would not block.
    bool done();
    /// The thread the jobs run on.
    std::thread::id id() const { return thread_.get_id(); }

private:
    void loop();

    std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable done_;
    std::function<void()> job_;
    bool running_ = false; ///< a job set and not finished (under mutex_)
    bool pending_ = false; ///< started and not waited for (the caller's)
    bool stop_ = false;
    std::exception_ptr failure_;
    std::thread thread_;
};

} // namespace osc::app
