#pragma once

#include "sim/entity.hpp"

#include <array>
#include <vector>

namespace osc::sim {

class Prop : public Entity {
public:
    bool is_prop() const override { return true; }

    /// Its blueprint is UNTARGETABLE (a deposit's marker): no click picks it.
    bool untargetable = false;
    /// Its blueprint is RECLAIMABLE: a Reclaim order may take it.
    bool reclaimable_category = false;
    bool obstructs_building = false;
    /// Its blueprint's Economy.ReclaimMassMax and ReclaimEnergyMax.
    f32 reclaim_mass_max = 0;
    f32 reclaim_energy_max = 0;

    /// SinkAway: how fast the prop sinks into the ground (units/s, <= 0).
    f32 sink_rate = 0;

    /// TryCopyPose's copy of a unit's skinning matrices: a wreck keeps the
    /// pose its unit died in. Empty draws the mesh at rest.
    std::vector<std::array<f32, 16>> pose;

    i32 bounded_priority = 0;
    i32 bounded_tick = 0;
    i32 bounded_handle = -1;
};

} // namespace osc::sim
