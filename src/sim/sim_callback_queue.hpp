#pragma once

#include "core/types.hpp"
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace osc::sim {

/// Func name of a UserUnit:ProcessInfo(action, value) request (Args Action,
/// Value; unit_ids = the unit). Handled by the engine, not SimCallbacks.lua.
inline constexpr const char* kProcessInfoCallback = "__osc_ProcessInfo";

/// Func name of the UI's unit-setting requests: SetPaused, SetFireState,
/// ToggleFireState, ToggleScriptBit, SetAutoMode and SetAutoSurfaceMode
/// (Args Setting, Value, and Bit for a script bit; the UI resolves a toggle
/// to the value it sets). Handled by the engine.
inline constexpr const char* kUnitSettingCallback = "__osc_UnitSetting";

/// Func name of DecreaseBuildCountInQueue(index, count) (Args Index, Count;
/// unit_ids = the factory). Handled by the engine.
inline constexpr const char* kDecreaseBuildCountCallback = "__osc_DecreaseBuildCount";

/// Func name of IncreaseBuildCountInQueue(index, count) (Args Index, Count;
/// unit_ids = the factory). Handled by the engine.
inline constexpr const char* kIncreaseBuildCountCallback = "__osc_IncreaseBuildCount";

/// Func name of a dropped player's defeat (Args Army, 0-based), which the
/// game loop decides between ticks and the sim applies in the next one.
inline constexpr const char* kDefeatArmyCallback = "__osc_DefeatArmy";

/// Func name of a network game's pause request (M218f): its command's
/// source asks (Moho's CMDST_RequestPause).
inline constexpr const char* kRequestPauseCallback = "__osc_RequestPause";

/// Func name of a loaded game's post-load: Moho's SimSync.SyncPlayableRect,
/// then the global OnPostLoad(), which re-send to the new UI what only the
/// sim remembers (campaign mode, objectives, transmissions...). A load
/// schedules it, so it is in the game's history and replays with it.
inline constexpr const char* kPostLoadCallback = "__osc_PostLoad";

/// Func name of Moho's CMDST_SetCommandTarget: order `Command` of the units
/// gets entity `Target`, or the point X, Y, Z (unit_ids = its units).
inline constexpr const char* kSetCommandTargetCallback = "__osc_SetCommandTarget";

/// Func name of Moho's CMDST_RemoveCommandFromQueue: order `Command` off
/// each named unit's queue (unit_ids).
inline constexpr const char* kRemoveCommandCallback = "__osc_RemoveCommand";

/// Func name of Moho's CMDST_SetCommandType, as the UI sends it: move order
/// `Command` of the named units (unit_ids) made a patrol (`Type`).
inline constexpr const char* kSetCommandTypeCallback = "__osc_SetCommandType";

/// The engine's own callbacks start with this; a script may not issue one.
inline constexpr const char* kEngineCallbackPrefix = "__osc_";

/// A SimCallback argument: FA's callbacks carry strings, numbers and bools.
using SimCallbackArg = std::variant<std::string, f64, bool>;

struct SimCallbackEntry {
    std::string func_name;
    // Args of the engine's own callbacks, and of a script's in replays
    // before v13. Ordered, so every peer builds the args table the same way.
    std::map<std::string, SimCallbackArg> args;
    // A script's Args given as one value, in replays v8 to v12.
    std::optional<SimCallbackArg> value;
    // Optional: selected unit entity IDs (when addUnitSelection=true)
    std::vector<u32> unit_ids;
    // A script's Args as lua_to_bytes wrote them (Moho's SCR_ToByteStream).
    std::optional<std::string> lua_args;
};

class SimCallbackQueue {
public:
    void push(SimCallbackEntry entry) {
        queue_.push_back(std::move(entry));
    }

    std::vector<SimCallbackEntry> drain() {
        std::vector<SimCallbackEntry> result;
        result.swap(queue_);
        return result;
    }

    bool empty() const { return queue_.empty(); }

private:
    std::vector<SimCallbackEntry> queue_;
};

} // namespace osc::sim
