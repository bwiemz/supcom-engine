#pragma once

#include "core/types.hpp"
#include "sim/waitable.hpp"

#include <algorithm>
#include <vector>
#include <memory>

namespace osc::sim {

/// A resource drain that completes once it has drawn its whole cost (M206d).
/// It asks its unit's army for its cost spread over its duration, and moves
/// on as the economy grants it: a stall slows it. Used by teleports and by
/// weapons with EnergyRequired (OverCharge).
/// Pattern: CreateEconomyEvent → WaitFor → RemoveEconomyEvent
class EconomyEvent : public Waitable {
public:
    EconomyEvent(u32 unit_id, f64 mass_drain, f64 energy_drain, f64 duration)
        : unit_id_(unit_id)
        , total_mass_(mass_drain)
        , total_energy_(energy_drain)
        , duration_(duration)
    {}

    bool is_done() const override { return done_; }
    bool is_cancelled() const override { return cancelled_; }

    u32 unit_id() const { return unit_id_; }
    f64 total_mass() const { return total_mass_; }
    f64 total_energy() const { return total_energy_; }
    f64 duration() const { return duration_; }
    f64 elapsed() const { return elapsed_; }

    bool active() const { return !done_ && !cancelled_; }
    /// How far it is: 0 to 1.
    f64 progress() const { return progress_; }
    /// What it asks for per second: its cost over its duration (a tick at
    /// least).
    f64 mass_per_second() const { return total_mass_ / span(); }
    f64 energy_per_second() const { return total_energy_ / span(); }
    /// Move it on by `dt` seconds at the economy's `efficiency` (1: all it
    /// asked was granted). Done when its whole cost has been drawn.
    void advance(f64 dt, f64 efficiency) {
        if (!active()) return;
        elapsed_ += dt;
        progress_ = std::min(1.0, progress_ + dt / span() * efficiency);
        if (progress_ >= 1.0 - 1e-9) { // ten 0.1 s steps sum just short of 1
            progress_ = 1.0;
            done_ = true;
        }
    }
    /// Its request was counted in this tick's economy: it may move on.
    bool counted() const { return counted_; }
    void set_counted(bool v) { counted_ = v; }

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
    f64 span() const { return std::max(duration_, 0.1); }

    u32 unit_id_ = 0;
    f64 total_mass_ = 0.0;
    f64 total_energy_ = 0.0;
    f64 duration_ = 0.0;
    f64 elapsed_ = 0.0;
    f64 progress_ = 0.0;
    bool done_ = false;
    bool cancelled_ = false;
    bool counted_ = false;
    int lua_table_ref_ = -2; // LUA_NOREF
    int callback_ref_ = -2;  // LUA_NOREF
};

/// Owns all active economy events.
class EconomyEventRegistry {
public:
    EconomyEvent* create(u32 unit_id, f64 mass, f64 energy, f64 duration) {
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

private:
    std::vector<std::unique_ptr<EconomyEvent>> events_;
};

} // namespace osc::sim
