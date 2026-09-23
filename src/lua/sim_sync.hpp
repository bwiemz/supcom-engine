#pragma once

#include "core/lua_copy.hpp"

struct lua_State;

namespace osc::lua {

/// Registry key (UI state) of a pending SetFocusArmy request: 0-based army
/// or -1 for observer. While a world session runs, the UI's SetFocusArmy is
/// a request the next sim beat applies, as Moho's CWldSession does.
inline constexpr const char* kFocusArmyRequestKey = "__osc_focus_army_request";

/// Plain-data deep copy between states (see core/lua_copy.hpp).
using core::copy_lua_value;

/// One Moho sim beat on the sim -> user channel:
///  1. apply a pending focus-army request: both states' focus army change,
///     the sim's NoteFocusArmyChanged(new, old) runs and Sync.FocusArmyChanged
///     is set (1-based armies, -1 = observer);
///  2. the sim's Sync table is copied into the user state's Sync, the
///     previous one kept as PreviousSync;
///  3. the sim's ResetSyncTable() starts the next beat's table;
///  4. the user state's OnSync() reacts (/lua/UserSync.lua plus hooks).
/// Script errors are logged and count as failures in test modes.
void sync_beat(lua_State* sim_L, lua_State* ui_L);

} // namespace osc::lua
