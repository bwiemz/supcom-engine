#include "app/sim_thread.hpp"

#include <utility>

namespace osc::app {

SimThread::SimThread() : thread_([this] { loop(); }) {}

SimThread::~SimThread() {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    wake_.notify_one();
    thread_.join();
}

void SimThread::start(std::function<void()> job) {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        job_ = std::move(job);
        running_ = true;
    }
    pending_ = true;
    wake_.notify_one();
}

void SimThread::wait() {
    if (!pending_) return;
    std::exception_ptr failure;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        done_.wait(lock, [this] { return !running_; });
        failure = std::exchange(failure_, nullptr);
    }
    pending_ = false;
    if (failure) std::rethrow_exception(failure);
}

void SimThread::loop() {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        wake_.wait(lock, [this] { return stop_ || (running_ && job_); });
        if (stop_) return;
        std::function<void()> job = std::move(job_);
        job_ = nullptr;
        lock.unlock();
        std::exception_ptr failure;
        try {
            job();
        } catch (...) {
            failure = std::current_exception();
        }
        lock.lock();
        failure_ = failure;
        running_ = false;
        done_.notify_one();
    }
}

} // namespace osc::app
