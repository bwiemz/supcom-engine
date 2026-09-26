#pragma once

#include "core/types.hpp"
#include <map>
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

/// A SimCallback argument: FA's callbacks carry strings, numbers and bools.
using SimCallbackArg = std::variant<std::string, f64, bool>;

struct SimCallbackEntry {
    std::string func_name;
    // Args: simple key→value map. Values can be string, number, or bool.
    // This covers the vast majority of FA SimCallback usage. Ordered, so
    // every peer builds the script's args table the same way.
    std::map<std::string, SimCallbackArg> args;
    // Optional: selected unit entity IDs (when addUnitSelection=true)
    std::vector<u32> unit_ids;
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
