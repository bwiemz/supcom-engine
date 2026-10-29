#pragma once

#include "core/types.hpp"
#include "sim/econ_request.hpp"
#include "sim/entity_id.hpp"
#include "sim/waitable.hpp"

#include <algorithm>
#include <vector>
#include <memory>

namespace osc::sim {

/// A resource drain that completes once it has drawn its whole cost (M206d).
/// It asks its unit's army for its cost spread over its duration's whole
/// ticks, and moves on a tick each time its request is granted whole. Used by
/// teleports and by weapons with EnergyRequired (OverCharge).
/// Pattern: CreateEconomyEvent → WaitFor → RemoveEconomyEvent
class EconomyEvent : public Waitable {
    friend struct StateIO; // snapshots (state_io.hpp)
public:
    EconomyEvent(EntityId unit_id, f64 mass_drain, f64 energy_drain, f64 duration)
        : unit_id_(unit_id), total_mass_(mass_drain), total_energy_(energy_drain),
          duration_(duration),
          ticks_(std::max(1, static_cast<i32>(static_cast<f32>(duration) * 10.0f))),
          remaining_(ticks_) {
        // As misc/CEconomyEvent.cpp's constructor: the cost times 1/ticks, in floats.
        const f32 scale = 1.0f / static_cast<f32>(ticks_);
        request_.requested = {static_cast<f32>(mass_drain) * scale,
                              static_cast<f32>(energy_drain) * scale};
    }

    bool is_done() const override { return remaining_ == 0; }
    bool is_cancelled() const override { return cancelled_; }

    EntityId unit_id() const { return unit_id_; }
    f64 total_mass() const { return total_mass_; }
    f64 total_energy() const { return total_energy_; }
    f64 duration() const { return duration_; }

    bool active() const { return !is_done() && !cancelled_; }
    /// How far it is: 0 to 1.
    f32 progress() const { return 1.0f - static_cast<f32>(remaining_) / static_cast<f32>(ticks_); }
    EconRequest& request() { return request_; }
    const EconRequest& request() const { return request_; }
    void step() {
        if (active()) {
            --remaining_;
        }
    }

    void cancel() { cancelled_ = true; }

    /// The script's progress callback, callback(unit, progress), or LUA_NOREF.
    int callback_ref() const { return callback_ref_; }
    void set_callback_ref(int ref) { callback_ref_ = ref; }

    /// Registry ref to the script's handle table (LUA_NOREF if none): the
    /// handle's _c_object is nulled before the registry frees a finished
    /// event, since scripts RemoveEconomyEvent() it after WaitFor returns.
    int lua_table_ref() const { return lua_table_ref_; }
    void set_lua_table_ref(int ref) { lua_table_ref_ = ref; }
private:
    EntityId unit_id_{};
    f64 total_mass_ = 0.0;
    f64 total_energy_ = 0.0;
    f64 duration_ = 0.0;
    i32 ticks_ = 1;
    i32 remaining_ = 1;
    EconRequest request_;
    bool cancelled_ = false;
    int lua_table_ref_ = -2; // LUA_NOREF
    int callback_ref_ = -2;  // LUA_NOREF
};

/// Owns all active economy events.
class EconomyEventRegistry {
    friend struct StateIO; // snapshots (state_io.hpp)
public:
    EconomyEvent* create(EntityId unit_id, f64 mass, f64 energy, f64 duration) {
        auto evt = std::make_unique<EconomyEvent>(unit_id, mass, energy, duration);
        auto* ptr = evt.get();
        events_.push_back(std::move(evt));
        return ptr;
    }

    /// Remove completed/cancelled events.
    void gc() {
        events_.erase(
            std::remove_if(events_.begin(), events_.end(),
                [](const std::unique_ptr<EconomyEvent>& e) {
                    return !e || e->is_done() || e->is_cancelled();
                }),
            events_.end());
    }

    size_t count() const { return events_.size(); }

    /// Visit every event there was when the pass began. `fn` may run a
    /// script that makes events (a progress callback): they are appended,
    /// which may move the list, so the pass goes by index and leaves them
    /// for the next pass. Events stay in place, so `fn`'s reference holds.
    template<typename F>
    void for_each(F&& fn) {
        const size_t n = events_.size();
        for (size_t i = 0; i < n; ++i) {
            if (events_[i]) fn(*events_[i]);
        }
    }
    template <typename F> void for_each(F&& fn) const {
        for (const auto& evt : events_) {
            if (evt) fn(static_cast<const EconomyEvent&>(*evt));
        }
    }

private:
    std::vector<std::unique_ptr<EconomyEvent>> events_;
};

} // namespace osc::sim
