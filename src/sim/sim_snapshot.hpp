#pragma once

// A snapshot of the sim between ticks: its C++ state (state_io.hpp) and its
// Lua heap (lpersist.h), with the pointers between them translated (M208c-b;
// design: docs/plans/2026-09-29-m208c-state-snapshots-design.md).
//
// Lua holds C++ addresses as light userdata: entities, weapons, navigators,
// manipulators, economy events, brains and platoons as `_c_object`, and the
// host's singletons (the sim, its thread manager, the VFS, ...) under
// registry and global names. A save names each by what it is; a load finds
// the object that name is in the restored sim, and the loading host's
// singleton under the same name. An address that names nothing live (a
// handle outliving its object) comes back NULL.

#include "core/types.hpp"

#include <string>
#include <vector>

namespace osc::sim {

class SimState;

/// Append a snapshot of `sim` (between ticks) to `out`. Empty on success,
/// else what failed.
std::string save_snapshot(SimState& sim, std::vector<u8>& out);

/// Replace `sim`, booted for the same game and not yet ticked, with a
/// snapshot's. Empty on success, else what failed: then the sim (and its
/// Lua state) must be discarded.
std::string load_snapshot(SimState& sim, const std::vector<u8>& in);

} // namespace osc::sim
