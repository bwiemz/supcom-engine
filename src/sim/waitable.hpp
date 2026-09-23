#pragma once

#include "core/types.hpp"

namespace osc::sim {

/// Base class for objects that can be waited on via WaitFor().
/// Both Manipulator and EconomyEvent inherit from this.
class Waitable {
public:
    virtual ~Waitable() = default;

    /// Whether the waitable has reached its goal / is done.
    virtual bool is_done() const = 0;

    /// Whether the waitable has been destroyed/cancelled.
    virtual bool is_cancelled() const = 0;

    // WaitFor support: the thread parked on this object, as its registry
    // ref and ThreadManager serial. A killed thread's ref returns to Lua's
    // free list and can name a new thread, so a wake must match both.
    bool has_waiting_thread() const { return waiting_thread_ref_ >= 0; }
    int waiting_thread_ref() const { return waiting_thread_ref_; }
    u64 waiting_thread_serial() const { return waiting_thread_serial_; }
    void set_waiting_thread(int ref, u64 serial) {
        waiting_thread_ref_ = ref;
        waiting_thread_serial_ = serial;
    }
    void clear_waiting_thread() { set_waiting_thread(-2, 0); }

protected:
    int waiting_thread_ref_ = -2; // LUA_NOREF: no one is waiting
    u64 waiting_thread_serial_ = 0;
};

} // namespace osc::sim
