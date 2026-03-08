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

    // WaitFor support: store coroutine registry ref waiting on this object.
    // LUA_NOREF = -2 means no one is waiting.
    int waiting_thread_ref() const { return waiting_thread_ref_; }
    void set_waiting_thread_ref(int ref) { waiting_thread_ref_ = ref; }

protected:
    int waiting_thread_ref_ = -2; // LUA_NOREF
};

} // namespace osc::sim
