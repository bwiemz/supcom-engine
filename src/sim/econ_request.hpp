#pragma once

#include "core/types.hpp"

#include <algorithm>

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
    void grant(f64 share) {
        const EconValue short_of = outstanding();
        const auto s = static_cast<f32>(share);
        granted.mass += short_of.mass * s;
        granted.energy += short_of.energy * s;
    }
    /// Moho's TakeGrantedResourcesAndReset, once the grants cover what it asks.
    bool take() {
        if (granted.mass < requested.mass || granted.energy < requested.energy) {
            return false;
        }
        granted = {};
        return true;
    }
};

} // namespace osc::sim
