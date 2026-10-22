#pragma once

#include "core/types.hpp"

#include <algorithm>
#include <optional>
#include <utility>

namespace osc::sim {

struct EconValue {
    f32 mass = 0.0f;
    f32 energy = 0.0f;
};

/// Moho's CEconRequest: a tick's amounts asked of the army's economy, and what it has granted so
/// far; each tick the economy grants a share of what is still short (ProcessArmyEconomy).
struct EconRequest {
    EconValue requested;
    EconValue granted;

    void ask(f64 mass, f64 energy) {
        requested = {static_cast<f32>(mass), static_cast<f32>(energy)};
        if (requested.mass <= 0.0f && requested.energy <= 0.0f) {
            granted = {};
        }
    }
    EconValue outstanding() const {
        return {std::max(0.0f, requested.mass - granted.mass),
                std::max(0.0f, requested.energy - granted.energy)};
    }
    EconValue grant(f32 share) {
        const EconValue short_of = outstanding();
        const EconValue given{short_of.mass * share, short_of.energy * share};
        granted.mass += given.mass;
        granted.energy += given.energy;
        return given;
    }
    /// Moho's TakeGrantedResourcesAndReset, once the grants cover what it asks.
    std::optional<EconValue> take() {
        if (granted.mass < requested.mass || granted.energy < requested.energy) {
            return std::nullopt;
        }
        return std::exchange(granted, {});
    }
    /// Moho's LimitingRate: the least share of what it asks its grants cover.
    f32 limiting_rate() const {
        f32 rate = 1.0f;
        if (requested.energy > 0.0f) {
            rate = std::min(rate, granted.energy / requested.energy);
        }
        if (requested.mass > 0.0f) {
            rate = std::min(rate, granted.mass / requested.mass);
        }
        return rate;
    }
    /// Moho's WithdrawFromGrantedEconValue: what it spends of its grants; the rest stays.
    EconValue withdraw(EconValue wanted) {
        const EconValue spent{std::min(wanted.mass, granted.mass),
                              std::min(wanted.energy, granted.energy)};
        granted = {std::max(0.0f, granted.mass - spent.mass),
                   std::max(0.0f, granted.energy - spent.energy)};
        return spent;
    }
};

} // namespace osc::sim
