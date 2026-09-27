#pragma once

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

/// Moho's Unit::IsHigherPriorityThan: whether `a` keeps its way when it
/// meets `b` (the other yields). The formation lead and priority order are
/// left out, as M204 keeps neither.
bool outranks(const Unit& a, const Unit& b);

} // namespace osc::sim
