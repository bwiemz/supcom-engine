#pragma once

#include "core/types.hpp"

namespace osc::sim {

class SimState;
class Unit;

/// The steering pass (M203c; Moho's CAiSteeringImpl). Each tick, before the
/// units move, every ground unit on a path looks about two seconds ahead for
/// units it would run into:
/// - it checks when it gets a path, and again as the 20 ticks it looked
///   ahead run out;
/// - at the tick of a meeting it steps aside to overtake a unit ahead going
///   its way, or stops to let one crossing or coming head-on pass.
/// Units and candidates are taken in id order, so every platform decides the
/// same.
void steer_ground_units(SimState& sim);

/// The formation a unit moves in, as a key (0: none): Moho's
/// mInfoCache.mFormationLayer (Unit::GetFormation, kept only for a form
/// command). A unit guarding, not an engineer and not off fighting for it,
/// is in its guarded unit's guard formation; one carrying out a slot of a
/// formation order, in that order's.
u64 formation_layer(const Unit& u);

/// Moho's Unit::IsSameFormationLayerWith: in one formation, and neither
/// attacking.
bool same_formation_layer(const Unit& a, const Unit& b);

/// Moho's Unit::IsHigherPriorityThan: whether `a` keeps its way when it
/// meets `b` (the other yields). The formation lead and priority order are
/// left out, as M204 keeps neither.
bool outranks(const Unit& a, const Unit& b);

} // namespace osc::sim
