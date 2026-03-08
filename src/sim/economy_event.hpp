#pragma once

#include "core/types.hpp"
#include "sim/waitable.hpp"

#include <vector>
#include <memory>

namespace osc::sim {

/// A temporary resource drain that completes after a set duration.
/// Used by teleport, weapon energy requirements, overcharge, remote viewing.
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

    /// Advance the event by dt seconds. Returns true when completed.
    bool tick(f64 dt) {
        if (done_ || cancelled_) return done_;
        elapsed_ += dt;
        if (duration_ <= 0.0 || elapsed_ >= duration_) {
            done_ = true;
        }
        return done_;
    }

    /// Per-tick resource drain amounts (proportional to dt/duration).
    f64 mass_drain_per_tick(f64 dt) const {
        if (duration_ <= 0.0) return total_mass_;
        return total_mass_ * (dt / duration_);
    }
    f64 energy_drain_per_tick(f64 dt) const {
        if (duration_ <= 0.0) return total_energy_;
        return total_energy_ * (dt / duration_);
    }

    void cancel() { cancelled_ = true; }

private:
    u32 unit_id_ = 0;
    f64 total_mass_ = 0.0;
    f64 total_energy_ = 0.0;
    f64 duration_ = 0.0;
    f64 elapsed_ = 0.0;
    bool done_ = false;
    bool cancelled_ = false;
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

    /// Tick all events, returns list of newly-completed events for thread waking.
    void tick(f64 dt) {
        for (auto& evt : events_) {
            if (evt && !evt->is_done() && !evt->is_cancelled()) {
                evt->tick(dt);
            }
        }
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

    template<typename F>
    void for_each(F&& fn) {
        for (auto& evt : events_) {
            if (evt) fn(*evt);
        }
    }

private:
    std::vector<std::unique_ptr<EconomyEvent>> events_;
};

} // namespace osc::sim
